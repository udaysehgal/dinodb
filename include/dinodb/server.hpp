#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dinodb/store.hpp"

namespace dinodb {

struct ServerConfig {
    std::string bind_addr = "0.0.0.0";
    uint16_t port = 7379;
    int worker_threads = 0;         // 0 -> hardware_concurrency
    std::string rdb_path = "dinodb.rdb";
    int autosave_interval_sec = 300;  // 0 disables periodic autosave
};

class Server {
public:
    explicit Server(ServerConfig cfg);
    ~Server();

    // Blocks running the accept loop + worker pool until stop() is called
    // or a fatal error occurs. Returns an exit-style status code.
    int run();
    void stop();

    Store& store() { return store_; }
    const ServerConfig& config() const { return cfg_; }

    // Serialized: SAVE from a client and the autosave timer never write the
    // snapshot file at the same time.
    bool save_snapshot();

    int64_t uptime_seconds() const;

    std::atomic<uint64_t> commands_processed{0};
    std::atomic<uint32_t> connected_clients{0};
    std::atomic<uint64_t> expired_keys{0};

private:
    ServerConfig cfg_;
    Store store_;
    std::atomic<bool> running_{false};
    std::mutex save_mtx_;
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();

    // Background housekeeping: active expiry plus periodic autosave.
    void cron_loop();
    std::thread cron_thread_;
};

}  // namespace dinodb
