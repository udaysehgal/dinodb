#include <atomic>
#include <string>
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

// MSET/MGET lock every shard they touch for the whole command, so a reader
// must never see a mix of two MSETs. Eight keys almost surely span several
// of the 32 shards.
TEST(mset_mget_are_atomic_across_shards) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    const std::vector<std::string> keys = {"k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7"};
    auto mset = [&](const std::string& v) {
        resp::Request req = {"MSET"};
        for (const auto& k : keys) { req.push_back(k); req.push_back(v); }
        dispatch(srv.store(), srv, req);
    };
    resp::Request mget = {"MGET"};
    mget.insert(mget.end(), keys.begin(), keys.end());
    mset("a");

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) mset(i % 2 ? "a" : "b");
        done = true;
    });
    std::thread deleter([&] {
        // Multi-key DEL + MSET racing with the writer must also never tear.
        for (int i = 0; i < 2000; ++i) {
            resp::Request del = {"DEL"};
            del.insert(del.end(), keys.begin(), keys.end());
            dispatch(srv.store(), srv, del);
            mset("a");
        }
    });
    const std::string all_a = dispatch(srv.store(), srv, mget);
    mset("b");
    const std::string all_b = dispatch(srv.store(), srv, mget);
    mset("a");
    int torn = 0;
    while (!done) {
        std::string got = dispatch(srv.store(), srv, mget);
        if (got != all_a && got != all_b) {
            // An all-missing reply (between DEL and MSET) is also consistent.
            if (got.find("$1\r\n") != std::string::npos && got.find("$-1\r\n") != std::string::npos) ++torn;
            else if (got.find("$1\r\na") != std::string::npos && got.find("$1\r\nb") != std::string::npos) ++torn;
        }
    }
    writer.join();
    deleter.join();
    CHECK_EQ(torn, 0);
}

TEST(default_bind_is_loopback) {
    ServerConfig cfg;
    CHECK_EQ(cfg.bind_addr, std::string("127.0.0.1"));
}
