// In-process microbenchmark of the request path, without the network:
// RESP parse -> dispatch -> reply, single-threaded, over a 1M-key keyspace.
// Reports nanoseconds per request (best of 3 runs); deterministic enough to
// compare optimizations that are lost in the noise of a loopback benchmark.
//
//   ./build/bench/dinodb_micro_bench [requests]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "dinodb/commands.hpp"
#include "dinodb/resp.hpp"
#include "dinodb/server.hpp"

using namespace dinodb;

namespace {

std::string encode(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (auto& a : args) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

using MakeFn = std::vector<std::string> (*)(const std::string&);

// Runs `n` requests produced by make(key) over random keys, fed to the
// parser in pipelined batches of 16 like a client would send them. Each
// batch is encoded into one reused buffer right before it is parsed, so the
// input is cache-hot (as a fresh recv() buffer is) and only the keyspace
// lookups miss the cache. Only parse + dispatch is timed.
double run(Server& srv, size_t n, size_t keyspace, MakeFn make) {
    std::mt19937_64 rng(42);
    resp::Parser parser;
    resp::Request req;
    std::string batch, out;
    size_t replies = 0;
    std::chrono::duration<double, std::nano> total{0};
    for (size_t done = 0; done < n; done += 16) {
        batch.clear();
        for (size_t i = 0; i < 16; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "key:%012zu", static_cast<size_t>(rng() % keyspace));
            batch += encode(make(key));
        }
        auto t1 = std::chrono::steady_clock::now();
        parser.feed(batch.data(), batch.size());
        while (parser.next(req)) {
            out += dispatch(srv.store(), srv, req);
            ++replies;
        }
        out.clear();  // what a successful send() does
        auto t2 = std::chrono::steady_clock::now();
        total += t2 - t1;
    }
    if (replies < n) std::fprintf(stderr, "expected %zu replies, got %zu\n", n, replies);
    return total.count() / static_cast<double>(replies);
}

}  // namespace

int main(int argc, char** argv) {
    size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 3'000'000;
    constexpr size_t kKeyspace = 1'000'000;

    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);

    struct Case {
        const char* name;
        std::vector<std::string> (*make)(const std::string&);
    };
    const Case cases[] = {
        {"SET", [](const std::string& k) { return std::vector<std::string>{"SET", k, "xxx"}; }},
        {"GET", [](const std::string& k) { return std::vector<std::string>{"GET", k}; }},
        {"INCR", [](const std::string& k) { return std::vector<std::string>{"INCR", "ctr:" + k}; }},
        {"LPUSH", [](const std::string& k) { return std::vector<std::string>{"LPUSH", "list:" + k.substr(0, 9), "v"}; }},
    };

    for (const Case& c : cases) {
        run(srv, n, kKeyspace, c.make);  // warm-up: populates the keyspace
        double best = 1e18;
        for (int i = 0; i < 3; ++i) best = std::min(best, run(srv, n, kKeyspace, c.make));
        std::printf("%-6s %7.1f ns/request\n", c.name, best);
    }
    return 0;
}
