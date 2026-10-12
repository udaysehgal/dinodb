#include "dinodb/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <unordered_map>

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
    int64_t v = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<double> parse_double(const std::string& s) {
    if (s.empty() || std::isspace(static_cast<unsigned char>(s[0]))) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || std::isnan(v)) return std::nullopt;
    if (errno == ERANGE && !std::isinf(v)) return std::nullopt;  // underflow is fine, overflow isn't
    return v;
}

// Shortest round-trippable representation, like Redis 7 ("1.5", not "1.500000").
std::string format_double(double v) {
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, end);
}

// Redis-style glob: *, ?, [abc], [^abc], [a-z], and \ escapes.
bool glob_match(const char* pat, const char* str) {
    while (*pat) {
        switch (*pat) {
            case '*':
                while (*pat == '*') ++pat;
                if (!*pat) return true;
                for (const char* s = str; *s; ++s) {
                    if (glob_match(pat, s)) return true;
                }
                return false;
            case '?':
                if (!*str) return false;
                break;
            case '[': {
                if (!*str) return false;
                ++pat;
                bool negate = *pat == '^';
                if (negate) ++pat;
                bool matched = false;
                while (*pat && *pat != ']') {
                    if (*pat == '\\' && pat[1]) {
                        ++pat;
                        if (*pat == *str) matched = true;
                    } else if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                        char lo = std::min(pat[0], pat[2]), hi = std::max(pat[0], pat[2]);
                        if (*str >= lo && *str <= hi) matched = true;
                        pat += 2;
                    } else if (*pat == *str) {
                        matched = true;
                    }
                    ++pat;
                }
                if (matched == negate) return false;
                if (!*pat) return true;  // unterminated class: treat rest as matched, like Redis
                break;
            }
            case '\\':
                if (pat[1]) ++pat;
                [[fallthrough]];
            default:
                if (*pat != *str) return false;
                break;
        }
        ++pat;
        ++str;
    }
    return *str == '\0';
}

std::string err_wrongtype() {
    return resp::error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::string err_notint() { return resp::error("ERR value is not an integer or out of range"); }
std::string err_notfloat() { return resp::error("ERR value is not a valid float"); }
std::string err_syntax() { return resp::error("ERR syntax error"); }

struct Ctx {
    Store& store;
    Server& server;
};

// ---- String commands ----------------------------------------------------

std::string cmd_set(Ctx& c, const Request& r) {
    std::optional<int64_t> ex_millis;
    bool nx = false, xx = false;
    for (size_t i = 3; i < r.size(); ++i) {
        std::string opt = upper(r[i]);
        if ((opt == "EX" || opt == "PX") && i + 1 < r.size() && !ex_millis) {
            auto v = parse_int(r[++i]);
            if (!v) return err_notint();
            if (*v <= 0 || (opt == "EX" && *v > INT64_MAX / 1000)) {
                return resp::error("ERR invalid expire time in 'set' command");
            }
            ex_millis = opt == "EX" ? *v * 1000 : *v;
        } else if (opt == "NX" && !xx) {
            nx = true;
        } else if (opt == "XX" && !nx) {
            xx = true;
        } else {
            return err_syntax();
        }
    }
    return c.store.write(r[1], [&](Slot& slot) {
        bool already = slot.get() != nullptr;
        if ((nx && already) || (xx && !already)) return resp::nil_bulk();
        Entry& e = slot.reset(ValueType::String);
        e.value.str = r[2];
        if (ex_millis) e.expires_at = now() + std::chrono::milliseconds(*ex_millis);
        return resp::simple_string("OK");
    });
}

std::string cmd_get(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::nil_bulk();
        if (e->value.type != ValueType::String) return err_wrongtype();
        return resp::bulk_string(e->value.str);
    });
}

std::string cmd_getset(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        std::string reply = resp::nil_bulk();
        if (Entry* e = slot.get()) {
            if (e->value.type != ValueType::String) return err_wrongtype();
            reply = resp::bulk_string(e->value.str);
        }
        slot.reset(ValueType::String).value.str = r[2];
        return reply;
    });
}

std::string cmd_append(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::String);
        if (e.value.type != ValueType::String) return err_wrongtype();
        e.value.str += r[2];
        return resp::integer(static_cast<int64_t>(e.value.str.size()));
    });
}

