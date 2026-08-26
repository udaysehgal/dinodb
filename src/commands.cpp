#include "dinodb/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>

#include "dinodb/server.hpp"

namespace dinodb {

using resp::Request;

namespace {

std::string upper(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

std::optional<int64_t> parse_int(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

std::optional<double> parse_double(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return std::nullopt;
    return v;
}

bool glob_match(const char* pat, const char* str) {
    if (*pat == '\0') return *str == '\0';
    if (*pat == '*') {
        while (*pat == '*') ++pat;
        if (*pat == '\0') return true;
        for (const char* s = str; *s; ++s) {
            if (glob_match(pat, s)) return true;
        }
        return false;
    }
    if (*str == '\0') return false;
    if (*pat == '?' || *pat == *str) return glob_match(pat + 1, str + 1);
    return false;
}

std::string err_wrongtype() {
    return resp::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string err_args(const std::string& cmd) {
    return resp::error("ERR wrong number of arguments for '" + cmd + "' command");
}

std::string err_notint() { return resp::error("ERR value is not an integer or out of range"); }
std::string err_notfloat() { return resp::error("ERR value is not a valid float"); }

// ---- String commands ----------------------------------------------------

std::string cmd_set(Store& store, const Request& r) {
    if (r.size() < 3) return err_args("set");
    std::optional<int64_t> ex_millis;
    bool nx = false, xx = false;
    for (size_t i = 3; i < r.size(); ++i) {
        std::string opt = upper(r[i]);
        if (opt == "EX" && i + 1 < r.size()) {
            auto v = parse_int(r[++i]);
            if (!v) return err_notint();
            ex_millis = *v * 1000;
        } else if (opt == "PX" && i + 1 < r.size()) {
            auto v = parse_int(r[++i]);
            if (!v) return err_notint();
            ex_millis = *v;
        } else if (opt == "NX") {
            nx = true;
        } else if (opt == "XX") {
            xx = true;
        } else {
            return resp::error("ERR syntax error");
        }
    }
    bool already = store.exists(r[1]);
    if (nx && already) return resp::nil_bulk();
    if (xx && !already) return resp::nil_bulk();

    Entry& e = store.get_or_create(r[1], ValueType::String);
    e.value = Value{};
    e.value.type = ValueType::String;
    e.value.str = r[2];
    if (ex_millis) {
        e.expires_at = std::chrono::system_clock::now() + std::chrono::milliseconds(*ex_millis);
    } else {
        e.expires_at.reset();
    }
    return resp::simple_string("OK");
}

std::string cmd_get(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("get");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::nil_bulk();
    if (e->value.type != ValueType::String) return err_wrongtype();
    return resp::bulk_string(e->value.str);
}

std::string cmd_getset(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("getset");
    Entry& e = store.get_or_create(r[1], ValueType::String);
    if (e.value.type != ValueType::String && !e.value.str.empty()) return err_wrongtype();
    std::string old = e.value.type == ValueType::String ? e.value.str : "";
    bool had_value = e.value.type == ValueType::String;
    e.value = Value{};
    e.value.type = ValueType::String;
    e.value.str = r[2];
    e.expires_at.reset();
    return had_value ? resp::bulk_string(old) : resp::nil_bulk();
}

std::string cmd_append(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("append");
    Entry& e = store.get_or_create(r[1], ValueType::String);
    if (e.value.type != ValueType::String) return err_wrongtype();
    e.value.str += r[2];
    return resp::integer(static_cast<int64_t>(e.value.str.size()));
}

std::string cmd_strlen(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("strlen");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::String) return err_wrongtype();
    return resp::integer(static_cast<int64_t>(e->value.str.size()));
}

std::string incr_by(Store& store, const std::string& key, int64_t delta) {
    Entry& e = store.get_or_create(key, ValueType::String);
    if (e.value.type != ValueType::String) return err_wrongtype();
    int64_t cur = 0;
    if (!e.value.str.empty()) {
        auto v = parse_int(e.value.str);
        if (!v) return err_notint();
        cur = *v;
    }
    cur += delta;
    e.value.str = std::to_string(cur);
    return resp::integer(cur);
}

std::string cmd_mset(Store& store, const Request& r) {
    if (r.size() < 3 || r.size() % 2 != 1) return err_args("mset");
    for (size_t i = 1; i + 1 < r.size(); i += 2) {
        Entry& e = store.get_or_create(r[i], ValueType::String);
        e.value = Value{};
        e.value.type = ValueType::String;
        e.value.str = r[i + 1];
        e.expires_at.reset();
    }
    return resp::simple_string("OK");
}

std::string cmd_mget(Store& store, const Request& r) {
    if (r.size() < 2) return err_args("mget");
    std::vector<std::string> items;
    for (size_t i = 1; i < r.size(); ++i) {
        Entry* e = store.get_mut(r[i]);
        if (e && e->value.type == ValueType::String) {
            items.push_back(resp::bulk_string(e->value.str));
        } else {
            items.push_back(resp::nil_bulk());
        }
    }
    return resp::array(items);
}

// ---- Generic / key management -------------------------------------------

std::string cmd_del(Store& store, const Request& r) {
    if (r.size() < 2) return err_args("del");
    int64_t count = 0;
    for (size_t i = 1; i < r.size(); ++i) {
        if (store.del(r[i])) ++count;
    }
    return resp::integer(count);
}

std::string cmd_exists(Store& store, const Request& r) {
    if (r.size() < 2) return err_args("exists");
    int64_t count = 0;
    for (size_t i = 1; i < r.size(); ++i) {
        if (store.exists(r[i])) ++count;
    }
    return resp::integer(count);
}

std::string cmd_type(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("type");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::simple_string("none");
    return resp::simple_string(value_type_name(e->value.type));
}

std::string cmd_expire(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("expire");
    auto v = parse_int(r[2]);
    if (!v) return err_notint();
    return resp::integer(store.expire(r[1], *v) ? 1 : 0);
}

std::string cmd_pexpire(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("pexpire");
    auto v = parse_int(r[2]);
    if (!v) return err_notint();
    return resp::integer(store.pexpire(r[1], *v) ? 1 : 0);
}

std::string cmd_ttl(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("ttl");
    return resp::integer(store.ttl_seconds(r[1]));
}

std::string cmd_pttl(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("pttl");
    return resp::integer(store.ttl_millis(r[1]));
}

std::string cmd_persist(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("persist");
    return resp::integer(store.persist(r[1]) ? 1 : 0);
}

std::string cmd_keys(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("keys");
    std::vector<std::string> items;
    store.for_each([&](const std::string& k, const Entry&) {
        if (glob_match(r[1].c_str(), k.c_str())) items.push_back(resp::bulk_string(k));
    });
    return resp::array(items);
}

std::string cmd_flushall(Store& store, const Request&) {
    store.flush_all();
    return resp::simple_string("OK");
}

std::string cmd_dbsize(Store& store, const Request&) {
    return resp::integer(static_cast<int64_t>(store.size()));
}

// ---- Hash commands --------------------------------------------------------

std::string cmd_hset(Store& store, const Request& r) {
    if (r.size() < 4 || r.size() % 2 != 0) return err_args("hset");
    Entry& e = store.get_or_create(r[1], ValueType::Hash);
    if (e.value.type != ValueType::Hash) return err_wrongtype();
    int64_t added = 0;
    for (size_t i = 2; i + 1 < r.size(); i += 2) {
        auto [it, inserted] = e.value.hash.insert_or_assign(r[i], r[i + 1]);
        (void)it;
        if (inserted) ++added;
    }
    return resp::integer(added);
}

std::string cmd_hget(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("hget");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::nil_bulk();
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    auto it = e->value.hash.find(r[2]);
    if (it == e->value.hash.end()) return resp::nil_bulk();
    return resp::bulk_string(it->second);
}

std::string cmd_hdel(Store& store, const Request& r) {
    if (r.size() < 3) return err_args("hdel");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    int64_t count = 0;
    for (size_t i = 2; i < r.size(); ++i) {
        count += e->value.hash.erase(r[i]);
    }
    return resp::integer(count);
}

std::string cmd_hexists(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("hexists");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    return resp::integer(e->value.hash.count(r[2]) ? 1 : 0);
}

std::string cmd_hgetall(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("hgetall");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    std::vector<std::string> items;
    for (auto& [k, v] : e->value.hash) {
        items.push_back(resp::bulk_string(k));
        items.push_back(resp::bulk_string(v));
    }
    return resp::array(items);
}

std::string cmd_hkeys(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("hkeys");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    std::vector<std::string> items;
    for (auto& [k, v] : e->value.hash) items.push_back(resp::bulk_string(k));
    return resp::array(items);
}

std::string cmd_hvals(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("hvals");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    std::vector<std::string> items;
    for (auto& [k, v] : e->value.hash) items.push_back(resp::bulk_string(v));
    return resp::array(items);
}

std::string cmd_hlen(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("hlen");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Hash) return err_wrongtype();
    return resp::integer(static_cast<int64_t>(e->value.hash.size()));
}

std::string cmd_hincrby(Store& store, const Request& r) {
    if (r.size() != 4) return err_args("hincrby");
    auto delta = parse_int(r[3]);
    if (!delta) return err_notint();
    Entry& e = store.get_or_create(r[1], ValueType::Hash);
    if (e.value.type != ValueType::Hash) return err_wrongtype();
    auto it = e.value.hash.find(r[2]);
    int64_t cur = 0;
    if (it != e.value.hash.end()) {
        auto v = parse_int(it->second);
        if (!v) return err_notint();
        cur = *v;
    }
    cur += *delta;
    e.value.hash[r[2]] = std::to_string(cur);
    return resp::integer(cur);
}

// ---- List commands ----------------------------------------------------

std::string cmd_lpush(Store& store, const Request& r, bool left) {
    if (r.size() < 3) return err_args(left ? "lpush" : "rpush");
    Entry& e = store.get_or_create(r[1], ValueType::List);
    if (e.value.type != ValueType::List) return err_wrongtype();
    for (size_t i = 2; i < r.size(); ++i) {
        if (left) e.value.list.push_front(r[i]);
        else e.value.list.push_back(r[i]);
    }
    return resp::integer(static_cast<int64_t>(e.value.list.size()));
}

std::string cmd_lpop(Store& store, const Request& r, bool left) {
    if (r.size() != 2) return err_args(left ? "lpop" : "rpop");
    Entry* e = store.get_mut(r[1]);
    if (!e || e->value.list.empty()) return resp::nil_bulk();
    if (e->value.type != ValueType::List) return err_wrongtype();
    std::string v = left ? e->value.list.front() : e->value.list.back();
    if (left) e->value.list.pop_front();
    else e->value.list.pop_back();
    return resp::bulk_string(v);
}

std::string cmd_llen(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("llen");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::List) return err_wrongtype();
    return resp::integer(static_cast<int64_t>(e->value.list.size()));
}

int64_t normalize_index(int64_t idx, int64_t len) {
    if (idx < 0) idx += len;
    return idx;
}

std::string cmd_lrange(Store& store, const Request& r) {
    if (r.size() != 4) return err_args("lrange");
    auto start = parse_int(r[2]);
    auto stop = parse_int(r[3]);
    if (!start || !stop) return err_notint();
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::List) return err_wrongtype();
    int64_t len = static_cast<int64_t>(e->value.list.size());
    int64_t s = std::clamp(normalize_index(*start, len), int64_t{0}, len);
    int64_t t = std::clamp(normalize_index(*stop, len), int64_t{-1}, len - 1);
    std::vector<std::string> items;
    if (s <= t) {
        for (int64_t i = s; i <= t; ++i) items.push_back(resp::bulk_string(e->value.list[i]));
    }
    return resp::array(items);
}

std::string cmd_lindex(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("lindex");
    auto idx = parse_int(r[2]);
    if (!idx) return err_notint();
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::nil_bulk();
    if (e->value.type != ValueType::List) return err_wrongtype();
    int64_t len = static_cast<int64_t>(e->value.list.size());
    int64_t i = normalize_index(*idx, len);
    if (i < 0 || i >= len) return resp::nil_bulk();
    return resp::bulk_string(e->value.list[i]);
}

// ---- Set commands ----------------------------------------------------

std::string cmd_sadd(Store& store, const Request& r) {
    if (r.size() < 3) return err_args("sadd");
    Entry& e = store.get_or_create(r[1], ValueType::Set);
    if (e.value.type != ValueType::Set) return err_wrongtype();
    int64_t added = 0;
    for (size_t i = 2; i < r.size(); ++i) {
        if (e.value.set.insert(r[i]).second) ++added;
    }
    return resp::integer(added);
}

std::string cmd_srem(Store& store, const Request& r) {
    if (r.size() < 3) return err_args("srem");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Set) return err_wrongtype();
    int64_t count = 0;
    for (size_t i = 2; i < r.size(); ++i) count += e->value.set.erase(r[i]);
    return resp::integer(count);
}

