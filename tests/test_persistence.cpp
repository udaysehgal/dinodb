#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "dinodb/commands.hpp"
#include "dinodb/persistence.hpp"
#include "dinodb/server.hpp"
#include "test_framework.hpp"

using namespace dinodb;

namespace {
std::string tmp_path(const char* name) {
    return std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/dinodb_test_" + name;
}

std::string run(Server& srv, std::vector<std::string> req) { return dispatch(srv.store(), srv, req); }
}  // namespace

TEST(snapshot_roundtrip_all_types) {
    std::string path = tmp_path("roundtrip.rdb");
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    cfg.rdb_path = path;
    {
        Server srv(cfg);
        run(srv, {"SET", "s", std::string("bin\0ary", 7)});
        run(srv, {"SET", "ttl", "v", "EX", "1000"});
        run(srv, {"HSET", "h", "f", "v"});
        run(srv, {"RPUSH", "l", "a", "b"});
        run(srv, {"SADD", "set", "m"});
        run(srv, {"ZADD", "z", "1.5", "m", "1.5", "a"});
        CHECK(srv.save_snapshot());
    }
    Server srv(cfg);
    CHECK(persistence::load(srv.store(), path) == persistence::LoadResult::Ok);
    CHECK_EQ(run(srv, {"GET", "s"}), resp::bulk_string(std::string("bin\0ary", 7)));
    CHECK(run(srv, {"TTL", "ttl"}) != std::string(":-1\r\n"));
    CHECK_EQ(run(srv, {"HGET", "h", "f"}), std::string("$1\r\nv\r\n"));
    CHECK_EQ(run(srv, {"LRANGE", "l", "0", "-1"}), std::string("*2\r\n$1\r\na\r\n$1\r\nb\r\n"));
    CHECK_EQ(run(srv, {"SISMEMBER", "set", "m"}), std::string(":1\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}),
             std::string("*4\r\n$1\r\na\r\n$3\r\n1.5\r\n$1\r\nm\r\n$3\r\n1.5\r\n"));
    std::remove(path.c_str());
}

TEST(missing_snapshot_is_reported) {
    Store s;
    CHECK(persistence::load(s, tmp_path("does_not_exist.rdb")) == persistence::LoadResult::Missing);
}

TEST(truncated_snapshot_loads_nothing) {
    std::string path = tmp_path("truncated.rdb");
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    cfg.rdb_path = path;
    {
        Server srv(cfg);
        for (int i = 0; i < 50; ++i) run(srv, {"SET", "k" + std::to_string(i), "value"});
        CHECK(srv.save_snapshot());
    }
    // Chop the file mid-record.
    std::string data;
    {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), {});
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size() - 3));
    }
    Store s;
    CHECK(persistence::load(s, path) == persistence::LoadResult::Corrupt);
    CHECK_EQ(s.size(), size_t{0});
    std::remove(path.c_str());
}

TEST(garbage_length_field_is_rejected) {
    std::string path = tmp_path("garbage.rdb");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("DINO\x01", 5);
        out.put(0);  // String type
        out.write("\xff\xff\xff\xff", 4);  // key length 4GB
    }
    Store s;
    CHECK(persistence::load(s, path) == persistence::LoadResult::Corrupt);
    std::remove(path.c_str());
}