std::string cmd_strlen(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::String) return err_wrongtype();
        return resp::integer(static_cast<int64_t>(e->value.str.size()));
    });
}

std::string incr_by(Store& store, const std::string& key, int64_t delta) {
    return store.write(key, [&](Slot& slot) {
        Entry* e = slot.get();
        int64_t cur = 0;
        if (e) {
            if (e->value.type != ValueType::String) return err_wrongtype();
            auto v = parse_int(e->value.str);
            if (!v) return err_notint();
            cur = *v;
        }
        if (__builtin_add_overflow(cur, delta, &cur)) {
            return resp::error("ERR increment or decrement would overflow");
        }
        slot.get_or_create(ValueType::String).value.str = std::to_string(cur);
        return resp::integer(cur);
    });
}

std::string cmd_incr(Ctx& c, const Request& r) { return incr_by(c.store, r[1], 1); }
std::string cmd_decr(Ctx& c, const Request& r) { return incr_by(c.store, r[1], -1); }

std::string cmd_incrby(Ctx& c, const Request& r) {
    auto v = parse_int(r[2]);
    if (!v) return err_notint();
    return incr_by(c.store, r[1], *v);
}

std::string cmd_decrby(Ctx& c, const Request& r) {
    auto v = parse_int(r[2]);
    if (!v || *v == INT64_MIN) return err_notint();
    return incr_by(c.store, r[1], -*v);
}

// MSET/MGET touch keys in different shards one at a time, so unlike Redis
// they are not atomic as a whole; each individual key update is.
std::string cmd_mset(Ctx& c, const Request& r) {
    if (r.size() % 2 != 1) return resp::error("ERR wrong number of arguments for 'mset' command");
    for (size_t i = 1; i + 1 < r.size(); i += 2) {
        c.store.write(r[i], [&](Slot& slot) { slot.reset(ValueType::String).value.str = r[i + 1]; });
    }
    return resp::simple_string("OK");
}

std::string cmd_mget(Ctx& c, const Request& r) {
    std::vector<std::string> items;
    for (size_t i = 1; i < r.size(); ++i) {
        items.push_back(c.store.read(r[i], [](const Entry* e) {
            return e && e->value.type == ValueType::String ? resp::bulk_string(e->value.str) : resp::nil_bulk();
        }));
    }
    return resp::array(items);
}

// ---- Generic / key management -------------------------------------------

std::string cmd_del(Ctx& c, const Request& r) {
    int64_t count = 0;
    for (size_t i = 1; i < r.size(); ++i) {
        if (c.store.del(r[i])) ++count;
    }
    return resp::integer(count);
}

std::string cmd_exists(Ctx& c, const Request& r) {
    int64_t count = 0;
    for (size_t i = 1; i < r.size(); ++i) {
        if (c.store.exists(r[i])) ++count;
    }
    return resp::integer(count);
}

std::string cmd_type(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        return resp::simple_string(e ? value_type_name(e->value.type) : "none");
    });
}

std::string cmd_expire(Ctx& c, const Request& r) {
    auto v = parse_int(r[2]);
    if (!v) return err_notint();
    return resp::integer(c.store.expire(r[1], *v) ? 1 : 0);
}

std::string cmd_pexpire(Ctx& c, const Request& r) {
    auto v = parse_int(r[2]);
    if (!v) return err_notint();
    return resp::integer(c.store.pexpire(r[1], *v) ? 1 : 0);
}

std::string cmd_ttl(Ctx& c, const Request& r) { return resp::integer(c.store.ttl_seconds(r[1])); }
std::string cmd_pttl(Ctx& c, const Request& r) { return resp::integer(c.store.ttl_millis(r[1])); }
std::string cmd_persist(Ctx& c, const Request& r) { return resp::integer(c.store.persist(r[1]) ? 1 : 0); }

std::string cmd_keys(Ctx& c, const Request& r) {
    std::vector<std::string> items;
    c.store.for_each([&](const std::string& k, const Entry&) {
        if (glob_match(r[1].c_str(), k.c_str())) items.push_back(resp::bulk_string(k));
    });
    return resp::array(items);
}

std::string cmd_flushall(Ctx& c, const Request&) {
    c.store.flush_all();
    return resp::simple_string("OK");
}