std::string cmd_sismember(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("sismember");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Set) return err_wrongtype();
    return resp::integer(e->value.set.count(r[2]) ? 1 : 0);
}

std::string cmd_smembers(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("smembers");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::Set) return err_wrongtype();
    std::vector<std::string> items;
    for (auto& m : e->value.set) items.push_back(resp::bulk_string(m));
    return resp::array(items);
}

std::string cmd_scard(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("scard");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::Set) return err_wrongtype();
    return resp::integer(static_cast<int64_t>(e->value.set.size()));
}

// ---- Sorted set commands -----------------------------------------------

std::string cmd_zadd(Store& store, const Request& r) {
    if (r.size() < 4 || r.size() % 2 != 0) return err_args("zadd");
    Entry& e = store.get_or_create(r[1], ValueType::ZSet);
    if (e.value.type != ValueType::ZSet) return err_wrongtype();
    int64_t added = 0;
    for (size_t i = 2; i + 1 < r.size(); i += 2) {
        auto score = parse_double(r[i]);
        if (!score) return err_notfloat();
        bool existed = e.value.zscores.count(r[i + 1]) > 0;
        e.value.zset_put(r[i + 1], *score);
        if (!existed) ++added;
    }
    return resp::integer(added);
}

std::string cmd_zscore(Store& store, const Request& r) {
    if (r.size() != 3) return err_args("zscore");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::nil_bulk();
    if (e->value.type != ValueType::ZSet) return err_wrongtype();
    auto it = e->value.zscores.find(r[2]);
    if (it == e->value.zscores.end()) return resp::nil_bulk();
    std::string s = std::to_string(it->second);
    return resp::bulk_string(s);
}

