#include <thread>
#include <vector>

#include "dinodb/commands.hpp"
#include "dinodb/server.hpp"
#include "test_framework.hpp"

using namespace dinodb;

// These tests are most useful under -DDINODB_SANITIZE=thread, where any
// access to a value outside its shard lock is reported as a data race.

TEST(concurrent_incr_loses_no_updates) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    constexpr int kThreads = 8, kOps = 5000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kOps; ++i) dispatch(srv.store(), srv, {"INCR", "ctr"});
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(dispatch(srv.store(), srv, {"GET", "ctr"}), resp::bulk_string(std::to_string(kThreads * kOps)));
}

TEST(concurrent_mixed_ops_on_shared_keys) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    constexpr int kThreads = 8, kOps = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::string me = std::to_string(t);
            for (int i = 0; i < kOps; ++i) {
                dispatch(srv.store(), srv, {"RPUSH", "list", me});
                dispatch(srv.store(), srv, {"LPOP", "list"});
                dispatch(srv.store(), srv, {"SADD", "set", me});
                dispatch(srv.store(), srv, {"ZINCRBY", "z", "1", me});
                dispatch(srv.store(), srv, {"HINCRBY", "h", "f", "1"});
                dispatch(srv.store(), srv, {"SET", "s", me, "NX"});
                dispatch(srv.store(), srv, {"DEL", "s"});
                dispatch(srv.store(), srv, {"KEYS", "*"});
            }
        });
    }
    for (auto& t : threads) t.join();
    CHECK_EQ(dispatch(srv.store(), srv, {"LLEN", "list"}), std::string(":0\r\n"));
    CHECK_EQ(dispatch(srv.store(), srv, {"SCARD", "set"}), resp::integer(kThreads));
    CHECK_EQ(dispatch(srv.store(), srv, {"HGET", "h", "f"}), resp::bulk_string(std::to_string(kThreads * kOps)));
    CHECK_EQ(dispatch(srv.store(), srv, {"ZSCORE", "z", "0"}), resp::bulk_string(std::to_string(kOps)));
}
