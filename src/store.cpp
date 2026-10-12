#include "dinodb/store.hpp"

#include <chrono>

namespace dinodb {

Slot::Slot(Shard::Map& map, const std::string& key) : map_(map), key_(key), it_(map.find(key)) {
    if (it_ != map_.end() && it_->second.expired_now()) {
        map_.erase(it_);
        it_ = map_.end();
    }
}

Slot::~Slot() {
    if (it_ != map_.end() && it_->second.value.empty_container()) map_.erase(it_);
}

Entry& Slot::get_or_create(ValueType type) {
    if (it_ == map_.end()) {
        Entry e;
        e.value.type = type;
        it_ = map_.emplace(key_, std::move(e)).first;
    }
    return it_->second;
}

Entry& Slot::reset(ValueType type) {
    Entry& e = get_or_create(type);
    e = Entry{};
    e.value.type = type;
    return e;
}

void Slot::remove() {
    if (it_ == map_.end()) return;
    map_.erase(it_);
    it_ = map_.end();
}

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

bool Store::del(const std::string& key) {
    return write(key, [](Slot& slot) {
        if (!slot.get()) return false;
        slot.remove();
        return true;
    });
}

bool Store::exists(const std::string& key) {
    return read(key, [](const Entry* e) { return e != nullptr; });
}

bool Store::expire(const std::string& key, int64_t seconds) {
    // Saturate instead of overflowing; anything this large never expires in practice.
    constexpr int64_t kMaxSeconds = INT64_MAX / 1000;
    if (seconds > kMaxSeconds) seconds = kMaxSeconds;
    if (seconds < -kMaxSeconds) seconds = -kMaxSeconds;
    return pexpire(key, seconds * 1000);
}

bool Store::pexpire(const std::string& key, int64_t millis) {
    return write(key, [&](Slot& slot) {
        Entry* e = slot.get();
        if (!e) return false;
        if (millis <= 0) {
            slot.remove();
        } else {
            // Cap at ~100 years so the time_point arithmetic cannot overflow.
            constexpr int64_t kMaxMillis = int64_t{100} * 365 * 24 * 3600 * 1000;
            e->expires_at = now() + std::chrono::milliseconds(std::min(millis, kMaxMillis));
        }
        return true;
    });
}

int64_t Store::ttl_seconds(const std::string& key) {
    int64_t ms = ttl_millis(key);
    if (ms < 0) return ms;
    return (ms + 500) / 1000;  // Redis rounds to the nearest second
}

int64_t Store::ttl_millis(const std::string& key) {
    return read(key, [](const Entry* e) -> int64_t {
        if (!e) return -2;
        if (!e->expires_at.has_value()) return -1;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*e->expires_at - now()).count();
        return remaining < 0 ? 0 : remaining;
    });
}

bool Store::persist(const std::string& key) {
    return write(key, [](Slot& slot) {
        Entry* e = slot.get();
        if (!e || !e->expires_at.has_value()) return false;
        e->expires_at.reset();
        return true;
    });
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
    auto t = now();
    for (auto& s : shards_) {
        std::shared_lock lock(s->mtx);
        for (auto& [k, e] : s->map) {
            if (!e.expired(t)) fn(k, e);
        }
    }
}

size_t Store::sweep_expired_step() {
    Shard& s = *shards_[sweep_cursor_.fetch_add(1) % shards_.size()];
    auto t = now();
    std::unique_lock lock(s.mtx);
    return std::erase_if(s.map, [&](const auto& kv) { return kv.second.expired(t); });
}

size_t Store::size() {
    size_t total = 0;
    auto t = now();
    for (auto& s : shards_) {
        std::shared_lock lock(s->mtx);
        for (auto& [k, e] : s->map) {
            if (!e.expired(t)) ++total;
        }
    }
    return total;
}

}  // namespace dinodb
