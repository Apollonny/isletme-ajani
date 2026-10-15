// This independent Gammu worker is distributed under GPL-2.0-or-later.
#include <filesystem>
#include <fstream>
#include <gammu.h>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <vector>
#include <windows.h>
using J = nlohmann::json;
namespace fs = std::filesystem;
static std::string from_unicode(const unsigned char *p) {
    std::wstring w;
    while (p[0] || p[1]) {
        w += (wchar_t)((p[0] << 8) | p[1]);
        p += 2;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, 0, 0);
    return s;
}
static std::vector<unsigned char> to_unicode(std::string s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), 0, 0);
    if (n <= 0)
        throw std::runtime_error("Invalid UTF8 SMS");
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    std::vector<unsigned char> b((n + 1) * 2);
    for (int i = 0; i < n; i++) {
        b[i * 2] = w[i] >> 8;
        b[i * 2 + 1] = w[i] & 255;
    }
    return b;
}
static J core(J request) {
    const wchar_t *p = L"\\\\.\\pipe\\YerelAjan-v1";
    if (!WaitNamedPipeW(p, 20000))
        throw std::runtime_error("Agent unavailable");
    HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Pipe failed");
    auto s = request.dump() + "\n";
    DWORD n;
    WriteFile(h, s.data(), (DWORD)s.size(), &n, 0);
    s.clear();
    char buf[8192];
    while (ReadFile(h, buf, sizeof buf, &n, 0) && n) {
        s.append(buf, n);
        if (s.find('\n') != std::string::npos)
            break;
    }
    CloseHandle(h);
    return J::parse(s);
}
static void persist(J state) {
    fs::create_directories("data");
    {
        std::ofstream f("data/sms-state.tmp");
        f << state.dump();
    }
    MoveFileExW(L"data/sms-state.tmp", L"data/sms-state.json",
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static void send(GSM_StateMachine *s, std::string number, std::string text) {
    auto b = to_unicode(text), num = to_unicode(number);
    GSM_MultiPartSMSInfo info{};
    GSM_ClearMultiPartSMSInfo(&info);
    info.EntriesNum = 1;
    info.UnicodeCoding = TRUE;
    info.Entries[0].ID = SMS_ConcatenatedTextLong;
    info.Entries[0].Buffer = b.data();
    GSM_MultiSMSMessage msgs{};
    auto err = GSM_EncodeMultiPartSMS(GSM_GetDebug(s), &info, &msgs);
    if (err != ERR_NONE)
        throw std::runtime_error(GSM_ErrorString(err));
    for (int i = 0; i < msgs.Number; i++) {
        auto &msg = msgs.SMS[i];
        if (num.size() > sizeof msg.Number)
            throw std::runtime_error("Phone number too long");
        memcpy(msg.Number, num.data(), num.size());
        msg.SMSC.Location = 1;
        msg.PDU = SMS_Submit;
        err = GSM_SendSMS(s, &msg);
        if (err != ERR_NONE)
            throw std::runtime_error(GSM_ErrorString(err));
    }
}
int main(int argc, char **argv) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        if (argc > 1 && std::string(argv[1]) == "--unicode-test") {
            std::string t = "İğne, çay, ölçü, 14:30, 500 TL";
            auto b = to_unicode(t);
            if (from_unicode(b.data()) != t)
                throw std::runtime_error("Unicode roundtrip");
            std::cout << "PASS: Turkish UTF-8/UCS-2 SMS roundtrip\n";
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--local-test") {
            std::string line;
            while (std::getline(std::cin, line)) {
                auto r = J::parse(line);
                r["channel"] = "sms";
                std::cout << core(r).dump() << std::endl;
            }
            return 0;
        }
        std::ifstream f("config.yaml");
        auto cfg = J::parse(f)["channels"]["sms"];
        if (!cfg.value("enabled", false)) {
            std::cout << "SMS disabled. Configure SIM modem first.\n";
            return 0;
        }
        GSM_StateMachine *s = GSM_AllocStateMachine();
        auto *c = GSM_GetConfig(s, 0);
        c->Device = _strdup(cfg.value("device", "COM3").c_str());
        c->Connection = _strdup(cfg.value("connection", "at").c_str());
        c->SyncTime = FALSE;
        c->LockDevice = FALSE;
        c->StartInfo = FALSE;
        GSM_SetConfigNum(s, 1);
        auto err = GSM_InitConnection(s, 1);
        if (err != ERR_NONE)
            throw std::runtime_error(GSM_ErrorString(err));
        J state = {{"seen", J::object()}, {"parts", J::object()}};
        if (fs::exists("data/sms-state.json")) {
            std::ifstream z("data/sms-state.json");
            state = J::parse(z);
        }
        while (true) {
            GSM_MultiSMSMessage messages{};
            messages.SMS[0].Folder = 0;
            messages.SMS[0].Location = 0;
            gboolean first = TRUE;
            while ((err = GSM_GetNextSMS(s, &messages, first)) == ERR_NONE) {
                first = FALSE;
                for (int i = 0; i < messages.Number; i++) {
                    auto &msg = messages.SMS[i];
                    if (msg.PDU != SMS_Deliver)
                        continue;
                    auto number = from_unicode(msg.Number), text = from_unicode(msg.Text);
                    std::string stamp = std::to_string(msg.DateTime.Year) + "-" +
                                        std::to_string(msg.DateTime.Month) + "-" +
                                        std::to_string(msg.DateTime.Day) + "-" +
                                        std::to_string(msg.DateTime.Hour) + "-" +
                                        std::to_string(msg.DateTime.Minute) + "-" +
                                        std::to_string(msg.DateTime.Second);
                    std::string id = number + "|" + stamp + "|" + text;
                    if (msg.UDH.AllParts > 1) {
                        int count = msg.UDH.AllParts, part = msg.UDH.PartNumber;
                        if (count > 20 || part < 1 || part > count)
                            continue;
                        std::string key =
                            number + "|" + std::to_string(msg.DateTime.Year) + "-" +
                            std::to_string(msg.DateTime.Month) + "-" +
                            std::to_string(msg.DateTime.Day) + "|" +
                            std::to_string(msg.UDH.ID16bit >= 0 ? msg.UDH.ID16bit : msg.UDH.ID8bit);
                        state["parts"][key][std::to_string(part)] = text;
                        persist(state);
                        if (state["parts"][key].size() != (size_t)count)
                            continue;
                        text.clear();
                        for (int k = 1; k <= count; k++)
                            text += state["parts"][key][std::to_string(k)].get<std::string>();
                        id = key + "|" + text;
                    }
                    if (state["seen"].contains(id))
                        continue;
                    try {
                        auto response = core(
                            {{"channel", "sms"}, {"sender", number}, {"id", id}, {"text", text}});
                        if (!response.contains("text"))
                            continue;
                        state["seen"][id] = "sending";
                        persist(state);
                        try {
                            send(s, number, response["text"]);
                            state["seen"][id] = "sent";
                        } catch (...) {
                            state["seen"][id] = "ambiguous_delivery_requires_operator";
                        }
                        persist(state);
                    } catch (const std::exception &e) {
                        std::cerr << "SMS processing: " << e.what() << "\n";
                    }
                }
            }
            try {
                auto pending = core({{"op", "outbox"}, {"channel", "sms"}});
                for (auto &m : pending.value("messages", J::array())) {
                    core({{"op", "outbox_ack"},
                          {"channel", "sms"},
                          {"id", m["id"]},
                          {"state", "sending"}});
                    try {
                        send(s, m.at("sender"), m.at("text"));
                        core({{"op", "outbox_ack"}, {"channel", "sms"}, {"id", m["id"]}});
                    } catch (...) {
                        core({{"op", "outbox_ack"},
                              {"channel", "sms"},
                              {"id", m["id"]},
                              {"state", "ambiguous_delivery_requires_operator"}});
                    }
                }
            } catch (...) {
            }
            Sleep(2000);
        }
        GSM_TerminateConnection(s);
        GSM_FreeStateMachine(s);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
