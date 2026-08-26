#pragma once

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
    std::shared_mutex mtx;
    std::unordered_map<std::string, Entry> map;
};

class Store {
public:
    explicit Store(size_t shard_count = 32);

    Shard& shard_for(const std::string& key);
    size_t shard_count() const { return shards_.size(); }

    // Returns nullptr if missing or expired (lazy expiry).
    Entry* get_mut(const std::string& key);
    const Entry* get(const std::string& key);

    Entry& get_or_create(const std::string& key, ValueType type);

    bool del(const std::string& key);
    bool exists(const std::string& key);
    bool expire(const std::string& key, int64_t seconds);
    bool pexpire(const std::string& key, int64_t millis);
    // -2 no key, -1 no ttl, else seconds remaining
    int64_t ttl_seconds(const std::string& key);
    int64_t ttl_millis(const std::string& key);
    bool persist(const std::string& key);

    void flush_all();

    // Used only by the persistence loader at startup (single-threaded),
    // but still takes the shard lock for consistency.
    void restore(const std::string& key, Entry entry);

    // Calls fn(key, entry) for every live (non-expired) key across all
    // shards. Takes a shared lock per shard for the duration of its scan.
    void for_each(const std::function<void(const std::string&, const Entry&)>& fn);

    size_t size();

private:
    std::vector<std::unique_ptr<Shard>> shards_;
    std::hash<std::string> hasher_;
};

}  // namespace dinodb
