#include "ajan.hpp"
#include <bcrypt.h>
#include <regex>
#include <set>
static int minute(std::string);
static void bind(sqlite3_stmt *s, J a) {
    for (size_t i = 0; i < a.size(); i++) {
        auto &v = a[i];
        if (v.is_number_integer())
            sqlite3_bind_int64(s, (int)i + 1, v.get<int64_t>());
        else if (v.is_null())
            sqlite3_bind_null(s, (int)i + 1);
        else {
            auto t = v.is_string() ? v.get<std::string>() : v.dump();
            sqlite3_bind_text(s, (int)i + 1, t.c_str(), (int)t.size(), SQLITE_TRANSIENT);
        }
    }
}
J Store::query(std::string q, J args) {
    sqlite3_stmt *s = nullptr;
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &s, 0) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    bind(s, args);
    J rows = J::array();
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        J row;
        for (int i = 0; i < sqlite3_column_count(s); i++) {
            auto key = sqlite3_column_name(s, i);
            switch (sqlite3_column_type(s, i)) {
            case SQLITE_INTEGER:
                row[key] = sqlite3_column_int64(s, i);
                break;
            case SQLITE_NULL:
                row[key] = nullptr;
                break;
            default:
                row[key] = (const char *)sqlite3_column_text(s, i);
            }
        }
        rows.push_back(row);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db));
    return rows;
}
void Store::exec(std::string q, J a) {
    query(q, a);
}
Store::Store(fs::path p, J prof) : profile(prof) {
    if (profile.contains("services")) {
        auto bounded = [](int n) { return n > 0 && n <= 1440; };
        if (!bounded(profile.value("slot_minutes", 30)))
            throw std::runtime_error("slot_minutes must be between 1 and 1440");
        std::set<std::string> services, resources;
        for (auto &s : profile.at("services")) {
            auto id = s.at("id").get<std::string>();
            if (id.empty() || !services.insert(id).second || !bounded(s.at("duration")) ||
                s.value("price_cents", 0) < 0)
                throw std::runtime_error("Invalid or duplicate service definition");
        }
        auto hours = [](J h) {
            for (auto it = h.begin(); it != h.end(); ++it) {
                if (it.key().size() != 1 || it.key()[0] < '0' || it.key()[0] > '6' ||
                    !it.value().is_array() || it.value().size() != 2 ||
                    minute(it.value()[0]) >= minute(it.value()[1]))
                    throw std::runtime_error("Invalid opening hours");
            }
        };
        hours(profile.at("hours"));
        for (auto &r : profile.at("resources")) {
            auto id = r.at("id").get<std::string>();
            if (id.empty() || !resources.insert(id).second || r.value("capacity", 1) < 1 ||
                r.value("capacity", 1) > 100)
                throw std::runtime_error("Invalid or duplicate resource definition");
            for (auto &s : r.at("services"))
                if (!services.count(s.get<std::string>()))
                    throw std::runtime_error("Resource references an unknown service");
            if (r.contains("hours"))
                hours(r.at("hours"));
        }
    }
    fs::create_directories(p.parent_path());
    if (sqlite3_open_v2(p.u8string().c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        0) != SQLITE_OK)
        throw std::runtime_error("SQLite open failed");
    sqlite3_busy_timeout(db, 3000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA cache_size=-4096");
    exec("CREATE TABLE IF NOT EXISTS bookings(code TEXT PRIMARY KEY, owner TEXT NOT NULL, service "
         "TEXT NOT NULL, resource TEXT NOT NULL, date TEXT NOT NULL, minute INTEGER NOT "
         "NULL,duration INTEGER NOT NULL, party INTEGER NOT NULL, name TEXT NOT NULL,status TEXT "
         "NOT NULL DEFAULT 'active')");
    exec("CREATE INDEX IF NOT EXISTS schedule ON bookings(resource,date,minute,status)");
    exec("CREATE TABLE IF NOT EXISTS effects(owner TEXT,request TEXT,result TEXT,PRIMARY "
         "KEY(owner,request))");
    exec("CREATE TABLE IF NOT EXISTS events(owner TEXT,id TEXT,response TEXT,PRIMARY "
         "KEY(owner,id))");
    exec("CREATE TABLE IF NOT EXISTS messages(id INTEGER PRIMARY KEY,owner TEXT,kind TEXT,text "
         "TEXT,state TEXT DEFAULT 'open',created TEXT DEFAULT CURRENT_TIMESTAMP)");
    exec("CREATE TABLE IF NOT EXISTS outbox(id INTEGER PRIMARY KEY,channel TEXT,sender TEXT,text "
         "TEXT,state TEXT DEFAULT 'queued')");
    exec("CREATE TABLE IF NOT EXISTS facts(id TEXT PRIMARY KEY,source TEXT,version TEXT,text "
         "TEXT,lang TEXT)");
    exec("CREATE VIRTUAL TABLE IF NOT EXISTS facts_fts USING fts5(id "
         "UNINDEXED,text,tokenize='unicode61')");
}
Store::~Store() {
    sqlite3_close(db);
}
void Store::import_facts(const fs::path &p) {
    J facts = J::array();
    auto ingest = [&](const fs::path &file) {
        if (fs::file_size(file) > 512 * 1024)
            throw std::runtime_error("Knowledge file too large");
        if (file.extension() == ".json") {
            auto rows = read_json(file);
            for (auto row : rows) {
                row["source"] = file.filename().string();
                facts.push_back(row);
            }
        } else if (file.extension() == ".md") {
            std::ifstream stream(file);
            std::string line, paragraph;
            int id = 0;
            auto flush = [&] {
                if (!paragraph.empty()) {
                    facts.push_back(
                        {{"id", file.stem().string() + ":" + std::to_string(++id)},
                         {"text", paragraph},
                         {"source", file.filename().string()},
                         {"lang", file.parent_path().filename() == "en" ? "en" : "tr"}});
                    paragraph.clear();
                }
            };
            while (std::getline(stream, line)) {
                if (line.empty())
                    flush();
                else {
                    if (!paragraph.empty())
                        paragraph += " ";
                    paragraph += line;
                }
            }
            flush();
        }
    };
    if (fs::is_directory(p)) {
        for (auto &file : fs::recursive_directory_iterator(p))
            if (file.is_regular_file())
                ingest(file.path());
    } else
        ingest(p);
    if (facts.size() > 10000)
        throw std::runtime_error("Knowledge record limit");
    exec("BEGIN IMMEDIATE");
    try {
        exec("DELETE FROM facts");
        exec("DELETE FROM facts_fts");
        for (auto &f : facts) {
            exec("INSERT INTO facts VALUES(?,?,?,?,?)",
                 J::array({f.at("id"), f.value("source", p.filename().string()),
                           f.value("version", "1"), f.at("text"), f.value("lang", "tr")}));
            exec("INSERT INTO facts_fts VALUES(?,?)", J::array({f.at("id"), lower(f.at("text"))}));
        }
        for (auto &s : profile.at("services")) {
            std::string t =
                s.at("name").get<std::string>() + ". " + s.value("description", "") +
                " Süre: " + std::to_string(s.at("duration").get<int>()) +
                " dakika. Fiyat: " + std::to_string(s.at("price_cents").get<int>() / 100) + "," +
                (s.at("price_cents").get<int>() % 100 < 10 ? "0" : "") +
                std::to_string(s.at("price_cents").get<int>() % 100) + " TL.";
            exec("INSERT INTO facts VALUES(?,?,?,?,?)",
                 J::array({"service:" + s.at("id").get<std::string>(), "profile.json",
                           profile.value("version", "1"), t, "tr"}));
            exec("INSERT INTO facts_fts VALUES(?,?)",
                 J::array({"service:" + s.at("id").get<std::string>(), lower(t)}));
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
J Store::search(std::string text) {
    auto w = wide(lower(text));
    std::vector<std::wstring> words;
    std::wstring word;
    for (wchar_t c : w) {
        if (iswalnum(c))
            word += c;
        else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(word);
    std::string q;
    for (auto &w : words) {
        if (w.size() < 3)
            continue;
        if (!q.empty())
            q += " OR ";
        q += '"' + utf8(w) + '"';
        if (q.size() > 500)
            break;
    }
    if (q.empty())
        return J::array();
    return query("SELECT f.id,f.text,f.source,f.version,f.lang FROM facts_fts x JOIN facts f ON "
                 "f.id=x.id WHERE facts_fts MATCH ? ORDER BY bm25(facts_fts) LIMIT 4",
                 J::array({q}));
}
static int minute(std::string t) {
    if (!std::regex_match(t, std::regex("[0-2][0-9]:[0-5][0-9]")))
        throw std::runtime_error("Invalid time");
    int m = stoi(t.substr(0, 2)) * 60 + stoi(t.substr(3));
    if (m >= 1440)
        throw std::runtime_error("Invalid time");
    return m;
}
static std::string at(int m) {
    char b[8];
    snprintf(b, 8, "%02d:%02d", m / 60, m % 60);
    return b;
}
static int weekday(std::string date) {
    if (!std::regex_match(date, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")))
        throw std::runtime_error("Invalid date");
    tm t{};
    t.tm_year = stoi(date.substr(0, 4)) - 1900;
    t.tm_mon = stoi(date.substr(5, 2)) - 1;
    t.tm_mday = stoi(date.substr(8, 2));
    t.tm_hour = 12;
    t.tm_isdst = -1;
    if (mktime(&t) < 0)
        throw std::runtime_error("Invalid date");
    char b[20];
    strftime(b, 20, "%Y-%m-%d", &t);
    if (date != b)
        throw std::runtime_error("Invalid date");
    return t.tm_wday;
}
J Store::slots(std::string service, std::string date, int party) {
    int wd = weekday(date);
    if (date < today())
        return {{"ok", false}, {"reason", "past_date"}};
    if (party < 1 || party > 100)
        return {{"ok", false}, {"reason", "invalid_party"}};
    J s = nullptr;
    for (auto &v : profile.at("services"))
        if (v.at("id") == service)
            s = v;
    if (s.is_null())
        return {{"ok", false}, {"reason", "unknown_service"}};
    J out = J::array();
    int dur = s.at("duration");
    for (auto &r : profile.at("resources")) {
        if (!r.at("services").is_array() ||
            std::find(r["services"].begin(), r["services"].end(), service) == r["services"].end())
            continue;
        if (party > r.value("capacity", 1))
            continue;
        auto key = std::to_string(wd);
        if (!profile["hours"].contains(key))
            continue;
        auto hours = r.contains("hours") ? r["hours"].value(key, profile["hours"][key])
                                         : profile["hours"][key];
        for (int m = minute(hours[0]); m + dur <= minute(hours[1]);
             m += profile.value("slot_minutes", 30)) {
            if (date == today()) {
                time_t now = time(0);
                tm lt;
                localtime_s(&lt, &now);
                if (m <= lt.tm_hour * 60 + lt.tm_min)
                    continue;
            }
            auto rows = query("SELECT minute,duration,party FROM bookings WHERE resource=? AND "
                              "date=? AND status='active' AND minute<? AND minute+duration>?",
                              J::array({r.at("id"), date, m + dur, m}));
            std::map<int, int> events;
            for (auto &row : rows) {
                int start = row["minute"], end = start + row["duration"].get<int>(),
                    n = row["party"];
                events[std::max(start, m)] += n;
                events[std::min(end, m + dur)] -= n;
            }
            int used = 0, current = 0;
            for (auto event : events) {
                current += event.second;
                used = std::max(used, current);
            }
            if (used + party <= r.value("capacity", 1))
                out.push_back({{"date", date},
                               {"time", at(m)},
                               {"resource", r["id"]},
                               {"resource_name", r["name"]}});
        }
    }
    return {{"ok", true}, {"service", s}, {"slots", out}};
}
J Store::book(J a, std::string owner, std::string req) {
    exec("BEGIN IMMEDIATE");
    try {
        auto replay =
            query("SELECT result FROM effects WHERE owner=? AND request=?", J::array({owner, req}));
        if (!replay.empty()) {
            exec("COMMIT");
            return J::parse(replay[0]["result"].get<std::string>());
        }
        auto result = slots(a.at("service_id"), a.at("date"), a.value("party", 1));
        bool found = false;
        if (result.value("ok", false))
            for (auto &v : result["slots"])
                if (v["time"] == a.at("time") && v["resource"] == a.at("resource"))
                    found = true;
        if (!found) {
            exec("ROLLBACK");
            return {{"ok", false}, {"reason", "unavailable"}};
        }
        BYTE b[4];
        if (BCryptGenRandom(0, b, 4, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            throw std::runtime_error("RNG failed");
        char code[9];
        snprintf(code, 9, "%02X%02X%02X%02X", b[0], b[1], b[2], b[3]);
        exec("INSERT INTO bookings(code,owner,service,resource,date,minute,duration,party,name) "
             "VALUES(?,?,?,?,?,?,?,?,?)",
             J::array({code, owner, a.at("service_id"), a.at("resource"), a.at("date"),
                       minute(a.at("time")), result["service"]["duration"], a.value("party", 1),
                       a.value("name", "Müşteri")}));
        J r = {{"ok", true},
               {"booking_code", code},
               {"date", a["date"]},
               {"time", a["time"]},
               {"service", result["service"]["name"]},
               {"resource", a["resource"]}};
        exec("INSERT INTO effects VALUES(?,?,?)", J::array({owner, req, r.dump()}));
        exec("COMMIT");
        return r;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
J Store::cancel(std::string code, std::string owner, std::string req) {
    exec("BEGIN IMMEDIATE");
    try {
        auto r =
            query("SELECT result FROM effects WHERE owner=? AND request=?", J::array({owner, req}));
        if (!r.empty()) {
            exec("COMMIT");
            return J::parse(r[0]["result"].get<std::string>());
        }
        auto rows = query("SELECT code FROM bookings WHERE code=? AND owner=? AND status='active'",
                          J::array({code, owner}));
        if (rows.empty()) {
            exec("ROLLBACK");
            return {{"ok", false}, {"reason", "not_found_or_not_owned"}};
        }
        exec("UPDATE bookings SET status='cancelled' WHERE code=? AND owner=?",
             J::array({code, owner}));
        J out = {{"ok", true}, {"cancelled", code}};
        exec("INSERT INTO effects VALUES(?,?,?)", J::array({owner, req, out.dump()}));
        exec("COMMIT");
        return out;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
J Store::change(J a, std::string owner, std::string req) {
    exec("BEGIN IMMEDIATE");
    try {
        auto replay =
            query("SELECT result FROM effects WHERE owner=? AND request=?", J::array({owner, req}));
        if (!replay.empty()) {
            exec("COMMIT");
            return J::parse(replay[0]["result"].get<std::string>());
        }
        auto b = query("SELECT * FROM bookings WHERE code=? AND owner=? AND status='active'",
                       J::array({a.at("booking_code"), owner}));
        if (b.empty()) {
            exec("ROLLBACK");
            return {{"ok", false}, {"reason", "not_found_or_not_owned"}};
        }
        auto old = b[0];
        exec("UPDATE bookings SET status='changing' WHERE code=?", J::array({old["code"]}));
        auto s = slots(old["service"], a.at("date"), old["party"]);
        bool found = false;
        for (auto &v : s.value("slots", J::array()))
            if (v["time"] == a.at("time") && v["resource"] == a.at("resource"))
                found = true;
        if (!found) {
            exec("ROLLBACK");
            return {{"ok", false}, {"reason", "unavailable"}};
        }
        exec("UPDATE bookings SET date=?,minute=?,resource=?,status='active' WHERE code=?",
             J::array({a.at("date"), minute(a.at("time")), a.at("resource"), old["code"]}));
        J out = {{"ok", true},
                 {"booking_code", old["code"]},
                 {"date", a["date"]},
                 {"time", a["time"]},
                 {"changed", true}};
        exec("INSERT INTO effects VALUES(?,?,?)", J::array({owner, req, out.dump()}));
        exec("COMMIT");
        return out;
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
J Store::overview() {
    return {
        {"bookings", query("SELECT code,service,resource,date,minute,party,name,status FROM "
                           "bookings ORDER BY date,minute LIMIT 100")},
        {"messages", query("SELECT * FROM messages WHERE state='open' ORDER BY id DESC LIMIT 100")},
        {"outbox", query("SELECT id,channel,sender,text,state FROM outbox WHERE state!='sent' "
                         "ORDER BY id DESC LIMIT 100")}};
}
