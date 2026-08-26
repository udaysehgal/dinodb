#include "dinodb/store.hpp"

#include <chrono>

namespace dinodb {

Store::Store(size_t shard_count) {
    shards_.reserve(shard_count);
    for (size_t i = 0; i < shard_count; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
}

Shard& Store::shard_for(const std::string& key) {
    size_t h = hasher_(key);
    return *shards_[h % shards_.size()];
}

Entry* Store::get_mut(const std::string& key) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return nullptr;
    if (it->second.expired(std::chrono::system_clock::now())) {
        s.map.erase(it);
        return nullptr;
    }
    return &it->second;
}

const Entry* Store::get(const std::string& key) {
    return get_mut(key);
}

Entry& Store::get_or_create(const std::string& key, ValueType type) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    auto now = std::chrono::system_clock::now();
    if (it != s.map.end() && it->second.expired(now)) {
        s.map.erase(it);
        it = s.map.end();
    }
    if (it == s.map.end()) {
        Entry e;
        e.value.type = type;
        it = s.map.emplace(key, std::move(e)).first;
    }
    return it->second;
}

bool Store::del(const std::string& key) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    bool live = !it->second.expired(std::chrono::system_clock::now());
    s.map.erase(it);
    return live;
}

bool Store::exists(const std::string& key) {
    return get_mut(key) != nullptr;
}

bool Store::expire(const std::string& key, int64_t seconds) {
    return pexpire(key, seconds * 1000);
}

bool Store::pexpire(const std::string& key, int64_t millis) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return false;
    if (it->second.expired(std::chrono::system_clock::now())) {
        s.map.erase(it);
        return false;
    }
    it->second.expires_at = std::chrono::system_clock::now() + std::chrono::milliseconds(millis);
    return true;
}

int64_t Store::ttl_seconds(const std::string& key) {
    int64_t ms = ttl_millis(key);
    if (ms < 0) return ms;
    return (ms + 999) / 1000;
}

int64_t Store::ttl_millis(const std::string& key) {
    Shard& s = shard_for(key);
    std::shared_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end()) return -2;
    auto now = std::chrono::system_clock::now();
    if (it->second.expired(now)) return -2;
    if (!it->second.expires_at.has_value()) return -1;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*it->second.expires_at - now).count();
    return remaining < 0 ? 0 : remaining;
}

bool Store::persist(const std::string& key) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    auto it = s.map.find(key);
    if (it == s.map.end() || it->second.expired(std::chrono::system_clock::now())) return false;
    if (!it->second.expires_at.has_value()) return false;
    it->second.expires_at.reset();
    return true;
}

void Store::restore(const std::string& key, Entry entry) {
    Shard& s = shard_for(key);
    std::unique_lock lock(s.mtx);
    s.map[key] = std::move(entry);
}

void Store::flush_all() {
    for (auto& s : shards_) {
        std::unique_lock lock(s->mtx);
        s->map.clear();
    }
}

void Store::for_each(const std::function<void(const std::string&, const Entry&)>& fn) {
    auto now = std::chrono::system_clock::now();
    for (auto& s : shards_) {
        std::shared_lock lock(s->mtx);
        for (auto& [k, e] : s->map) {
            if (!e.expired(now)) fn(k, e);
        }
    }
}

size_t Store::size() {
    size_t total = 0;
    auto now = std::chrono::system_clock::now();
    for (auto& s : shards_) {
        std::shared_lock lock(s->mtx);
        for (auto& [k, e] : s->map) {
            if (!e.expired(now)) ++total;
        }
    }
    return total;
}

}  // namespace dinodb
