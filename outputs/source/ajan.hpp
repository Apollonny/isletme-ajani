#pragma once
#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mmsystem.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <sqlite3.h>
#include <thread>
#include <vector>

using J = nlohmann::json;
namespace fs = std::filesystem;
std::string utf8(const std::wstring &);
std::wstring wide(const std::string &);
std::string lower(std::string);
J read_json(const fs::path &);
void write_json(const fs::path &, const J &);
double clock_ms();
std::string today();
std::string secret_get(const std::string &);
void secret_set(const std::string &, const std::string &);
J pipe_request(J);
void pipe_server(std::function<J(J)> handler, std::atomic<bool> &stop);
J plugin_run(J, J);
J doctor(J);
J runtime_resources(J);
struct Store {
    sqlite3 *db = nullptr;
    J profile;
    std::mutex mutex;
    Store(fs::path path, J p);
    ~Store();
    J query(std::string sql, J args = J::array());
    void exec(std::string sql, J args = J::array());
    J search(std::string);
    J slots(std::string service, std::string date, int party = 1);
    J book(J args, std::string owner, std::string request);
    J change(J args, std::string owner, std::string request);
    J cancel(std::string code, std::string owner, std::string request);
    void import_facts(const fs::path &);
    J overview();
};
struct Planner {
    virtual ~Planner() = default;
    virtual J plan(J context) = 0;
};
struct LocalPlanner : Planner {
    struct Impl;
    std::unique_ptr<Impl> impl;
    LocalPlanner(fs::path, int);
    ~LocalPlanner();
    J plan(J) override;
};
struct Session {
    std::string lang = "tr";
    J pending = nullptr, draft = nullptr;
    J history = J::array();
    std::string last_booking;
    bool human = false;
};
struct Agent {
    Store &store;
    Planner *planner;
    J config;
    std::map<std::string, Session> sessions;
    std::mutex mutex;
    Agent(Store &, Planner *, J);
    J turn(J);
    J render(J tool, Session &);
    J tool(std::string, J, std::string, std::string);
};
using Sink = std::function<void(const float *, size_t, int)>;
// Stateful rational-rate conversion; carries fractional phase across chunks.
struct PcmResampler {
    int target, source = 0, phase = 0;
    float previous = 0, filtered = 0;
    explicit PcmResampler(int rate = 16000) : target(rate) {}
    std::vector<float> push(const float *samples, size_t count, int rate);
};
struct Speech {
    struct Impl;
    std::unique_ptr<Impl> impl;
    Speech(fs::path, J);
    ~Speech();
    void stream(std::string, std::string, Sink, std::atomic<bool> &, bool cache_safe = false);
    J stats() const;
    std::string transcribe(const std::vector<float> &, std::string);
    bool vad(const float *, size_t);
    void reset_vad();
};
void save_wav(fs::path, const std::vector<float> &, int);
std::vector<float> load_wav(fs::path, int &);
void play_pcm(const float *, size_t, int, std::atomic<bool> &);
void microphone(Speech &, std::function<void(std::vector<float>)>, std::atomic<bool> &,
                std::function<void()> speech_started = {});
int run_ui(Agent *, Speech *, const J &, std::atomic<bool> &);
void service_run(std::function<void(std::atomic<bool> &)>);
