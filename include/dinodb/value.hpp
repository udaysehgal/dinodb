#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dinodb {

enum class ValueType { String, Hash, List, Set, ZSet };

inline const char* value_type_name(ValueType t) {
    switch (t) {
        case ValueType::String: return "string";
        case ValueType::Hash: return "hash";
        case ValueType::List: return "list";
        case ValueType::Set: return "set";
        case ValueType::ZSet: return "zset";
    }
    return "none";
}

// A single stored value. DinoDB keeps one struct per key rather than a
// tagged union of heap containers: it costs a bit more memory per key than
// FyroDB's inline/allocated hybrid layout, but it keeps the ownership and
// locking story simple, which is the tradeoff this project is making.
struct Value {
    ValueType type = ValueType::String;

    std::string str;
    std::unordered_map<std::string, std::string> hash;
    std::deque<std::string> list;
    std::unordered_set<std::string> set;

    // Sorted set: member -> score, plus a score-ordered index for range scans.
    std::unordered_map<std::string, double> zscores;
    std::multimap<double, std::string> zorder;

    void zset_put(const std::string& member, double score) {
        auto it = zscores.find(member);
        if (it != zscores.end()) {
            auto range = zorder.equal_range(it->second);
            for (auto r = range.first; r != range.second; ++r) {
                if (r->second == member) {
                    zorder.erase(r);
                    break;
                }
            }
            it->second = score;
        } else {
            zscores.emplace(member, score);
        }
        zorder.emplace(score, member);
    }

    bool zset_remove(const std::string& member) {
        auto it = zscores.find(member);
        if (it == zscores.end()) return false;
        auto range = zorder.equal_range(it->second);
        for (auto r = range.first; r != range.second; ++r) {
            if (r->second == member) {
                zorder.erase(r);
                break;
            }
        }
        zscores.erase(it);
        return true;
    }
};

using TimePoint = std::chrono::system_clock::time_point;

struct Entry {
    Value value;
    std::optional<TimePoint> expires_at;

    bool expired(TimePoint now) const {
        return expires_at.has_value() && *expires_at <= now;
    }
};

}  // namespace dinodb
