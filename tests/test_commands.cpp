#include "dinodb/commands.hpp"
#include "dinodb/server.hpp"
#include "test_framework.hpp"

using namespace dinodb;

namespace {
std::string run(Server& srv, std::vector<std::string> req) {
    return dispatch(srv.store(), srv, req);
}
}  // namespace

TEST(set_and_get_via_dispatch) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    CHECK_EQ(run(srv, {"SET", "k", "v"}), std::string("+OK\r\n"));
    CHECK_EQ(run(srv, {"GET", "k"}), std::string("$1\r\nv\r\n"));
    CHECK_EQ(run(srv, {"GET", "missing"}), std::string("$-1\r\n"));
}

TEST(incr_creates_and_increments) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    CHECK_EQ(run(srv, {"INCR", "counter"}), std::string(":1\r\n"));
    CHECK_EQ(run(srv, {"INCR", "counter"}), std::string(":2\r\n"));
    CHECK_EQ(run(srv, {"INCRBY", "counter", "10"}), std::string(":12\r\n"));
}

TEST(wrong_type_error) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    run(srv, {"SET", "k", "v"});
    std::string reply = run(srv, {"LPUSH", "k", "x"});
    CHECK(reply.rfind("-WRONGTYPE", 0) == 0);
}

TEST(list_push_pop_range) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    run(srv, {"RPUSH", "l", "a"});
    run(srv, {"RPUSH", "l", "b"});
    run(srv, {"RPUSH", "l", "c"});
    CHECK_EQ(run(srv, {"LLEN", "l"}), std::string(":3\r\n"));
    CHECK_EQ(run(srv, {"LRANGE", "l", "0", "-1"}),
             std::string("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(run(srv, {"LPOP", "l"}), std::string("$1\r\na\r\n"));
}

TEST(expire_and_ttl_roundtrip) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    run(srv, {"SET", "k", "v"});
    CHECK_EQ(run(srv, {"EXPIRE", "k", "100"}), std::string(":1\r\n"));
    CHECK_EQ(run(srv, {"TTL", "k"}), std::string(":100\r\n"));
    CHECK_EQ(run(srv, {"PERSIST", "k"}), std::string(":1\r\n"));
    CHECK_EQ(run(srv, {"TTL", "k"}), std::string(":-1\r\n"));
}

TEST(unknown_command_errors) {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    Server srv(cfg);
    std::string reply = run(srv, {"NOTACOMMAND"});
    CHECK(reply.rfind("-ERR unknown command", 0) == 0);
}