std::string cmd_dbsize(Ctx& c, const Request&) { return resp::integer(static_cast<int64_t>(c.store.size())); }

// ---- Hash commands --------------------------------------------------------

std::string cmd_hset(Ctx& c, const Request& r) {
    if (r.size() % 2 != 0) return resp::error("ERR wrong number of arguments for 'hset' command");
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::Hash);
        if (e.value.type != ValueType::Hash) return err_wrongtype();
        int64_t added = 0;
        for (size_t i = 2; i + 1 < r.size(); i += 2) {
            if (e.value.hash.insert_or_assign(r[i], r[i + 1]).second) ++added;
        }
        return resp::integer(added);
    });
}

std::string cmd_hget(Ctx& c, const Request& r) {
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::nil_bulk();
        if (e->value.type != ValueType::Hash) return err_wrongtype();
        auto it = e->value.hash.find(r[2]);
        return it == e->value.hash.end() ? resp::nil_bulk() : resp::bulk_string(it->second);
    });
}

std::string cmd_hdel(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry* e = slot.get();
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Hash) return err_wrongtype();
        int64_t count = 0;
        for (size_t i = 2; i < r.size(); ++i) count += static_cast<int64_t>(e->value.hash.erase(r[i]));
        return resp::integer(count);
    });
}

std::string cmd_hexists(Ctx& c, const Request& r) {
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Hash) return err_wrongtype();
        return resp::integer(e->value.hash.count(r[2]) ? 1 : 0);
    });
}

// Shared body of HGETALL / HKEYS / HVALS.
std::string hash_dump(Ctx& c, const Request& r, bool keys, bool vals) {
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::empty_array();
        if (e->value.type != ValueType::Hash) return err_wrongtype();
        std::vector<std::string> items;
        for (auto& [k, v] : e->value.hash) {
            if (keys) items.push_back(resp::bulk_string(k));
            if (vals) items.push_back(resp::bulk_string(v));
        }
        return resp::array(items);
    });
}

std::string cmd_hgetall(Ctx& c, const Request& r) { return hash_dump(c, r, true, true); }
std::string cmd_hkeys(Ctx& c, const Request& r) { return hash_dump(c, r, true, false); }
std::string cmd_hvals(Ctx& c, const Request& r) { return hash_dump(c, r, false, true); }

std::string cmd_hlen(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Hash) return err_wrongtype();
        return resp::integer(static_cast<int64_t>(e->value.hash.size()));
    });
}

std::string cmd_hincrby(Ctx& c, const Request& r) {
    auto delta = parse_int(r[3]);
    if (!delta) return err_notint();
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::Hash);
        if (e.value.type != ValueType::Hash) return err_wrongtype();
        int64_t cur = 0;
        auto it = e.value.hash.find(r[2]);
        if (it != e.value.hash.end()) {
            auto v = parse_int(it->second);
            if (!v) return resp::error("ERR hash value is not an integer");
            cur = *v;
        }
        if (__builtin_add_overflow(cur, *delta, &cur)) {
            return resp::error("ERR increment or decrement would overflow");
        }
        e.value.hash[r[2]] = std::to_string(cur);
        return resp::integer(cur);
    });
}

// ---- List commands ----------------------------------------------------

std::string push(Ctx& c, const Request& r, bool left) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::List);
        if (e.value.type != ValueType::List) return err_wrongtype();
        for (size_t i = 2; i < r.size(); ++i) {
            if (left) e.value.list.push_front(r[i]);
            else e.value.list.push_back(r[i]);
        }
        return resp::integer(static_cast<int64_t>(e.value.list.size()));
    });
}

std::string pop(Ctx& c, const Request& r, bool left) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry* e = slot.get();
        if (!e) return resp::nil_bulk();
        if (e->value.type != ValueType::List) return err_wrongtype();
        auto& list = e->value.list;
        if (list.empty()) return resp::nil_bulk();
        std::string reply = resp::bulk_string(left ? list.front() : list.back());
        if (left) list.pop_front();
        else list.pop_back();
        return reply;
    });
}

std::string cmd_lpush(Ctx& c, const Request& r) { return push(c, r, true); }
std::string cmd_rpush(Ctx& c, const Request& r) { return push(c, r, false); }
std::string cmd_lpop(Ctx& c, const Request& r) { return pop(c, r, true); }
std::string cmd_rpop(Ctx& c, const Request& r) { return pop(c, r, false); }

