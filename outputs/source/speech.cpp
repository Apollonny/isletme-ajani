#include "ajan.hpp"
#include <cmath>
#include <condition_variable>
#include <deque>
#include <onnxruntime_cxx_api.h>
#include <random>
#include <regex>
#include <sherpa-onnx/c-api/c-api.h>
#include <whisper.h>
std::vector<float> PcmResampler::push(const float *samples, size_t count, int rate) {
    if (rate <= 0 || target <= 0)
        throw std::runtime_error("Invalid PCM sample rate");
    if (rate != source) {
        source = rate;
        phase = 0;
        previous = filtered = 0;
    }
    std::vector<float> out;
    out.reserve(count * (uint64_t)target / rate + 2);
    float alpha = rate > target ? 1.f - std::exp(-2.f * 3.14159265f * (target * .4f) / rate) : 1.f;
    for (size_t i = 0; i < count; i++) {
        filtered += alpha * (samples[i] - filtered);
        phase += target;
        while (phase >= source) {
            phase -= source;
            float fraction = 1.f - (float)phase / target;
            out.push_back(previous + fraction * (filtered - previous));
        }
        previous = filtered;
    }
    return out;
}
struct Speech::Impl {
    fs::path models;
    J config;
    J last_stats = J::object();
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "YerelAjan"};
    Ort::SessionOptions opts;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::unique_ptr<Ort::Session> text, sound, decoder, vad_model;
    whisper_context *whisper = nullptr;
    std::map<wchar_t, int64_t> vocab;
    std::vector<float> state = std::vector<float>(256, 0), context = std::vector<float>(64, 0);
    std::mutex lock;
    const SherpaOnnxOfflineTts *english = nullptr;
    HMODULE normalizer = nullptr;
    char *(*normalize)(const char *) = nullptr;
    void (*release)(char *) = nullptr;
    Impl(fs::path p, J c) : models(p), config(c) {
        env.DisableTelemetryEvents();
        opts.SetIntraOpNumThreads(c.value("tts_threads", 2));
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        opts.DisableMemPattern();
        if (c.value("voice_language", "tr") == "tr")
            load_tr();
        normalizer = LoadLibraryW(L"ajan_normalizer.dll");
        if (!normalizer)
            throw std::runtime_error("Turkish normalizer DLL missing");
        normalize = (char *(*)(const char *))GetProcAddress(normalizer, "ajan_normalize");
        release = (void (*)(char *))GetProcAddress(normalizer, "ajan_normalize_free");
        if (!normalize || !release)
            throw std::runtime_error("Normalizer ABI mismatch");
        vad_model = std::make_unique<Ort::Session>(
            env, fs::u8path(c.value("vad_model", (p / L"silero-vad.onnx").u8string())).c_str(),
            opts);
    }
    void load_tr() {
        auto ema = fs::u8path(config.value("tts_tr", (models / "ema").u8string()));
        text = std::make_unique<Ort::Session>(env, (ema / L"text.onnx").c_str(), opts);
        sound = std::make_unique<Ort::Session>(env, (ema / L"sound.onnx").c_str(), opts);
        decoder = std::make_unique<Ort::Session>(env, (ema / L"decoder.onnx").c_str(), opts);
        auto m = read_json(ema / L"meta.json");
        for (size_t i = 0; i < m["vocab"].size(); i++) {
            auto w = wide(m["vocab"][i]);
            if (w.size() == 1)
                vocab[w[0]] = i;
        }
    }
    ~Impl() {
        if (english)
            SherpaOnnxDestroyOfflineTts(english);
        if (whisper)
            whisper_free(whisper);
        if (normalizer)
            FreeLibrary(normalizer);
    }
    template <class T> Ort::Value tensor(std::vector<T> &v, std::vector<int64_t> dims) {
        return Ort::Value::CreateTensor<T>(mem, v.data(), v.size(), dims.data(), dims.size());
    }
};
Speech::Speech(fs::path p, J c) : impl(new Impl(p, c)) {}
Speech::~Speech() = default;
J Speech::stats() const {
    return impl->last_stats;
}
static std::vector<std::wstring> pieces(std::wstring s, size_t first_limit = 72,
                                        size_t next_limit = 140) {
    std::vector<std::wstring> r;
    while (!s.empty()) {
        while (!s.empty() && iswspace(s[0]))
            s.erase(0, 1);
        if (s.empty())
            break;
        size_t limit = r.empty() ? first_limit : next_limit;
        size_t n = std::min(limit, s.size());
        if (n < s.size()) {
            size_t cut = s.find_last_of(L".!?;:, ", n);
            if (cut != std::wstring::npos && cut > 0)
                n = cut + 1;
        }
        auto p = s.substr(0, n);
        s.erase(0, n);
        if (p.find_last_not_of(L" \t\r\n.!?;:,") != std::wstring::npos) {
            while (!p.empty() && iswspace(p.back()))
                p.pop_back();
            if (p.back() != L'.' && p.back() != L'!' && p.back() != L'?')
                p += L'.';
            r.push_back(p);
        }
    }
    return r;
}
void Speech::stream(std::string input, std::string lang, Sink sink, std::atomic<bool> &cancel,
                    bool cache_safe) {
    std::lock_guard<std::mutex> guard(impl->lock);
    impl->last_stats = {{"normalizer_ms", 0.},
                        {"text_ms", 0.},
                        {"acoustic_ms", 0.},
                        {"decoder_ms", 0.},
                        {"cache_hit", false}};
    if (input.size() > 8192)
        throw std::runtime_error("TTS input limit");
    bool cache_allowed = cache_safe && lang == "tr" && input.size() < 1024;
    fs::path cache_path;
    std::vector<float> cached;
    if (cache_allowed) {
        auto ema = fs::u8path(impl->config.value("tts_tr", (impl->models / "ema").u8string()));
        std::string revision = "ema-native-v2";
        for (auto file : {"text.onnx", "sound.onnx", "decoder.onnx"})
            revision += std::to_string(
                (long long)fs::last_write_time(ema / file).time_since_epoch().count());
        auto key = std::hash<std::string>{}(revision + lang + input);
        cache_path = fs::path("data/voice-cache") / (std::to_string(key) + ".wav");
        if (fs::exists(cache_path)) {
            impl->last_stats["cache_hit"] = true;
            int rate;
            auto x = load_wav(cache_path, rate);
            if (!cancel)
                sink(x.data(), x.size(), rate);
            return;
        }
        auto original = sink;
        sink = [&, original](const float *x, size_t n, int rate) {
            if (cache_allowed) {
                if (cached.size() + n <= 1000000)
                    cached.insert(cached.end(), x, x + n);
                else {
                    cache_allowed = false;
                    cached.clear();
                }
            }
            original(x, n, rate);
        };
    }
    if (lang == "en") {
        impl->text.reset();
        impl->sound.reset();
        impl->decoder.reset();
        if (!impl->english) {
            auto p = fs::u8path(impl->config.value(
                "tts_en", (impl->models / "kokoro-int8-multi-lang-v1_0").u8string()));
            std::string model = (p / "model.int8.onnx").string(),
                        voices = (p / "voices.bin").string(), tokens = (p / "tokens.txt").string(),
                        data = (p / "espeak-ng-data").string(),
                        lexicon = (p / "lexicon-us-en.txt").string();
            SherpaOnnxOfflineTtsConfig c{};
            c.model.kokoro.model = model.c_str();
            c.model.kokoro.voices = voices.c_str();
            c.model.kokoro.tokens = tokens.c_str();
            c.model.kokoro.data_dir = data.c_str();
            c.model.kokoro.lexicon = lexicon.c_str();
            c.model.kokoro.lang = "en-us";
            c.model.kokoro.length_scale = 1;
            c.model.num_threads = impl->config.value("english_tts_threads", 4);
            c.model.provider = "cpu";
            c.max_num_sentences = 1;
            impl->english = SherpaOnnxCreateOfflineTts(&c);
            if (!impl->english)
                throw std::runtime_error("Kokoro load failed");
        }
        for (auto &piece : pieces(wide(input), 14, 28)) {
            if (cancel)
                return;
            auto s = utf8(piece);
            auto *a = SherpaOnnxOfflineTtsGenerate(impl->english, s.c_str(), 3, 1.f);
            if (!a)
                throw std::runtime_error("Kokoro synthesis failed");
            sink(a->samples, a->n, a->sample_rate);
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
        }
        return;
    }
    if (impl->english) {
        SherpaOnnxDestroyOfflineTts(impl->english);
        impl->english = nullptr;
    }
    if (!impl->text)
        impl->load_tr();
    input =
        std::regex_replace(input, std::regex("([0-9]{4}-[0-9]{2}-[0-9]{2}) ([0-9]{1,2}:[0-9]{2})"),
                           "tarih $1, saat $2");
    input =
        std::regex_replace(input, std::regex("([0-9]{1,2}:[0-9]{2})(?:–|-)([0-9]{1,2}:[0-9]{2})"),
                           "saat $1 ile saat $2");
    std::regex phones("\\+90 [0-9]{3} [0-9]{3} [0-9]{2} [0-9]{2}");
    std::smatch phone;
    std::string rewritten, phoneRest = input;
    const char *digits[] = {"sıfır", "bir",  "iki",  "üç",    "dört",
                            "beş",   "altı", "yedi", "sekiz", "dokuz"};
    while (std::regex_search(phoneRest, phone, phones)) {
        rewritten += phone.prefix().str();
        for (char c : phone[0].str()) {
            if (c == '+')
                rewritten += "artı ";
            else if (c >= '0' && c <= '9') {
                rewritten += digits[c - '0'];
                rewritten += " ";
            }
        }
        phoneRest = phone.suffix().str();
    }
    input = rewritten + phoneRest;
    std::regex codes("(?:Randevu kodu|randevu kodu|Kod|kod): ([A-F0-9]{8})");
    std::string spoken;
    std::smatch match;
    std::string remaining = input;
    const char *letters[] = {"sıfır", "bir",   "iki", "üç", "dört", "beş", "altı", "yedi",
                             "sekiz", "dokuz", "a",   "be", "ce",   "de",  "e",    "ef"};
    while (std::regex_search(remaining, match, codes)) {
        spoken += match.prefix().str();
        spoken += match[0].str().substr(0, (size_t)(match.position(1) - match.position(0)));
        for (char c : match[1].str()) {
            int n = c <= '9' ? c - '0' : c - 'A' + 10;
            spoken += letters[n];
            spoken += " ";
        }
        remaining = match.suffix().str();
    }
    spoken += remaining;
    double normalizedStarted = clock_ms();
    char *normalized = impl->normalize(spoken.c_str());
    if (!normalized)
        throw std::runtime_error("Ambiguous text normalization: clarify the value");
    std::string safe(normalized);
    impl->release(normalized);
    impl->last_stats["normalizer_ms"] = clock_ms() - normalizedStarted;
    auto all = pieces(wide(lower(safe)));
    size_t pieceNo = 0;
    for (auto piece : all) {
        if (cancel)
            return;
        std::vector<int64_t> ids, cw;
        std::vector<int> wstart;
        std::wstring clean;
        for (auto c : piece)
            clean += impl->vocab.count(c) ? c : L' ';
        while (!clean.empty() && clean.front() == L' ')
            clean.erase(0, 1);
        if (clean.empty())
            continue;
        int word = 0, start = 0;
        for (size_t i = 0; i < clean.size(); i++) {
            if (i && clean[i] != L' ' && clean[i - 1] == L' ') {
                word++;
                start = (int)i;
            }
            cw.push_back(word);
            wstart.push_back(start);
            ids.push_back(impl->vocab[clean[i]]);
        }
        size_t L = ids.size();
        auto it = impl->tensor(ids, {1, (int64_t)L});
        const char *in[] = {"ids"};
        const char *out[] = {"h", "dur"};
        double encodedStarted = clock_ms();
        auto tr = impl->text->Run(Ort::RunOptions{nullptr}, in, &it, 1, out, 2);
        impl->last_stats["text_ms"] =
            impl->last_stats["text_ms"].get<double>() + clock_ms() - encodedStarted;
        auto *dur = tr[1].GetTensorData<float>();
        int nw = word + 1;
        std::vector<double> sums(nw, 0);
        for (size_t i = 0; i < L; i++)
            sums[cw[i]] += dur[i];
        std::vector<int> counts(nw);
        int T = 0;
        for (int w = 0; w < nw; w++) {
            counts[w] = std::clamp((int)std::floor(sums[w] + .5), 1, 250);
            T += counts[w];
        }
        if (T > 1000)
            throw std::runtime_error("TTS duration exceeded bounded window");
        std::vector<int64_t> fw(T);
        std::vector<float> fp(T), cg(L), fg(T), c(L), done(L), total(nw, 0), wlen(nw, 0),
            woff(nw, 0);
        int f = 0;
        for (int w = 0; w < nw; w++)
            for (int j = 0; j < counts[w]; j++, f++) {
                fw[f] = w;
                fp[f] = (float)j / counts[w];
            }
        float acc = 0;
        for (size_t i = 0; i < L; i++) {
            c[i] = std::max(dur[i], 1e-4f);
            acc += c[i];
            done[i] = acc;
            total[cw[i]] += c[i];
            wlen[cw[i]]++;
        }
        for (int w = 1; w < nw; w++)
            woff[w] = woff[w - 1] + std::max(wlen[w - 1], 1.f);
        for (size_t i = 0; i < L; i++) {
            float before = done[wstart[i]] - c[wstart[i]];
            float cp = std::clamp((done[i] - before - .5f * c[i]) / std::max(total[cw[i]], 1e-8f),
                                  0.f, 1.f);
            cg[i] = woff[cw[i]] + cp * std::max(wlen[cw[i]], 1.f);
        }
        for (int j = 0; j < T; j++)
            fg[j] = woff[fw[j]] + fp[j] * std::max(wlen[fw[j]], 1.f);
        std::mt19937 rng(42 + (unsigned)pieceNo++);
        std::normal_distribution<float> gaussian(0, 1);
        std::vector<float> noise(4 * T * 64);
        for (auto &n : noise)
            n = gaussian(rng);
        std::vector<Ort::Value> sv;
        sv.push_back(std::move(tr[0]));
        sv.push_back(impl->tensor(cg, {1, (int64_t)L}));
        sv.push_back(impl->tensor(fg, {1, T}));
        sv.push_back(impl->tensor(cw, {1, (int64_t)L}));
        sv.push_back(impl->tensor(fw, {1, T}));
        sv.push_back(impl->tensor(noise, {1, 4, T, 64}));
        const char *sin[] = {"h", "cg", "fg", "cw", "fw", "noise"};
        const char *sout[] = {"latents"};
        double acousticStarted = clock_ms();
        auto sr = impl->sound->Run(Ort::RunOptions{nullptr}, sin, sv.data(), 6, sout, 1);
        impl->last_stats["acoustic_ms"] =
            impl->last_stats["acoustic_ms"].get<double>() + clock_ms() - acousticStarted;
        auto lat = sr[0].GetTensorData<float>();
        std::vector<float> z;
        for (int s = 0; s < T;) {
            if (cancel)
                return;
            int e = std::min(T, s + (s == 0 ? 25 : 100)), a = std::max(0, s - 8),
                b = std::min(T, e + 8), n = b - a;
            z.resize(64 * n);
            for (int d = 0; d < 64; d++)
                for (int j = 0; j < n; j++)
                    z[d * n + j] = lat[(a + j) * 64 + d];
            auto zt = impl->tensor(z, {1, 64, n});
            const char *din[] = {"z"};
            const char *dout[] = {"audio"};
            double decoderStarted = clock_ms();
            auto dr = impl->decoder->Run(Ort::RunOptions{nullptr}, din, &zt, 1, dout, 1);
            impl->last_stats["decoder_ms"] =
                impl->last_stats["decoder_ms"].get<double>() + clock_ms() - decoderStarted;
            auto wave = dr[0].GetTensorData<float>();
            sink(wave + (s - a) * 1920, (e - s) * 1920, 48000);
            s = e;
        }
        if (pieceNo < all.size()) {
            std::vector<float> pause(4800, 0);
            sink(pause.data(), pause.size(), 48000);
        }
    }
    if (cache_allowed && !cancel && !cached.empty()) {
        auto temporary = cache_path;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        save_wav(temporary, cached, 48000);
        MoveFileExW(temporary.c_str(), cache_path.c_str(), MOVEFILE_REPLACE_EXISTING);
        std::vector<fs::directory_entry> files;
        uint64_t bytes = 0;
        for (auto &f : fs::directory_iterator(cache_path.parent_path()))
            if (f.is_regular_file() && f.path().extension() == L".wav") {
                bytes += f.file_size();
                files.push_back(f);
            }
        std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
            return a.last_write_time() < b.last_write_time();
        });
        for (auto &f : files) {
            if (bytes <= 32 * 1024 * 1024)
                break;
            bytes -= f.file_size();
            fs::remove(f.path());
        }
    }
}
std::string Speech::transcribe(const std::vector<float> &audio, std::string lang) {
    if (audio.empty() || audio.size() > 16 * 16000)
        throw std::runtime_error("ASR utterance length limit");
    if (!impl->whisper) {
        auto cp = whisper_context_default_params();
        cp.use_gpu = false;
        impl->whisper = whisper_init_from_file_with_params(
            impl->config.value("stt_model", (impl->models / L"whisper-base-q5.bin").u8string())
                .c_str(),
            cp);
        if (!impl->whisper)
            throw std::runtime_error("STT model load failed");
    }
    auto p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = impl->config.value("stt_threads", 3);
    p.language = lang.c_str();
    p.translate = false;
    p.no_context = true;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.print_special = false;
    p.audio_ctx = impl->config.value("bounded_stt_context", true)
                      ? std::min(1500, std::max(256, ((int)audio.size() / 320 / 64 + 2) * 64))
                      : 0;
    p.temperature = 0;
    p.temperature_inc = 0;
    p.suppress_blank = true;
    p.suppress_nst = true;
    if (whisper_full(impl->whisper, p, audio.data(), (int)audio.size()))
        throw std::runtime_error("STT decode failed");
    std::string out;
    for (int i = 0; i < whisper_full_n_segments(impl->whisper); i++)
        out += whisper_full_get_segment_text(impl->whisper, i);
    return out;
}
bool Speech::vad(const float *audio, size_t n) {
    if (n != 512)
        throw std::runtime_error("VAD requires 512 samples");
    std::vector<float> input(576);
    std::copy(impl->context.begin(), impl->context.end(), input.begin());
    std::copy(audio, audio + n, input.begin() + 64);
    std::copy(audio + n - 64, audio + n, impl->context.begin());
    std::vector<int64_t> sr = {16000};
    std::vector<Ort::Value> iv;
    iv.push_back(impl->tensor(input, {1, 576}));
    iv.push_back(impl->tensor(impl->state, {2, 1, 128}));
    iv.push_back(impl->tensor(sr, {}));
    const char *in[] = {"input", "state", "sr"};
    const char *out[] = {"output", "stateN"};
    auto r = impl->vad_model->Run(Ort::RunOptions{nullptr}, in, iv.data(), 3, out, 2);
    std::copy_n(r[1].GetTensorData<float>(), 256, impl->state.begin());
    return r[0].GetTensorData<float>()[0] > .5f;
}
void Speech::reset_vad() {
    std::fill(impl->state.begin(), impl->state.end(), 0);
    std::fill(impl->context.begin(), impl->context.end(), 0);
}
void save_wav(fs::path p, const std::vector<float> &audio, int rate) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    uint32_t n = (uint32_t)audio.size() * 2, total = n + 36, fmt = 16, bytes = rate * 2;
    uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    f.write("RIFF", 4);
    f.write((char *)&total, 4);
    f.write("WAVEfmt ", 8);
    f.write((char *)&fmt, 4);
    f.write((char *)&pcm, 2);
    f.write((char *)&channels, 2);
    f.write((char *)&rate, 4);
    f.write((char *)&bytes, 4);
    f.write((char *)&align, 2);
    f.write((char *)&bits, 2);
    f.write("data", 4);
    f.write((char *)&n, 4);
    for (float x : audio) {
        int16_t v = (int16_t)std::lround(std::clamp(x, -1.f, 1.f) * 32767);
        f.write((char *)&v, 2);
    }
}
std::vector<float> load_wav(fs::path p, int &rate) {
    std::ifstream f(p, std::ios::binary);
    char id[4];
    uint32_t n;
    f.read(id, 4);
    if (std::string(id, 4) != "RIFF")
        throw std::runtime_error("PCM WAV required");
    f.read((char *)&n, 4);
    f.read(id, 4);
    uint16_t fmt = 0, ch = 0, bits = 0;
    std::vector<float> out;
    while (f.read(id, 4)) {
        f.read((char *)&n, 4);
        if (std::string(id, 4) == "fmt ") {
            std::vector<char> b(n);
            f.read(b.data(), n);
            memcpy(&fmt, b.data(), 2);
            memcpy(&ch, b.data() + 2, 2);
            memcpy(&rate, b.data() + 4, 4);
            memcpy(&bits, b.data() + 14, 2);
        } else if (std::string(id, 4) == "data") {
            if (fmt != 1 || bits != 16 || ch != 1)
                throw std::runtime_error("16-bit mono WAV required");
            out.resize(n / 2);
            for (auto &v : out) {
                int16_t x;
                f.read((char *)&x, 2);
                v = x / 32768.f;
            }
            break;
        } else
            f.seekg(n + (n & 1), std::ios::cur);
    }
    return out;
}
void play_pcm(const float *x, size_t n, int rate, std::atomic<bool> &cancel) {
    if (cancel || !n)
        return;
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, (DWORD)rate, (DWORD)rate * 2, 2, 16, 0};
    HWAVEOUT h;
    if (waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        throw std::runtime_error("Audio output unavailable");
    std::vector<int16_t> b(n);
    for (size_t i = 0; i < n; i++)
        b[i] = (int16_t)(std::clamp(x[i], -1.f, 1.f) * 32767);
    WAVEHDR hdr{};
    hdr.lpData = (char *)b.data();
    hdr.dwBufferLength = (DWORD)b.size() * 2;
    waveOutPrepareHeader(h, &hdr, sizeof hdr);
    waveOutWrite(h, &hdr, sizeof hdr);
    while (!(hdr.dwFlags & WHDR_DONE) && !cancel)
        Sleep(5);
    if (cancel)
        waveOutReset(h);
    waveOutUnprepareHeader(h, &hdr, sizeof hdr);
    waveOutClose(h);
}
void microphone(Speech &speech, std::function<void(std::vector<float>)> done,
                std::atomic<bool> &stop, std::function<void()> speech_started) {
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    HWAVEIN h;
    if (waveInOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        throw std::runtime_error("Microphone unavailable");
    std::vector<std::vector<int16_t>> buffers(8, std::vector<int16_t>(512));
    std::vector<WAVEHDR> headers(8);
    for (int i = 0; i < 8; i++) {
        headers[i].lpData = (char *)buffers[i].data();
        headers[i].dwBufferLength = 1024;
        waveInPrepareHeader(h, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(h, &headers[i], sizeof(WAVEHDR));
    }
    waveInStart(h);
    std::vector<float> utterance, pre;
    int silence = 0;
    bool talking = false;
    while (!stop) {
        for (int i = 0; i < 8; i++)
            if (headers[i].dwFlags & WHDR_DONE) {
                std::vector<float> x(512);
                for (int j = 0; j < 512; j++)
                    x[j] = buffers[i][j] / 32768.f;
                bool active = speech.vad(x.data(), 512);
                if (active && !talking) {
                    if (speech_started)
                        speech_started();
                    talking = true;
                    utterance = pre;
                    silence = 0;
                }
                if (talking) {
                    utterance.insert(utterance.end(), x.begin(), x.end());
                    silence = active ? 0 : silence + 512;
                    if (silence >= 4096 || utterance.size() >= 16 * 16000) {
                        if (utterance.size() > 4096)
                            done(utterance);
                        utterance.clear();
                        talking = false;
                        silence = 0;
                        speech.reset_vad();
                    }
                }
                pre.insert(pre.end(), x.begin(), x.end());
                if (pre.size() > 4096)
                    pre.erase(pre.begin(), pre.end() - 4096);
                headers[i].dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(h, &headers[i], sizeof(WAVEHDR));
            }
        Sleep(5);
    }
    waveInStop(h);
    waveInReset(h);
    for (auto &hdr : headers)
        waveInUnprepareHeader(h, &hdr, sizeof hdr);
    waveInClose(h);
}
