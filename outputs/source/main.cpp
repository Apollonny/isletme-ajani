#include "ajan.hpp"
#include <iostream>
#include <psapi.h>
static SERVICE_STATUS_HANDLE serviceHandle;
static SERVICE_STATUS status{};
static std::function<void(std::atomic<bool> &)> serviceBody;
static std::atomic<bool> serviceStop = false;
static void WINAPI ctrl(DWORD code) {
    if (code == SERVICE_CONTROL_STOP) {
        serviceStop = true;
        status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(serviceHandle, &status);
    }
}
static void WINAPI service_main(DWORD, wchar_t **) {
    serviceHandle = RegisterServiceCtrlHandlerW(L"YerelAjan", ctrl);
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(serviceHandle, &status);
    try {
        serviceBody(serviceStop);
    } catch (...) {
    }
    status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(serviceHandle, &status);
}
void service_run(std::function<void(std::atomic<bool> &)> f) {
    serviceBody = f;
    SERVICE_TABLE_ENTRYW table[] = {{(LPWSTR)L"YerelAjan", service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table))
        throw std::runtime_error("Start from Windows service manager");
}
struct ChannelWorkers {
    HANDLE job = nullptr;
    explicit ChannelWorkers(J config) {
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        for (auto entry : std::vector<std::pair<std::string, std::wstring>>{
                 {"sip", L"YerelAjanSIP.exe"},
                 {"sms", L"YerelAjanSMS.exe"},
                 {"whatsapp", L"YerelAjanGateway.exe"}}) {
            if (!config["channels"][entry.first].value("enabled", false))
                continue;
            auto executable = fs::absolute(entry.second);
            std::wstring command = L"\"" + executable.native() + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                               fs::current_path().c_str(), &si, &pi)) {
                AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    ~ChannelWorkers() {
        if (job)
            CloseHandle(job);
    }
};
int wmain(int argc, wchar_t **argv) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        wchar_t modulePath[32768];
        GetModuleFileNameW(nullptr, modulePath, 32768);
        fs::current_path(fs::path(modulePath).parent_path());
        fs::path configPath = "config.yaml";
        for (int i = 1; i + 1 < argc; i++)
            if (std::wstring(argv[i]) == L"--config")
                configPath = argv[i + 1];
        if (configPath.is_absolute())
            fs::current_path(configPath.parent_path());
        auto cfg = read_json(configPath);
        std::string mode = argc > 1 ? utf8(argv[1]) : "--ui";
        if (mode == "--doctor") {
            auto r = doctor(cfg);
            std::cout << r.dump(2) << "\n";
            return r.value("ok", false) ? 0 : 1;
        }
        if (mode == "--secret") {
            if (argc != 3)
                throw std::runtime_error("Usage: --secret NAME (secret read from stdin)");
            std::string secret;
            std::getline(std::cin, secret);
            secret_set(utf8(argv[2]), secret);
            std::cout << "Protected for the current Windows identity.\n";
            return 0;
        }
        if (mode == "--request") {
            if (argc < 3)
                throw std::runtime_error("Usage: --request JSON");
            J r = pipe_request(J::parse(utf8(argv[2])));
            std::cout << r.dump() << "\n";
            return 0;
        }
        if (mode == "--tts" || mode == "--tts-en" || mode == "--tts-bench" ||
            mode == "--speech-test" || mode == "--transcribe") {
            double modelStarted = clock_ms();
            auto speechConfig = runtime_resources(cfg);
            if (mode == "--tts-en")
                speechConfig["voice_language"] = "en";
            Speech speech("models", speechConfig);
            double modelLoad = clock_ms() - modelStarted;
            if (mode == "--speech-test") {
                std::atomic<bool> cancelled = false;
                int chunks = 0;
                double interrupted = 0;
                speech.stream(
                    "Bu uzun sesli yanıt müşteri araya girdiğinde durdurulmalıdır. İkinci cümle "
                    "okunmamalıdır.",
                    "tr",
                    [&](const float *, size_t, int) {
                        ++chunks;
                        interrupted = clock_ms();
                        cancelled = true;
                    },
                    cancelled);
                double delay = clock_ms() - interrupted;
                speech.stream(
                    "Bu cümle okunmamalıdır.", "tr", [&](const float *, size_t, int) { ++chunks; },
                    cancelled);
                if (chunks != 1 || !interrupted)
                    throw std::runtime_error("Neural TTS cancellation failed");
                std::cout << J({{"passed", true},
                                {"chunks_before_cancel", chunks},
                                {"cancel_return_ms", delay}})
                                 .dump()
                          << "\n";
                return 0;
            }
            if (mode == "--transcribe") {
                if (argc < 3)
                    throw std::runtime_error("Usage: --transcribe WAV");
                int rate;
                auto x = load_wav(argv[2], rate);
                if (rate != 16000) {
                    std::vector<float> y((uint64_t)x.size() * 16000 / rate);
                    for (size_t i = 0; i < y.size(); i++)
                        y[i] = x[std::min(x.size() - 1, (size_t)((uint64_t)i * rate / 16000))];
                    x = std::move(y);
                }
                std::cout << speech.transcribe(x, "tr") << "\n";
                return 0;
            }
            int runs = mode == "--tts-bench" ? 12 : 1;
            for (int i = 1; i + 1 < argc; i++)
                if (std::wstring(argv[i]) == L"--runs")
                    runs = std::clamp(_wtoi(argv[i + 1]), 1, 100);
            std::vector<float> audio;
            std::atomic<bool> cancel = false;
            J metrics = J::array();
            std::string text =
                argc > 2 ? utf8(argv[2]) : "Merhaba, size nasıl yardımcı olabilirim?";
            for (int i = 0; i < runs; i++) {
                audio.clear();
                double start = clock_ms(), first = 0;
                int rate = 0;
                speech.stream(
                    text, mode == "--tts-en" ? "en" : "tr",
                    [&](const float *x, size_t n, int r) {
                        if (!first)
                            first = clock_ms() - start;
                        rate = r;
                        audio.insert(audio.end(), x, x + n);
                    },
                    cancel);
                PROCESS_MEMORY_COUNTERS_EX mem{};
                GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&mem,
                                     sizeof mem);
                double elapsed = clock_ms() - start;
                metrics.push_back({{"first_pcm_ms", first},
                                   {"stages", speech.stats()},
                                   {"total_ms", elapsed},
                                   {"audio_seconds", (double)audio.size() / rate},
                                   {"rtf", elapsed / 1000 / ((double)audio.size() / rate)},
                                   {"working_set_mb", mem.WorkingSetSize / 1048576.0},
                                   {"peak_working_set_mb", mem.PeakWorkingSetSize / 1048576.0},
                                   {"private_mb", mem.PrivateUsage / 1048576.0}});
            }
            fs::path out = argc > 3 ? fs::path(argv[3]) : fs::path("data/tts-sample.wav");
            save_wav(out, audio, mode == "--tts-en" ? 24000 : 48000);
            std::cout
                << J({{"text", text}, {"model_load_ms", modelLoad}, {"metrics", metrics}}).dump()
                << "\n";
            return 0;
        }
        if (mode == "--ui") {
            ShowWindow(GetConsoleWindow(), SW_HIDE);
            try {
                auto health = pipe_request({{"op", "health"}});
                if (health.value("ok", false)) {
                    std::atomic<bool> localStop = false;
                    Speech speech("models", runtime_resources(cfg));
                    return run_ui(nullptr, &speech, cfg, localStop);
                }
            } catch (...) {
            }
        }
        fs::path profilePath = fs::u8path(cfg["business"]["profile"].get<std::string>());
        J prof = read_json(profilePath);
        if (prof.value("demo", true))
            for (auto channel : {"sip", "sms", "whatsapp"})
                if (cfg["channels"][channel].value("enabled", false))
                    throw std::runtime_error("Customize the demo profile and set demo=false before "
                                             "enabling live channels");
        Store store(fs::u8path(cfg["storage"]["database"].get<std::string>()), prof);
        store.import_facts(fs::u8path(cfg["business"]["facts"].get<std::string>()));
        store.exec("DELETE FROM messages WHERE created<datetime('now',?)",
                   J::array({"-" +
                             std::to_string(
                                 std::clamp(cfg["storage"].value("retention_days", 30), 1, 3650)) +
                             " days"}));
        LocalPlanner planner(fs::u8path(cfg["models"]["llm"].get<std::string>()),
                             cfg["resources"].value("llm_threads", 3));
        Agent agent(store, &planner, cfg);
        if (mode == "--chat" || mode == "--jsonl") {
            std::string line;
            int id = 0;
            if (mode == "--chat")
                std::cout << "YerelAjan — çıkmak için /quit\n";
            while (std::getline(std::cin, line)) {
                if (line == "/quit")
                    break;
                J req = mode == "--jsonl" ? J::parse(line)
                                          : J({{"channel", "local"},
                                               {"sender", "demo"},
                                               {"id", std::to_string(++id)},
                                               {"text", line}});
                auto r = agent.turn(req);
                if (mode == "--jsonl")
                    std::cout << r.dump() << std::endl;
                else
                    std::cout << r.value("text", "") << std::endl;
            }
            return 0;
        }
        auto handler = [&](J r) -> J {
            std::string op = r.value("op", "turn");
            if (op == "turn")
                return agent.turn(r);
            if (op == "overview") {
                std::lock_guard<std::mutex> l(store.mutex);
                return store.overview();
            }
            if (op == "reset_session") {
                std::lock_guard<std::mutex> a(agent.mutex);
                agent.sessions.erase(r.at("session").get<std::string>());
                return {{"ok", true}};
            }
            if (op == "takeover" || op == "resume") {
                std::lock_guard<std::mutex> l(agent.mutex);
                agent.sessions[r.at("session").get<std::string>()].human = (op == "takeover");
                return {{"ok", true}};
            }
            if (op == "staff_reply") {
                std::string owner = r.at("session"), text = r.at("text");
                auto colon = owner.find(':');
                if (colon == std::string::npos || text.empty() || text.size() > 4096)
                    throw std::runtime_error("Invalid operator reply");
                std::lock_guard<std::mutex> a(agent.mutex);
                std::lock_guard<std::mutex> d(store.mutex);
                if (!agent.sessions[owner].human)
                    throw std::runtime_error("Take over the session before replying");
                store.exec("INSERT INTO outbox(channel,sender,text) VALUES(?,?,?)",
                           J::array({owner.substr(0, colon), owner.substr(colon + 1), text}));
                return {{"ok", true}, {"text", "Çalışan yanıtı gönderim kuyruğuna alındı."}};
            }
            if (op == "outbox") {
                std::lock_guard<std::mutex> d(store.mutex);
                if (r.contains("sender"))
                    return {{"messages",
                             store.query("SELECT id,sender,text FROM outbox WHERE channel=? AND "
                                         "sender=? AND state='queued' ORDER BY id LIMIT 1",
                                         J::array({r.at("channel"), r.at("sender")}))}};
                return {{"messages", store.query("SELECT id,sender,text FROM outbox WHERE "
                                                 "channel=? AND state='queued' ORDER BY id LIMIT 1",
                                                 J::array({r.at("channel")}))}};
            }
            if (op == "outbox_ack") {
                std::lock_guard<std::mutex> d(store.mutex);
                store.exec("UPDATE outbox SET state=? WHERE id=? AND channel=?",
                           J::array({r.value("state", "sent"), r.at("id"), r.at("channel")}));
                return {{"ok", true}};
            }
            if (op == "health")
                return {{"ok", true}, {"ai", "local"}, {"whatsapp_exception", true}};
            throw std::runtime_error("Unknown IPC operation");
        };
        auto server = [&](std::atomic<bool> &stop) {
            std::thread p([&] { pipe_server(handler, stop); });
            ChannelWorkers workers(cfg);
            while (!stop)
                Sleep(100);
            try {
                pipe_request({{"op", "health"}});
            } catch (...) {
            }
            p.join();
        };
        if (mode == "--service") {
            service_run(server);
            return 0;
        }
        std::atomic<bool> stop = false;
        if (mode == "--serve") {
            server(stop);
            return 0;
        }
        std::thread pipe([&] { pipe_server(handler, stop); });
        ChannelWorkers workers(cfg);
        int ret = 0;
        try {
            Speech speech("models", runtime_resources(cfg));
            std::atomic<bool> warmcancel = false;
            speech.stream("Merhaba.", "tr", [](const float *, size_t, int) {}, warmcancel);
            ret = run_ui(&agent, &speech, cfg, stop);
        } catch (const std::exception &e) {
            std::cerr << "Voice disabled: " << e.what() << "\n";
            ret = run_ui(&agent, nullptr, cfg, stop);
        }
        stop = true;
        try {
            pipe_request({{"op", "health"}});
        } catch (...) {
        }
        pipe.join();
        return ret;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