std::string cmd_llen(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::List) return err_wrongtype();
        return resp::integer(static_cast<int64_t>(e->value.list.size()));
    });
}

// Clamps Redis-style [start, stop] (negative = from the end) to [0, len).
// Returns false if the resulting range is empty.
bool resolve_range(int64_t start, int64_t stop, int64_t len, int64_t& s, int64_t& t) {
    if (start < 0) start = std::max<int64_t>(start + len, 0);
    if (stop < 0) stop += len;
    if (stop >= len) stop = len - 1;
    s = start;
    t = stop;
    return s <= t && s < len;
}

std::string cmd_lrange(Ctx& c, const Request& r) {
    auto start = parse_int(r[2]);
    auto stop = parse_int(r[3]);
    if (!start || !stop) return err_notint();
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::empty_array();
        if (e->value.type != ValueType::List) return err_wrongtype();
        int64_t s, t;
        std::vector<std::string> items;
        if (resolve_range(*start, *stop, static_cast<int64_t>(e->value.list.size()), s, t)) {
            for (int64_t i = s; i <= t; ++i) items.push_back(resp::bulk_string(e->value.list[i]));
        }
        return resp::array(items);
    });
}

std::string cmd_lindex(Ctx& c, const Request& r) {
    auto idx = parse_int(r[2]);
    if (!idx) return err_notint();
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::nil_bulk();
        if (e->value.type != ValueType::List) return err_wrongtype();
        int64_t len = static_cast<int64_t>(e->value.list.size());
        int64_t i = *idx < 0 ? *idx + len : *idx;
        if (i < 0 || i >= len) return resp::nil_bulk();
        return resp::bulk_string(e->value.list[i]);
    });
}

// ---- Set commands ----------------------------------------------------

std::string cmd_sadd(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::Set);
        if (e.value.type != ValueType::Set) return err_wrongtype();
        int64_t added = 0;
        for (size_t i = 2; i < r.size(); ++i) {
            if (e.value.set.insert(r[i]).second) ++added;
        }
        return resp::integer(added);
    });
}

std::string cmd_srem(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry* e = slot.get();
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Set) return err_wrongtype();
        int64_t count = 0;
        for (size_t i = 2; i < r.size(); ++i) count += static_cast<int64_t>(e->value.set.erase(r[i]));
        return resp::integer(count);
    });
}

std::string cmd_sismember(Ctx& c, const Request& r) {
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Set) return err_wrongtype();
        return resp::integer(e->value.set.count(r[2]) ? 1 : 0);
    });
}

std::string cmd_smembers(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::empty_array();
        if (e->value.type != ValueType::Set) return err_wrongtype();
        std::vector<std::string> items;
        for (auto& m : e->value.set) items.push_back(resp::bulk_string(m));
        return resp::array(items);
    });
}

std::string cmd_scard(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::Set) return err_wrongtype();
        return resp::integer(static_cast<int64_t>(e->value.set.size()));
    });
}

// ---- Sorted set commands -----------------------------------------------

std::string cmd_zadd(Ctx& c, const Request& r) {
    if (r.size() % 2 != 0) return err_syntax();
    // Validate every score up front so a bad one doesn't leave a partial update.
    std::vector<double> scores;
    for (size_t i = 2; i + 1 < r.size(); i += 2) {
        auto score = parse_double(r[i]);
        if (!score) return err_notfloat();
        scores.push_back(*score);
    }
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::ZSet);
        if (e.value.type != ValueType::ZSet) return err_wrongtype();
        int64_t added = 0;
        for (size_t i = 0; i < scores.size(); ++i) {
            const std::string& member = r[3 + 2 * i];
            if (!e.value.zscores.count(member)) ++added;
            e.value.zset_put(member, scores[i]);
        }
        return resp::integer(added);
    });
}

std::string cmd_zscore(Ctx& c, const Request& r) {
    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::nil_bulk();
        if (e->value.type != ValueType::ZSet) return err_wrongtype();
        auto it = e->value.zscores.find(r[2]);
        return it == e->value.zscores.end() ? resp::nil_bulk() : resp::bulk_string(format_double(it->second));
    });
}

