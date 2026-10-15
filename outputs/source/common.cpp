#include "ajan.hpp"
#include <ctime>
#include <sddl.h>
#include <wincrypt.h>
std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), 0, 0);
    if (!n && !s.empty())
        throw std::runtime_error("Invalid UTF-8");
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string utf8(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, 0, 0);
    return s;
}
std::string lower(std::string s) {
    auto w = wide(s);
    for (auto &c : w) {
        if (c == L'I')
            c = L'ı';
        else if (c == L'İ')
            c = L'i';
        else if (c == L'Ç')
            c = L'ç';
        else if (c == L'Ğ')
            c = L'ğ';
        else if (c == L'Ö')
            c = L'ö';
        else if (c == L'Ş')
            c = L'ş';
        else if (c == L'Ü')
            c = L'ü';
        else
            c = towlower(c);
    }
    return utf8(w);
}
J read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("Missing file: " + p.string());
    return J::parse(f);
}
void write_json(const fs::path &p, const J &j) {
    fs::create_directories(p.parent_path());
    auto t = p;
    t += L".tmp";
    {
        std::ofstream f(t);
        f << j.dump(2);
    }
    MoveFileExW(t.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
double clock_ms() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
J runtime_resources(J config) {
    J resources = config.at("resources");
    resources["tts_tr"] = config["models"]["tts_tr"];
    resources["tts_en"] = config["models"]["tts_en"];
    resources["stt_model"] = config["models"]["stt"];
    resources["vad_model"] = config["models"].value("vad", "models/silero-vad.onnx");
    return resources;
}
std::string today() {
    time_t t = time(0);
    tm a;
    localtime_s(&a, &t);
    char b[20];
    strftime(b, 20, "%Y-%m-%d", &a);
    return b;
}
static fs::path secret_path(std::string key) {
    if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
        std::string::npos)
        throw std::runtime_error("Invalid secret name");
    return fs::path(L"data") / L"secrets" / (wide(key) + L".bin");
}
void secret_set(const std::string &k, const std::string &v) {
    DATA_BLOB in{(DWORD)v.size(), (BYTE *)v.data()}, out{};
    if (!CryptProtectData(&in, L"YerelAjan", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("DPAPI protection failed");
    auto p = secret_path(k);
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write((char *)out.pbData, out.cbData);
    LocalFree(out.pbData);
}
std::string secret_get(const std::string &k) {
    auto p = secret_path(k);
    std::ifstream f(p, std::ios::binary);
    std::vector<BYTE> b((std::istreambuf_iterator<char>(f)), {});
    if (b.empty())
        throw std::runtime_error("Secret not configured: " + k);
    DATA_BLOB in{(DWORD)b.size(), b.data()}, out{};
    if (!CryptUnprotectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out))
        throw std::runtime_error("DPAPI identity mismatch");
    std::string v((char *)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return v;
}
static const wchar_t *PIPE = L"\\\\.\\pipe\\YerelAjan-v1";
J pipe_request(J req) {
    if (!WaitNamedPipeW(PIPE, 30000))
        throw std::runtime_error("Ajan service is not ready");
    HANDLE h = CreateFileW(PIPE, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Pipe connection failed");
    auto s = req.dump() + "\n";
    DWORD n;
    WriteFile(h, s.data(), (DWORD)s.size(), &n, 0);
    std::string r;
    char b[4096];
    while (ReadFile(h, b, sizeof b, &n, 0) && n) {
        r.append(b, n);
        if (r.find('\n') != std::string::npos)
            break;
        if (r.size() > 1024 * 1024) {
            CloseHandle(h);
            throw std::runtime_error("Response too large");
        }
    }
    CloseHandle(h);
    return J::parse(r);
}
void pipe_server(std::function<J(J)> handler, std::atomic<bool> &stop) {
    HANDLE tok;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, 0, 0, &n);
    std::vector<BYTE> b(n);
    GetTokenInformation(tok, TokenUser, b.data(), n, &n);
    LPWSTR sid;
    ConvertSidToStringSidW(((TOKEN_USER *)b.data())->User.Sid, &sid);
    std::wstring sd = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    CloseHandle(tok);
    PSECURITY_DESCRIPTOR descriptor;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sd.c_str(), SDDL_REVISION_1, &descriptor,
                                                         0);
    SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
    while (!stop) {
        HANDLE h = CreateNamedPipeW(PIPE, PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                        PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 65536, 65536, 1000, &sa);
        if (h == INVALID_HANDLE_VALUE)
            break;
        bool connected = ConnectNamedPipe(h, 0) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            std::string s;
            char buf[4096];
            DWORD m;
            while (ReadFile(h, buf, sizeof buf, &m, 0) && m) {
                s.append(buf, m);
                if (s.find('\n') != std::string::npos || s.size() > 65536)
                    break;
            }
            J out;
            try {
                if (s.size() > 65536)
                    throw std::runtime_error("Input limit");
                out = handler(J::parse(s));
            } catch (const std::exception &e) {
                out = {{"ok", false}, {"error", e.what()}};
            }
            s = out.dump() + "\n";
            WriteFile(h, s.data(), (DWORD)s.size(), &m, 0);
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    LocalFree(descriptor);
}
