#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dinodb/value.hpp"

namespace dinodb {

// DinoDB's concurrency model is deliberately different from a lock-free /
// epoch-reclaimed design: keys are hashed into a fixed number of shards,
// each guarded by its own std::shared_mutex. Reads take a shared lock,
// writes take an exclusive lock, and only the single shard involved in a
// key operation is ever blocked, so unrelated keys never contend.
class Shard {
public:
    using Map = std::unordered_map<std::string, Entry>;
    std::shared_mutex mtx;
    Map map;
};

inline TimePoint now() { return std::chrono::system_clock::now(); }

// A key's slot inside its shard, handed to Store::write callbacks while the
// shard's exclusive lock is held. Expired entries are already gone by the
// time the callback sees the slot. On destruction (still under the lock) a
// container that was emptied is deleted, matching Redis semantics.
class Slot {
public:
    Slot(Shard::Map& map, const std::string& key);
    ~Slot();
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    // nullptr if the key does not exist.
    Entry* get() { return it_ == map_.end() ? nullptr : &it_->second; }
    // The existing entry (of whatever type), or a new empty one of `type`.
    Entry& get_or_create(ValueType type);
    // Replaces any existing entry with a fresh one of `type`, without a TTL.
    Entry& reset(ValueType type);
    void remove();

private:
    Shard::Map& map_;
    const std::string& key_;
    Shard::Map::iterator it_;
};

class Store {
public:
    explicit Store(size_t shard_count = 32);

    Shard& shard_for(const std::string& key);
    size_t shard_count() const { return shards_.size(); }

    // Runs fn(const Entry*) with the key's shard held under a shared lock.
    // The pointer is null if the key is missing or expired, and must not
    // escape fn: once fn returns, another thread may modify or free it.
    template <typename F>
    auto read(const std::string& key, F&& fn) {
        Shard& s = shard_for(key);
        std::shared_lock lock(s.mtx);
        auto it = s.map.find(key);
        const Entry* e = (it == s.map.end() || it->second.expired(now())) ? nullptr : &it->second;
        return fn(e);
    }

    // Runs fn(Slot&) with the key's shard held under an exclusive lock, so
    // a read-modify-write inside fn (INCR, SET NX, LPOP, ...) is atomic.
    template <typename F>
    auto write(const std::string& key, F&& fn) {
        Shard& s = shard_for(key);
        std::unique_lock lock(s.mtx);
        Slot slot(s.map, key);
        return fn(slot);
    }

    bool del(const std::string& key);
    bool exists(const std::string& key);
    // Non-positive TTLs delete the key, as in Redis. Returns false if the
    // key does not exist.
    bool expire(const std::string& key, int64_t seconds);
    bool pexpire(const std::string& key, int64_t millis);
    // -2 no key, -1 no ttl, else time remaining
    int64_t ttl_seconds(const std::string& key);
    int64_t ttl_millis(const std::string& key);
    bool persist(const std::string& key);

    void flush_all();

    // Used only by the persistence loader at startup.
    void restore(const std::string& key, Entry entry);

    // Calls fn(key, entry) for every live (non-expired) key across all
    // shards. Takes a shared lock per shard for the duration of its scan.
    void for_each(const std::function<void(const std::string&, const Entry&)>& fn);

    // Erases expired keys from one shard (round-robin across calls) and
    // returns how many were removed. Called periodically so keys that are
    // never touched again still release their memory.
    size_t sweep_expired_step();

    size_t size();

private:
    std::vector<std::unique_ptr<Shard>> shards_;
    std::hash<std::string> hasher_;
    std::atomic<size_t> sweep_cursor_{0};
};

}  // namespace dinodb