std::string cmd_zrem(Ctx& c, const Request& r) {
    return c.store.write(r[1], [&](Slot& slot) {
        Entry* e = slot.get();
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::ZSet) return err_wrongtype();
        int64_t count = 0;
        for (size_t i = 2; i < r.size(); ++i) {
            if (e->value.zset_remove(r[i])) ++count;
        }
        return resp::integer(count);
    });
}

std::string cmd_zcard(Ctx& c, const Request& r) {
    return c.store.read(r[1], [](const Entry* e) {
        if (!e) return resp::integer(0);
        if (e->value.type != ValueType::ZSet) return err_wrongtype();
        return resp::integer(static_cast<int64_t>(e->value.zscores.size()));
    });
}

std::string cmd_zincrby(Ctx& c, const Request& r) {
    auto delta = parse_double(r[2]);
    if (!delta) return err_notfloat();
    return c.store.write(r[1], [&](Slot& slot) {
        Entry& e = slot.get_or_create(ValueType::ZSet);
        if (e.value.type != ValueType::ZSet) return err_wrongtype();
        auto it = e.value.zscores.find(r[3]);
        double cur = (it != e.value.zscores.end() ? it->second : 0) + *delta;
        if (std::isnan(cur)) return resp::error("ERR resulting score is not a number (NaN)");
        e.value.zset_put(r[3], cur);
        return resp::bulk_string(format_double(cur));
    });
}

std::string cmd_zrange(Ctx& c, const Request& r) {
    auto start = parse_int(r[2]);
    auto stop = parse_int(r[3]);
    if (!start || !stop) return err_notint();
    bool withscores = false;
    if (r.size() == 5 && upper(r[4]) == "WITHSCORES") withscores = true;
    else if (r.size() > 4) return err_syntax();

    return c.store.read(r[1], [&](const Entry* e) {
        if (!e) return resp::empty_array();
        if (e->value.type != ValueType::ZSet) return err_wrongtype();
        int64_t s, t;
        std::vector<std::string> items;
        if (resolve_range(*start, *stop, static_cast<int64_t>(e->value.zorder.size()), s, t)) {
            auto it = std::next(e->value.zorder.begin(), s);
            for (int64_t i = s; i <= t; ++i, ++it) {
                items.push_back(resp::bulk_string(it->second));
                if (withscores) items.push_back(resp::bulk_string(format_double(it->first)));
            }
        }
        return resp::array(items);
    });
}

// ---- Server / connection commands --------------------------------------

std::string cmd_ping(Ctx&, const Request& r) {
    if (r.size() > 2) return resp::error("ERR wrong number of arguments for 'ping' command");
    return r.size() == 2 ? resp::bulk_string(r[1]) : resp::simple_string("PONG");
}

std::string cmd_echo(Ctx&, const Request& r) { return resp::bulk_string(r[1]); }
std::string cmd_ok(Ctx&, const Request&) { return resp::simple_string("OK"); }
std::string cmd_command(Ctx&, const Request&) { return resp::empty_array(); }

std::string cmd_select(Ctx&, const Request& r) {
    auto db = parse_int(r[1]);
    if (!db) return err_notint();
    if (*db != 0) return resp::error("ERR DB index is out of range");
    return resp::simple_string("OK");
}

std::string cmd_config(Ctx&, const Request& r) {
    return upper(r[1]) == "GET" ? resp::empty_array() : resp::simple_string("OK");
}

std::string cmd_save(Ctx& c, const Request&) {
    return c.server.save_snapshot() ? resp::simple_string("OK") : resp::error("ERR save failed");
}

std::string cmd_info(Ctx& c, const Request&) {
    std::string info =
        "# Server\r\n"
        "dinodb_version:0.2.0\r\n"
        "uptime_in_seconds:" + std::to_string(c.server.uptime_seconds()) + "\r\n" +
        "# Clients\r\n"
        "connected_clients:" + std::to_string(c.server.connected_clients.load()) + "\r\n" +
        "# Stats\r\n"
        "total_commands_processed:" + std::to_string(c.server.commands_processed.load()) + "\r\n" +
        "expired_keys:" + std::to_string(c.server.expired_keys.load()) + "\r\n" +
        "# Keyspace\r\n"
        "db0:keys=" + std::to_string(c.store.size()) + "\r\n";
    return resp::bulk_string(info);
}

