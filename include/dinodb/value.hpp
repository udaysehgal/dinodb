#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
// tagged union of heap containers: it costs more memory per key than an
// adaptive, size-tuned encoding would, but it keeps the ownership and
// locking story simple, which is the tradeoff this project is making.
struct Value {
    ValueType type = ValueType::String;

    std::string str;
    std::unordered_map<std::string, std::string> hash;
    std::deque<std::string> list;
    std::unordered_set<std::string> set;

    // Sorted set: member -> score, plus a (score, member)-ordered index for
    // range scans. Ordering by the pair gives Redis's tie-break rule (equal
    // scores sort lexicographically by member) and O(log n) updates.
    std::unordered_map<std::string, double> zscores;
    std::set<std::pair<double, std::string>> zorder;

    void zset_put(const std::string& member, double score) {
        auto it = zscores.find(member);
        if (it != zscores.end()) {
            zorder.erase({it->second, member});
            it->second = score;
        } else {
            zscores.emplace(member, score);
        }
        zorder.emplace(score, member);
    }

    bool zset_remove(const std::string& member) {
        auto it = zscores.find(member);
        if (it == zscores.end()) return false;
        zorder.erase({it->second, member});
        zscores.erase(it);
        return true;
    }

    // Redis deletes a key as soon as its container becomes empty; strings
    // are never "empty" in this sense (SET k "" is a real value).
    bool empty_container() const {
        switch (type) {
            case ValueType::String: return false;
            case ValueType::Hash: return hash.empty();
            case ValueType::List: return list.empty();
            case ValueType::Set: return set.empty();
            case ValueType::ZSet: return zscores.empty();
        }
        return false;
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