std::string cmd_zrem(Store& store, const Request& r) {
    if (r.size() < 3) return err_args("zrem");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::ZSet) return err_wrongtype();
    int64_t count = 0;
    for (size_t i = 2; i < r.size(); ++i) {
        if (e->value.zset_remove(r[i])) ++count;
    }
    return resp::integer(count);
}

std::string cmd_zcard(Store& store, const Request& r) {
    if (r.size() != 2) return err_args("zcard");
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::integer(0);
    if (e->value.type != ValueType::ZSet) return err_wrongtype();
    return resp::integer(static_cast<int64_t>(e->value.zscores.size()));
}

std::string cmd_zincrby(Store& store, const Request& r) {
    if (r.size() != 4) return err_args("zincrby");
    auto delta = parse_double(r[2]);
    if (!delta) return err_notfloat();
    Entry& e = store.get_or_create(r[1], ValueType::ZSet);
    if (e.value.type != ValueType::ZSet) return err_wrongtype();
    double cur = 0;
    auto it = e.value.zscores.find(r[3]);
    if (it != e.value.zscores.end()) cur = it->second;
    cur += *delta;
    e.value.zset_put(r[3], cur);
    return resp::bulk_string(std::to_string(cur));
}

std::string cmd_zrange(Store& store, const Request& r) {
    if (r.size() < 4) return err_args("zrange");
    auto start = parse_int(r[2]);
    auto stop = parse_int(r[3]);
    if (!start || !stop) return err_notint();
    bool withscores = r.size() >= 5 && upper(r[4]) == "WITHSCORES";
    Entry* e = store.get_mut(r[1]);
    if (!e) return resp::empty_array();
    if (e->value.type != ValueType::ZSet) return err_wrongtype();

    int64_t len = static_cast<int64_t>(e->value.zorder.size());
    int64_t s = std::clamp(normalize_index(*start, len), int64_t{0}, len);
    int64_t t = std::clamp(normalize_index(*stop, len), int64_t{-1}, len - 1);

    std::vector<std::string> items;
    if (s <= t) {
        int64_t idx = 0;
        for (auto& [score, member] : e->value.zorder) {
            if (idx > t) break;
            if (idx >= s) {
                items.push_back(resp::bulk_string(member));
                if (withscores) items.push_back(resp::bulk_string(std::to_string(score)));
            }
            ++idx;
        }
    }
    return resp::array(items);
}

