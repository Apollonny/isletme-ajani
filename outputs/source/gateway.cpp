#include "ajan.hpp"
#include <bcrypt.h>
#include <http.h>
#include <iostream>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
static std::string hmac(std::string key, std::string body) {
    BCRYPT_ALG_HANDLE a;
    BCRYPT_HASH_HANDLE h;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, 0, BCRYPT_ALG_HANDLE_HMAC_FLAG))
        throw std::runtime_error("HMAC algorithm failed");
    DWORD n, z;
    BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, (BYTE *)&n, sizeof n, &z, 0);
    std::vector<BYTE> obj(n), digest(32);
    if (BCryptCreateHash(a, &h, obj.data(), n, (BYTE *)key.data(), (ULONG)key.size(), 0))
        throw std::runtime_error("HMAC initialization failed");
    BCryptHashData(h, (BYTE *)body.data(), (ULONG)body.size(), 0);
    BCryptFinishHash(h, digest.data(), 32, 0);
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    std::string out = "sha256=";
    const char *hex = "0123456789abcdef";
    for (BYTE b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}
static bool equal(std::string a, std::string b) {
    unsigned diff = (unsigned)(a.size() ^ b.size());
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++)
        diff |= (i < a.size() ? (unsigned char)a[i] : 0) ^ (i < b.size() ? (unsigned char)b[i] : 0);
    return !diff;
}
static std::string graph_send(J cfg, std::string to, std::string text) {
    auto token = secret_get(cfg["token_secret"]);
    auto host = WinHttpOpen(L"YerelAjan/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, 0, 0, 0);
    if (!host)
        throw std::runtime_error("HTTP session failed");
    WinHttpSetTimeouts(host, 5000, 5000, 5000, 10000);
    auto conn = WinHttpConnect(host, L"graph.facebook.com", 443, 0);
    std::wstring path =
        L"/" + wide(cfg["graph_version"]) + L"/" + wide(cfg["phone_number_id"]) + L"/messages";
    auto req = WinHttpOpenRequest(conn, L"POST", path.c_str(), 0, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::wstring headers =
        L"Content-Type: application/json\r\nAuthorization: Bearer " + wide(token) + L"\r\n";
    std::string body = J({{"messaging_product", "whatsapp"},
                          {"recipient_type", "individual"},
                          {"to", to},
                          {"type", "text"},
                          {"text", {{"body", text}}}})
                           .dump();
    bool ok = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1L, (void *)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(req, 0);
    DWORD code = 0, n = sizeof code;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &code, &n,
                        0);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(host);
    if (!ok || code >= 300)
        throw std::runtime_error("WhatsApp delivery failed HTTP " + std::to_string(code));
    return "accepted";
}
static void http_reply(HANDLE queue, HTTP_REQUEST_ID id, int code, std::string body) {
    HTTP_RESPONSE response{};
    response.StatusCode = (USHORT)code;
    response.pReason = code == 200 ? "OK" : "Rejected";
    response.ReasonLength = (USHORT)strlen(response.pReason);
    HTTP_DATA_CHUNK chunk{};
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = (void *)body.data();
    chunk.FromMemory.BufferLength = (ULONG)body.size();
    response.EntityChunkCount = 1;
    response.pEntityChunks = &chunk;
    HttpSendHttpResponse(queue, id, 0, &response, 0, 0, 0, 0, 0, 0);
}
static std::string signature(HTTP_REQUEST *r) {
    for (int i = 0; i < r->Headers.UnknownHeaderCount; i++) {
        auto &h = r->Headers.pUnknownHeaders[i];
        std::string n(h.pName, h.NameLength);
        for (char &c : n)
            c = (char)tolower(c);
        if (n == "x-hub-signature-256")
            return std::string(h.pRawValue, h.RawValueLength);
    }
    return "";
}
int wmain(int argc, wchar_t **argv) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        auto cfg = read_json("config.yaml")["channels"]["whatsapp"];
        if (argc > 1 && std::wstring(argv[1]) == L"--signature-test") {
            auto s = hmac("key", "The quick brown fox jumps over the lazy dog");
            if (s != "sha256=f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8")
                throw std::runtime_error("HMAC mismatch");
            if (equal(s, s + "x"))
                throw std::runtime_error("Signature length error");
            std::cout << "PASS: HMAC-SHA256 and comparison\n";
            return 0;
        }
        if (!cfg.value("enabled", false)) {
            std::cout << "WhatsApp disabled. Configure Meta and HTTPS prerequisites.\n";
            return 0;
        }
        Store queue("data/whatsapp-queue.sqlite", J::object());
        queue.exec("CREATE TABLE IF NOT EXISTS inbox(id TEXT PRIMARY KEY,sender TEXT,text "
                   "TEXT,state TEXT DEFAULT 'new',reply TEXT,attempts INTEGER DEFAULT 0)");
        std::atomic<bool> stop = false;
        std::thread worker([&] {
            while (!stop) {
                try {
                    J jobs;
                    {
                        std::lock_guard<std::mutex> g(queue.mutex);
                        jobs = queue.query("SELECT * FROM inbox WHERE state IN ('new','reply') AND "
                                           "attempts<5 ORDER BY rowid LIMIT 1");
                    }
                    if (jobs.empty()) {
                        auto pending = pipe_request({{"op", "outbox"}, {"channel", "whatsapp"}});
                        for (auto &m : pending.value("messages", J::array())) {
                            graph_send(cfg, m.at("sender"), m.at("text"));
                            pipe_request({{"op", "outbox_ack"},
                                          {"channel", "whatsapp"},
                                          {"id", m.at("id")}});
                        }
                        Sleep(250);
                        continue;
                    }
                    auto j = jobs[0];
                    std::string reply;
                    if (j["state"] == "new") {
                        auto r = pipe_request({{"channel", "whatsapp"},
                                               {"sender", j["sender"]},
                                               {"id", j["id"]},
                                               {"text", j["text"]}});
                        reply = r.value("text", "");
                        std::lock_guard<std::mutex> g(queue.mutex);
                        queue.exec("UPDATE inbox SET state='reply',reply=? WHERE id=?",
                                   J::array({reply, j["id"]}));
                    } else
                        reply = j["reply"];
                    try {
                        graph_send(cfg, j["sender"], reply);
                        std::lock_guard<std::mutex> g(queue.mutex);
                        queue.exec("UPDATE inbox SET state='sent' WHERE id=?", J::array({j["id"]}));
                    } catch (...) {
                        std::lock_guard<std::mutex> g(queue.mutex);
                        queue.exec("UPDATE inbox SET attempts=attempts+1 WHERE id=?",
                                   J::array({j["id"]}));
                        Sleep(2000);
                    }
                } catch (const std::exception &e) {
                    std::cerr << "Gateway queue: " << e.what() << "\n";
                    Sleep(2000);
                }
            }
        });
        struct JoinWorker {
            std::atomic<bool> &stop;
            std::thread &worker;
            ~JoinWorker() {
                stop = true;
                if (worker.joinable())
                    worker.join();
            }
        } joinWorker{stop, worker};
        HTTPAPI_VERSION version{2, 0};
        if (HttpInitialize(version, HTTP_INITIALIZE_SERVER, 0))
            throw std::runtime_error("HTTP.sys initialization failed");
        HANDLE h;
        if (HttpCreateHttpHandle(&h, 0))
            throw std::runtime_error("HTTP.sys queue failed");
        std::wstring prefix = wide(cfg.at("listen_url"));
        if (prefix.rfind(L"https://", 0) != 0)
            throw std::runtime_error("Production webhook requires HTTPS");
        if (HttpAddUrl(h, prefix.c_str(), 0))
            throw std::runtime_error(
                "HTTPS URL binding failed; configure URL ACL and SSL certificate");
        std::cout << "HTTPS WhatsApp listener ready\n";
        while (!stop) {
            std::vector<BYTE> buffer(65536);
            auto *r = (HTTP_REQUEST *)buffer.data();
            ULONG received;
            ULONG result =
                HttpReceiveHttpRequest(h, HTTP_NULL_ID, 0, r, (ULONG)buffer.size(), &received, 0);
            if (result)
                continue;
            try {
                if (r->Verb == HttpVerbGET) {
                    std::string url(r->pRawUrl, r->RawUrlLength);
                    auto get = [&](std::string k) {
                        auto pos = url.find(k + "=");
                        if (pos == std::string::npos)
                            return std::string();
                        pos += k.size() + 1;
                        return url.substr(pos, url.find('&', pos) - pos);
                    };
                    if (get("hub.mode") == "subscribe" &&
                        equal(get("hub.verify_token"), secret_get(cfg["verify_secret"])))
                        http_reply(h, r->RequestId, 200, get("hub.challenge"));
                    else
                        http_reply(h, r->RequestId, 403, "");
                    continue;
                }
                if (r->Verb != HttpVerbPOST) {
                    http_reply(h, r->RequestId, 405, "");
                    continue;
                }
                std::string body;
                char chunk[8192];
                ULONG read;
                while (true) {
                    auto rc = HttpReceiveRequestEntityBody(h, r->RequestId, 0, chunk, sizeof chunk,
                                                           &read, 0);
                    if (read)
                        body.append(chunk, read);
                    if (body.size() > 65536)
                        throw std::runtime_error("Webhook too large");
                    if (rc == ERROR_HANDLE_EOF)
                        break;
                    if (rc)
                        throw std::runtime_error("Webhook read failed");
                }
                if (!equal(signature(r), hmac(secret_get(cfg["app_secret"]), body))) {
                    http_reply(h, r->RequestId, 403, "");
                    continue;
                }
                J event = J::parse(body);
                for (auto &entry : event.value("entry", J::array()))
                    for (auto &change : entry.value("changes", J::array()))
                        for (auto &message : change["value"].value("messages", J::array())) {
                            if (message.value("type", "") != "text")
                                continue;
                            std::lock_guard<std::mutex> g(queue.mutex);
                            queue.exec("INSERT OR IGNORE INTO inbox(id,sender,text) VALUES(?,?,?)",
                                       J::array({message.at("id"), message.at("from"),
                                                 message["text"].at("body")}));
                        }
                http_reply(h, r->RequestId, 200, "EVENT_RECEIVED");
            } catch (const std::exception &e) {
                std::cerr << "Webhook rejected: " << e.what() << "\n";
                http_reply(h, r->RequestId, 400, "");
            }
        }
        stop = true;
        worker.join();
        HttpRemoveUrl(h, prefix.c_str());
        CloseHandle(h);
        HttpTerminate(HTTP_INITIALIZE_SERVER, 0);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