// ---- Dispatch table -----------------------------------------------------

struct Command {
    std::string (*handler)(Ctx&, const Request&);
    // Redis convention: N > 0 means exactly N args (including the command
    // name), -N means at least N.
    int arity;
};

const std::unordered_map<std::string, Command>& command_table() {
    static const std::unordered_map<std::string, Command> table = {
        {"PING", {cmd_ping, -1}},
        {"ECHO", {cmd_echo, 2}},
        {"SELECT", {cmd_select, 2}},
        {"QUIT", {cmd_ok, -1}},
        {"INFO", {cmd_info, -1}},
        {"DBSIZE", {cmd_dbsize, 1}},
        {"FLUSHALL", {cmd_flushall, -1}},
        {"FLUSHDB", {cmd_flushall, -1}},
        {"SAVE", {cmd_save, 1}},
        {"BGSAVE", {cmd_save, -1}},
        {"COMMAND", {cmd_command, -1}},
        {"CONFIG", {cmd_config, -2}},

        {"DEL", {cmd_del, -2}},
        {"EXISTS", {cmd_exists, -2}},
        {"TYPE", {cmd_type, 2}},
        {"EXPIRE", {cmd_expire, 3}},
        {"PEXPIRE", {cmd_pexpire, 3}},
        {"TTL", {cmd_ttl, 2}},
        {"PTTL", {cmd_pttl, 2}},
        {"PERSIST", {cmd_persist, 2}},
        {"KEYS", {cmd_keys, 2}},

        {"SET", {cmd_set, -3}},
        {"GET", {cmd_get, 2}},
        {"GETSET", {cmd_getset, 3}},
        {"APPEND", {cmd_append, 3}},
        {"STRLEN", {cmd_strlen, 2}},
        {"MSET", {cmd_mset, -3}},
        {"MGET", {cmd_mget, -2}},
        {"INCR", {cmd_incr, 2}},
        {"DECR", {cmd_decr, 2}},
        {"INCRBY", {cmd_incrby, 3}},
        {"DECRBY", {cmd_decrby, 3}},

        {"HSET", {cmd_hset, -4}},
        {"HGET", {cmd_hget, 3}},
        {"HDEL", {cmd_hdel, -3}},
        {"HEXISTS", {cmd_hexists, 3}},
        {"HGETALL", {cmd_hgetall, 2}},
        {"HKEYS", {cmd_hkeys, 2}},
        {"HVALS", {cmd_hvals, 2}},
        {"HLEN", {cmd_hlen, 2}},
        {"HINCRBY", {cmd_hincrby, 4}},

        {"LPUSH", {cmd_lpush, -3}},
        {"RPUSH", {cmd_rpush, -3}},
        {"LPOP", {cmd_lpop, 2}},
        {"RPOP", {cmd_rpop, 2}},
        {"LLEN", {cmd_llen, 2}},
        {"LRANGE", {cmd_lrange, 4}},
        {"LINDEX", {cmd_lindex, 3}},

        {"SADD", {cmd_sadd, -3}},
        {"SREM", {cmd_srem, -3}},
        {"SISMEMBER", {cmd_sismember, 3}},
        {"SMEMBERS", {cmd_smembers, 2}},
        {"SCARD", {cmd_scard, 2}},

        {"ZADD", {cmd_zadd, -4}},
        {"ZSCORE", {cmd_zscore, 3}},
        {"ZREM", {cmd_zrem, -3}},
        {"ZCARD", {cmd_zcard, 2}},
        {"ZINCRBY", {cmd_zincrby, 4}},
        {"ZRANGE", {cmd_zrange, -4}},
    };
    return table;
}

}  // namespace

std::string dispatch(Store& store, Server& server, const Request& req) {
    if (req.empty()) return "";
    std::string name = upper(req[0]);
    auto it = command_table().find(name);
    if (it == command_table().end()) return resp::error("ERR unknown command '" + req[0] + "'");

    const Command& cmd = it->second;
    int argc = static_cast<int>(req.size());
    if ((cmd.arity > 0 && argc != cmd.arity) || (cmd.arity < 0 && argc < -cmd.arity)) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return resp::error("ERR wrong number of arguments for '" + lower + "' command");
    }

    Ctx ctx{store, server};
    return cmd.handler(ctx, req);
}

}  // namespace dinodb
