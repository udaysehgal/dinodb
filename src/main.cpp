#include <cstdlib>
#include <cstring>
#include <csignal>
#include <iostream>
#include <string>

#include "dinodb/persistence.hpp"
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

// Parses a non-negative integer no larger than max, or exits with an error.
int parse_num(const std::string& what, const std::string& s, long max) {
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || v < 0 || v > max) {
        std::cerr << "dinodb: invalid value '" << s << "' for " << what << "\n";
        std::exit(2);
    }
    return static_cast<int>(v);
}

int env_or_int(const char* name, int fallback, long max) {
    const char* v = std::getenv(name);
    return v ? parse_num(name, v, max) : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    dinodb::ServerConfig cfg;
    cfg.bind_addr = env_or("DINODB_BIND", cfg.bind_addr);
    cfg.port = static_cast<uint16_t>(env_or_int("DINODB_PORT", cfg.port, 65535));
    cfg.worker_threads = env_or_int("DINODB_WORKERS", cfg.worker_threads, 1024);
    cfg.rdb_path = env_or("DINODB_RDB_PATH", cfg.rdb_path);
    cfg.autosave_interval_sec = env_or_int("DINODB_AUTOSAVE_SEC", cfg.autosave_interval_sec, 1 << 30);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(parse_num(arg, argv[++i], 65535));
        } else if (arg == "--bind" && i + 1 < argc) {
            cfg.bind_addr = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            cfg.worker_threads = parse_num(arg, argv[++i], 1024);
        } else if (arg == "--rdb" && i + 1 < argc) {
            cfg.rdb_path = argv[++i];
        } else if (arg == "--autosave" && i + 1 < argc) {
            cfg.autosave_interval_sec = parse_num(arg, argv[++i], 1 << 30);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "dinodb [--port N] [--bind ADDR] [--workers N] [--rdb PATH] [--autosave SECONDS]\n";
            return 0;
        } else {
            std::cerr << "dinodb: unknown or incomplete option '" << arg << "' (see --help)\n";
            return 2;
        }
    }

    dinodb::Server server(cfg);
    g_server = &server;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    switch (dinodb::persistence::load(server.store(), cfg.rdb_path)) {
        case dinodb::persistence::LoadResult::Ok:
            std::cout << "dinodb: loaded snapshot from " << cfg.rdb_path << "\n";
            break;
        case dinodb::persistence::LoadResult::Missing:
            break;
        case dinodb::persistence::LoadResult::Corrupt:
            // Starting empty would let the next autosave overwrite the
            // user's data, so refuse instead (as Redis does).
            std::cerr << "dinodb: snapshot " << cfg.rdb_path
                      << " is corrupt or from an incompatible version; refusing to start\n";
            return 1;
    }

    int status = server.run();
    if (status != 0) return status;

    std::cout << "dinodb: saving snapshot to " << cfg.rdb_path << " before exit\n";
    if (!server.save_snapshot()) {
        std::cerr << "dinodb: final save failed\n";
        return 1;
    }
    return 0;
}
