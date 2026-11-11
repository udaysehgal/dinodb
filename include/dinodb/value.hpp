#pragma once

#include <chrono>
#include <deque>
#include <memory>
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

// The non-string containers, allocated only for keys that hold one.
struct Containers {
    std::unordered_map<std::string, std::string> hash;
    std::deque<std::string> list;
    std::unordered_set<std::string> set;
    // Sorted set: member -> score, plus a (score, member)-ordered index for
    // range scans. Ordering by the pair gives Redis's tie-break rule (equal
    // scores sort lexicographically by member) and O(log n) updates.
    std::unordered_map<std::string, double> zscores;
    std::set<std::pair<double, std::string>> zorder;
};

// A single stored value. Strings, by far the most common type, live
// inline; the other containers sit behind one pointer that is allocated
// on first use. That keeps a string key's hash-table node to about two
// cache lines instead of five, which matters because walking those nodes
// (cache misses) dominates the cost of a GET or SET.
struct Value {
    ValueType type = ValueType::String;
    std::string str;

    std::unordered_map<std::string, std::string>& hash() { return big().hash; }
    std::deque<std::string>& list() { return big().list; }
    std::unordered_set<std::string>& set() { return big().set; }
    std::unordered_map<std::string, double>& zscores() { return big().zscores; }
    std::set<std::pair<double, std::string>>& zorder() { return big().zorder; }

    const std::unordered_map<std::string, std::string>& hash() const { return big().hash; }
    const std::deque<std::string>& list() const { return big().list; }
    const std::unordered_set<std::string>& set() const { return big().set; }
    const std::unordered_map<std::string, double>& zscores() const { return big().zscores; }
    const std::set<std::pair<double, std::string>>& zorder() const { return big().zorder; }

    void zset_put(const std::string& member, double score) {
        auto& scores = zscores();
        auto it = scores.find(member);
        if (it != scores.end()) {
            zorder().erase({it->second, member});
            it->second = score;
        } else {
            scores.emplace(member, score);
        }
        zorder().emplace(score, member);
    }

    bool zset_remove(const std::string& member) {
        auto& scores = zscores();
        auto it = scores.find(member);
        if (it == scores.end()) return false;
        zorder().erase({it->second, member});
        scores.erase(it);
        return true;
    }

    // Redis deletes a key as soon as its container becomes empty; strings
    // are never "empty" in this sense (SET k "" is a real value).
    bool empty_container() const {
        if (type == ValueType::String) return false;
        if (!containers_) return true;
        switch (type) {
            case ValueType::String: return false;
            case ValueType::Hash: return containers_->hash.empty();
            case ValueType::List: return containers_->list.empty();
            case ValueType::Set: return containers_->set.empty();
            case ValueType::ZSet: return containers_->zscores.empty();
        }
        return false;
    }

private:
    std::unique_ptr<Containers> containers_;

    Containers& big() {
        if (!containers_) containers_ = std::make_unique<Containers>();
        return *containers_;
    }
    const Containers& big() const {
        static const Containers kEmpty;
        return containers_ ? *containers_ : kEmpty;
    }
};

using TimePoint = std::chrono::system_clock::time_point;

struct Entry {
    // First, so the expiry check reads the same cache line as the key.
    std::optional<TimePoint> expires_at;
    Value value;

    bool expired(TimePoint now) const {
        return expires_at.has_value() && *expires_at <= now;
    }

    // Reads the clock only if the entry has a TTL. That matters more than
    // it looks: on Apple Silicon reading the clock is a serializing
    // instruction that waits out the cache misses of the lookup that found
    // this entry, which made it the single largest cost of a GET/SET.
    bool expired_now() const {
        return expires_at.has_value() && *expires_at <= std::chrono::system_clock::now();
    }
};

}  // namespace dinodb
