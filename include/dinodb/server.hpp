#pragma once

#include <atomic>
#include <cstdint>
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

    bool save_snapshot();
    bool load_snapshot();

    std::atomic<uint64_t> commands_processed{0};
    std::atomic<uint32_t> connected_clients{0};

private:
    ServerConfig cfg_;
    Store store_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;

    void autosave_loop();
    std::thread autosave_thread_;
};

}  // namespace dinodb