// ---- Server / connection commands --------------------------------------

std::string cmd_ping(const Request& r) {
    if (r.size() >= 2) return resp::bulk_string(r[1]);
    return resp::simple_string("PONG");
}

std::string cmd_echo(const Request& r) {
    if (r.size() != 2) return err_args("echo");
    return resp::bulk_string(r[1]);
}

std::string cmd_info(Server& server) {
    std::string info =
        "# Server\r\n"
        "dinodb_version:0.1.0\r\n"
        "# Stats\r\n"
        "connected_clients:" + std::to_string(server.connected_clients.load()) + "\r\n" +
        "commands_processed:" + std::to_string(server.commands_processed.load()) + "\r\n" +
        "# Keyspace\r\n"
        "db0:keys=" + std::to_string(server.store().size()) + "\r\n";
    return resp::bulk_string(info);
}

}  // namespace

std::string dispatch(Store& store, Server& server, const Request& req) {
    if (req.empty()) return "";
    std::string cmd = upper(req[0]);

    if (cmd == "PING") return cmd_ping(req);
    if (cmd == "ECHO") return cmd_echo(req);
    if (cmd == "SELECT") return resp::simple_string("OK");
    if (cmd == "QUIT") return resp::simple_string("OK");
    if (cmd == "INFO") return cmd_info(server);
    if (cmd == "DBSIZE") return cmd_dbsize(store, req);
    if (cmd == "FLUSHALL" || cmd == "FLUSHDB") return cmd_flushall(store, req);
    if (cmd == "SAVE" || cmd == "BGSAVE") return server.save_snapshot() ? resp::simple_string("OK") : resp::error("ERR save failed");
    if (cmd == "COMMAND") return resp::empty_array();
    if (cmd == "CONFIG") return req.size() >= 2 && upper(req[1]) == "GET" ? resp::empty_array() : resp::simple_string("OK");

    if (cmd == "DEL") return cmd_del(store, req);
    if (cmd == "EXISTS") return cmd_exists(store, req);
    if (cmd == "TYPE") return cmd_type(store, req);
    if (cmd == "EXPIRE") return cmd_expire(store, req);
    if (cmd == "PEXPIRE") return cmd_pexpire(store, req);
    if (cmd == "TTL") return cmd_ttl(store, req);
    if (cmd == "PTTL") return cmd_pttl(store, req);
    if (cmd == "PERSIST") return cmd_persist(store, req);
    if (cmd == "KEYS") return cmd_keys(store, req);

    if (cmd == "SET") return cmd_set(store, req);
    if (cmd == "GET") return cmd_get(store, req);
    if (cmd == "GETSET") return cmd_getset(store, req);
    if (cmd == "APPEND") return cmd_append(store, req);
    if (cmd == "STRLEN") return cmd_strlen(store, req);
    if (cmd == "MSET") return cmd_mset(store, req);
    if (cmd == "MGET") return cmd_mget(store, req);
    if (cmd == "INCR") return req.size() == 2 ? incr_by(store, req[1], 1) : err_args("incr");
    if (cmd == "DECR") return req.size() == 2 ? incr_by(store, req[1], -1) : err_args("decr");
    if (cmd == "INCRBY") {
        if (req.size() != 3) return err_args("incrby");
        auto v = parse_int(req[2]);
        if (!v) return err_notint();
        return incr_by(store, req[1], *v);
    }
    if (cmd == "DECRBY") {
        if (req.size() != 3) return err_args("decrby");
        auto v = parse_int(req[2]);
        if (!v) return err_notint();
        return incr_by(store, req[1], -*v);
    }

    if (cmd == "HSET") return cmd_hset(store, req);
    if (cmd == "HGET") return cmd_hget(store, req);
    if (cmd == "HDEL") return cmd_hdel(store, req);
    if (cmd == "HEXISTS") return cmd_hexists(store, req);
    if (cmd == "HGETALL") return cmd_hgetall(store, req);
    if (cmd == "HKEYS") return cmd_hkeys(store, req);
    if (cmd == "HVALS") return cmd_hvals(store, req);
    if (cmd == "HLEN") return cmd_hlen(store, req);
    if (cmd == "HINCRBY") return cmd_hincrby(store, req);

    if (cmd == "LPUSH") return cmd_lpush(store, req, true);
    if (cmd == "RPUSH") return cmd_lpush(store, req, false);
    if (cmd == "LPOP") return cmd_lpop(store, req, true);
    if (cmd == "RPOP") return cmd_lpop(store, req, false);
    if (cmd == "LLEN") return cmd_llen(store, req);
    if (cmd == "LRANGE") return cmd_lrange(store, req);
    if (cmd == "LINDEX") return cmd_lindex(store, req);

    if (cmd == "SADD") return cmd_sadd(store, req);
    if (cmd == "SREM") return cmd_srem(store, req);
    if (cmd == "SISMEMBER") return cmd_sismember(store, req);
    if (cmd == "SMEMBERS") return cmd_smembers(store, req);
    if (cmd == "SCARD") return cmd_scard(store, req);

    if (cmd == "ZADD") return cmd_zadd(store, req);
    if (cmd == "ZSCORE") return cmd_zscore(store, req);
    if (cmd == "ZREM") return cmd_zrem(store, req);
    if (cmd == "ZCARD") return cmd_zcard(store, req);
    if (cmd == "ZINCRBY") return cmd_zincrby(store, req);
    if (cmd == "ZRANGE") return cmd_zrange(store, req);

    return resp::error("ERR unknown command '" + req[0] + "'");
}

}  // namespace dinodb
