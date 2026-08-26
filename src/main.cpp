#include <cstdlib>
#include <cstring>
#include <csignal>
#include <iostream>
#include <string>

#include "dinodb/server.hpp"

namespace {

dinodb::Server* g_server = nullptr;

void handle_signal(int) {
    if (g_server) g_server->stop();
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

int env_or_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v) return fallback;
    return std::atoi(v);
}

}  // namespace

int main(int argc, char** argv) {
    dinodb::ServerConfig cfg;
    cfg.bind_addr = env_or("DINODB_BIND", cfg.bind_addr);
    cfg.port = static_cast<uint16_t>(env_or_int("DINODB_PORT", cfg.port));
    cfg.worker_threads = env_or_int("DINODB_WORKERS", cfg.worker_threads);
    cfg.rdb_path = env_or("DINODB_RDB_PATH", cfg.rdb_path);
    cfg.autosave_interval_sec = env_or_int("DINODB_AUTOSAVE_SEC", cfg.autosave_interval_sec);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--bind" && i + 1 < argc) {
            cfg.bind_addr = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            cfg.worker_threads = std::atoi(argv[++i]);
        } else if (arg == "--rdb" && i + 1 < argc) {
            cfg.rdb_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "dinodb [--port N] [--bind ADDR] [--workers N] [--rdb PATH]\n";
            return 0;
        }
    }

    dinodb::Server server(cfg);
    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (server.load_snapshot()) {
        std::cout << "dinodb: loaded snapshot from " << cfg.rdb_path << "\n";
    }

    int status = server.run();

    std::cout << "dinodb: saving snapshot to " << cfg.rdb_path << " before exit\n";
    server.save_snapshot();

    return status;
}
