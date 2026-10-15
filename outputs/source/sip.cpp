#include "ajan.hpp"
#include <condition_variable>
#include <deque>
#include <iostream>
#include <pjsua2.hpp>
using namespace pj;
struct VoicePort : AudioMediaPort {
    std::mutex mutex;
    std::deque<float> incoming;
    std::deque<int16_t> outgoing;
    std::condition_variable ready;
    std::atomic<bool> stop = false, cancel = false;
    std::atomic<unsigned> epoch = 0;
    std::atomic<bool> english = false;
    std::shared_ptr<Speech> speech;
    std::thread reader, dialog, staff;
    std::string sender;
    std::function<void()> handoff;
    VoicePort(std::string who, std::shared_ptr<Speech> warmed, std::function<void()> transfer,
              bool en = false)
        : english(en), speech(std::move(warmed)), sender(who), handoff(transfer) {
        speech->reset_vad();
        try {
            pipe_request({{"op", "reset_session"}, {"session", "sip:" + who}});
        } catch (...) {
        }
        MediaFormatAudio f;
        f.init(PJMEDIA_FORMAT_PCM, 16000, 1, 20000, 16);
        createPort("YerelAjanPCM", f);
        reader = std::thread([this] { run(); });
        staff = std::thread([this] {
            while (!stop) {
                try {
                    auto pending =
                        pipe_request({{"op", "outbox"}, {"channel", "sip"}, {"sender", sender}});
                    for (auto &m : pending.value("messages", J::array())) {
                        auto gen = epoch.load();
                        cancel = false;
                        say(m.at("text"), "tr", gen);
                        pipe_request({{"op", "outbox_ack"},
                                      {"channel", "sip"},
                                      {"id", m.at("id")},
                                      {"state", cancel.load() ? "interrupted" : "sent"}});
                    }
                } catch (...) {
                }
                for (int i = 0; i < 25 && !stop; i++)
                    Sleep(20);
            }
        });
    }
    ~VoicePort() {
        stop = true;
        cancel = true;
        ready.notify_all();
        if (reader.joinable())
            reader.join();
        if (dialog.joinable())
            dialog.join();
        if (staff.joinable())
            staff.join();
    }
    void onFrameReceived(MediaFrame &f) override {
        if (f.type != PJMEDIA_FRAME_TYPE_AUDIO)
            return;
        std::lock_guard<std::mutex> g(mutex);
        auto *p = (int16_t *)f.buf.data();
        for (size_t i = 0; i < f.buf.size() / 2; i++) {
            if (incoming.size() < 32000)
                incoming.push_back(p[i] / 32768.f);
        }
        ready.notify_one();
    }
    void onFrameRequested(MediaFrame &f) override {
        f.type = PJMEDIA_FRAME_TYPE_AUDIO;
        f.buf.resize(f.size);
        auto *p = (int16_t *)f.buf.data();
        std::lock_guard<std::mutex> g(mutex);
        for (size_t i = 0; i < f.size / 2; i++) {
            p[i] = outgoing.empty() ? 0 : outgoing.front();
            if (!outgoing.empty())
                outgoing.pop_front();
        }
    }
    void say(std::string text, std::string lang, unsigned generation, bool cache_safe = false) {
        PcmResampler resampler(16000);
        speech->stream(
            text, lang,
            [&](const float *x, size_t n, int sr) {
                auto pcm = resampler.push(x, n, sr);
                x = pcm.data();
                n = pcm.size();
                for (size_t at = 0; at < n;) {
                    if (stop || epoch != generation) {
                        cancel = true;
                        return;
                    }
                    std::unique_lock<std::mutex> g(mutex);
                    if (outgoing.size() > 32000) {
                        g.unlock();
                        Sleep(5);
                        continue;
                    }
                    size_t e = std::min(n, at + 960);
                    for (size_t i = at; i < e; i++)
                        outgoing.push_back((int16_t)(std::clamp(x[i], -1.f, 1.f) * 32767));
                    at = e;
                }
            },
            cancel, cache_safe);
    }
    void run() {
        std::vector<float> utterance, pre;
        int silent = 0;
        bool talking = false;
        while (!stop) {
            std::vector<float> x(512);
            {
                std::unique_lock<std::mutex> g(mutex);
                ready.wait_for(g, std::chrono::milliseconds(50),
                               [&] { return incoming.size() >= 512 || stop; });
                if (stop)
                    break;
                if (incoming.size() < 512)
                    continue;
                for (auto &v : x) {
                    v = incoming.front();
                    incoming.pop_front();
                }
            }
            try {
                bool active = speech->vad(x.data(), 512);
                if (active && !talking) {
                    talking = true;
                    utterance = pre;
                    silent = 0;
                    ++epoch;
                    cancel = true;
                    std::lock_guard<std::mutex> g(mutex);
                    outgoing.clear();
                }
                if (talking) {
                    utterance.insert(utterance.end(), x.begin(), x.end());
                    silent = active ? 0 : silent + 512;
                    if (silent >= 4096 || utterance.size() >= 16 * 16000) {
                        talking = false;
                        if (dialog.joinable())
                            dialog.join();
                        auto audio = utterance;
                        auto gen = epoch.load();
                        dialog = std::thread([this, audio, gen] {
                            try {
                                auto text = speech->transcribe(audio, english ? "en" : "tr");
                                if (text.empty() || stop || epoch != gen)
                                    return;
                                auto r = pipe_request({{"channel", "sip"},
                                                       {"sender", sender},
                                                       {"id", std::to_string((uint64_t)clock_ms())},
                                                       {"text", text}});
                                if (stop || epoch != gen)
                                    return;
                                cancel = false;
                                english = r.value("language", "tr") == "en";
                                say(r.value("text", ""), r.value("language", "tr"), gen,
                                    r.contains("evidence") && r["evidence"].is_array() &&
                                        !r["evidence"].empty());
                                if (r.value("handoff", false) && handoff)
                                    handoff();
                            } catch (const std::exception &e) {
                                std::cerr << "Voice turn: " << e.what() << "\n";
                            }
                        });
                        utterance.clear();
                        silent = 0;
                        speech->reset_vad();
                    }
                }
                pre.insert(pre.end(), x.begin(), x.end());
                if (pre.size() > 4096)
                    pre.erase(pre.begin(), pre.end() - 4096);
            } catch (const std::exception &e) {
                std::cerr << "Audio: " << e.what() << "\n";
            }
        }
    }
};
struct AgentCall : Call {
    std::unique_ptr<VoicePort> port;
    J cfg;
    std::shared_ptr<Speech> speech;
    std::atomic<bool> ended = false;
    AgentCall(Account &a, int id, J c, std::shared_ptr<Speech> warmed)
        : Call(a, id), cfg(c), speech(std::move(warmed)) {}
    void onCallState(OnCallStateParam &) override {
        if (getInfo().state == PJSIP_INV_STATE_DISCONNECTED) {
            ended = true;
            if (port) {
                port->stop = true;
                port->cancel = true;
            }
        }
    }
    void onCallMediaState(OnCallMediaStateParam &) override {
        auto ci = getInfo();
        if (ci.state == PJSIP_INV_STATE_DISCONNECTED)
            return;
        for (unsigned i = 0; i < ci.media.size(); i++)
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO &&
                ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
                if (!port) {
                    port = std::make_unique<VoicePort>(
                        ci.remoteUri, speech,
                        [this] {
                            auto uri = cfg["channels"]["sip"].value("staff_uri", "");
                            if (!uri.empty()) {
                                CallOpParam p;
                                xfer(uri, p);
                            }
                        },
                        cfg["channels"]["sip"].value("language", "tr") == "en");
                }
                auto media = getAudioMedia((int)i);
                media.startTransmit(*port);
                port->startTransmit(media);
            }
    }
};
struct AgentAccount : Account {
    J cfg;
    std::shared_ptr<Speech> speech;
    std::vector<std::unique_ptr<AgentCall>> calls;
    AgentAccount(J c) : cfg(c) {
        auto lang = c["channels"]["sip"].value("language", "tr");
        auto resources = runtime_resources(c);
        resources["voice_language"] = lang;
        speech = std::make_shared<Speech>("models", resources);
        std::atomic<bool> cancel = false;
        speech->stream(
            lang == "en" ? "Hello." : "Merhaba.", lang, [](const float *, size_t, int) {}, cancel);
        speech->transcribe(std::vector<float>(16000, 0), lang);
    }
    void onIncomingCall(OnIncomingCallParam &p) override {
        calls.erase(std::remove_if(calls.begin(), calls.end(),
                                   [](const auto &c) { return c->ended.load(); }),
                    calls.end());
        for (auto &c : calls)
            if (!c->ended) {
                Call busy(*this, p.callId);
                CallOpParam reject;
                reject.statusCode = PJSIP_SC_BUSY_HERE;
                busy.answer(reject);
                return;
            }
        auto c = std::make_unique<AgentCall>(*this, p.callId, cfg, speech);
        CallOpParam accept;
        accept.statusCode = PJSIP_SC_OK;
        c->answer(accept);
        calls.push_back(std::move(c));
    }
};
int wmain(int argc, wchar_t **argv) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        J cfg = read_json("config.yaml"), sip = cfg["channels"]["sip"];
        bool localTest = argc > 1 && std::wstring(argv[1]) == L"--local-test";
        if (!localTest && !sip.value("enabled", false)) {
            std::cout << "SIP disabled; local test: --local-test\n";
            return 0;
        }
        Endpoint ep;
        ep.libCreate();
        EpConfig ec;
        ec.uaConfig.threadCnt = 1;
        ec.logConfig.level = 2;
        ec.medConfig.clockRate = 16000;
        ec.medConfig.sndClockRate = 16000;
        ep.libInit(ec);
        TransportConfig tc;
        tc.port = sip.value("port", 5060);
        tc.boundAddress = localTest ? "127.0.0.1" : sip.value("bind_address", "127.0.0.1");
        ep.transportCreate(PJSIP_TRANSPORT_UDP, tc);
        ep.libStart();
        ep.audDevManager().setNullDev();
        AgentAccount account(cfg);
        AccountConfig ac;
        ac.idUri =
            localTest ? "sip:agent@127.0.0.1" : sip.value("account_uri", "sip:agent@127.0.0.1");
        ac.regConfig.registrarUri = localTest ? "" : sip.value("registrar", "");
        if (!ac.regConfig.registrarUri.empty())
            ac.sipConfig.authCreds.emplace_back("digest", "*", sip.at("username"), 0,
                                                secret_get(sip.at("password_secret")));
        account.create(ac);
        std::cout << "SIP ready on " << tc.boundAddress << ":" << tc.port << "\n";
        while (true) {
            ep.libHandleEvents(50);
            Sleep(10);
        }
        return 0;
    } catch (const Error &e) {
        std::cerr << e.info() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
