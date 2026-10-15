#include "ajan.hpp"
#include <bcrypt.h>
static std::string sha256(const fs::path &path) {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
    BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    DWORD size, read;
    BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (BYTE *)&size, sizeof size, &read, 0);
    std::vector<BYTE> object(size), digest(32);
    BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0);
    std::ifstream file(path, std::ios::binary);
    std::vector<char> buffer(1024 * 1024);
    while (file) {
        file.read(buffer.data(), buffer.size());
        auto count = file.gcount();
        if (count)
            BCryptHashData(hash, (BYTE *)buffer.data(), (ULONG)count, 0);
    }
    BCryptFinishHash(hash, digest.data(), 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    std::string out;
    const char *hex = "0123456789abcdef";
    for (BYTE b : digest) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}
J doctor(J config) {
    J result = {{"ok", true},
                {"files", J::array()},
                {"runtime", "CPU-only"},
                {"telemetry", "disabled"},
                {"whatsapp", "network exception; live credentials required"}};
    auto manifest = read_json("models.lock.json");
    for (auto &item : manifest) {
        fs::path path = fs::u8path(item.at("file").get<std::string>());
        bool valid = fs::exists(path) && fs::file_size(path) == item.at("bytes").get<uint64_t>() &&
                     sha256(path) == item.at("sha256");
        result["files"].push_back({{"path", item["file"]}, {"valid", valid}});
        if (!valid)
            result["ok"] = false;
    }
    for (auto &channel : {"sip", "sms", "whatsapp"})
        result["channels"][channel]["enabled"] =
            config["channels"][channel].value("enabled", false);
    result["live_tests"] = {{"physical_modem", "not_verified"},
                            {"meta_whatsapp", "not_verified"},
                            {"external_pbx", "not_verified"}};
    result["demo_business"] =
        read_json(config["business"]["profile"].get<std::string>()).value("demo", false);
    return result;
}
