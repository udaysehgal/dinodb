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

namespace {
ServerConfig quiet_config() {
    ServerConfig cfg;
    cfg.autosave_interval_sec = 0;
    return cfg;
}
}  // namespace

TEST(set_nx_xx_and_expiry_validation) {
    Server srv(quiet_config());
    CHECK_EQ(run(srv, {"SET", "k", "a", "XX"}), std::string("$-1\r\n"));
    CHECK_EQ(run(srv, {"SET", "k", "a", "NX"}), std::string("+OK\r\n"));
    CHECK_EQ(run(srv, {"SET", "k", "b", "NX"}), std::string("$-1\r\n"));
    CHECK_EQ(run(srv, {"SET", "k", "c", "XX"}), std::string("+OK\r\n"));
    CHECK_EQ(run(srv, {"GET", "k"}), std::string("$1\r\nc\r\n"));
    CHECK(run(srv, {"SET", "k", "v", "EX", "0"}).rfind("-ERR invalid expire time", 0) == 0);
    CHECK(run(srv, {"SET", "k", "v", "EX", "-1"}).rfind("-ERR invalid expire time", 0) == 0);
    CHECK(run(srv, {"SET", "k", "v", "EX", "9223372036854775807"}).rfind("-ERR invalid expire time", 0) == 0);
    CHECK_EQ(run(srv, {"SET", "k", "v", "NX", "XX"}), std::string("-ERR syntax error\r\n"));
}

TEST(set_clears_previous_ttl) {
    Server srv(quiet_config());
    run(srv, {"SET", "k", "v", "EX", "100"});
    run(srv, {"SET", "k", "v2"});
    CHECK_EQ(run(srv, {"TTL", "k"}), std::string(":-1\r\n"));
}

TEST(getset_rejects_non_string) {
    Server srv(quiet_config());
    run(srv, {"HSET", "h", "f", "v"});
    CHECK(run(srv, {"GETSET", "h", "x"}).rfind("-WRONGTYPE", 0) == 0);
    CHECK_EQ(run(srv, {"TYPE", "h"}), std::string("+hash\r\n"));
    CHECK_EQ(run(srv, {"GETSET", "s", "1"}), std::string("$-1\r\n"));
    CHECK_EQ(run(srv, {"GETSET", "s", "2"}), std::string("$1\r\n1\r\n"));
}

TEST(incr_rejects_overflow_and_garbage) {
    Server srv(quiet_config());
    run(srv, {"SET", "n", "9223372036854775807"});
    CHECK(run(srv, {"INCR", "n"}).rfind("-ERR increment or decrement would overflow", 0) == 0);
    CHECK_EQ(run(srv, {"GET", "n"}), std::string("$19\r\n9223372036854775807\r\n"));
    run(srv, {"SET", "s", "12abc"});
    CHECK(run(srv, {"INCR", "s"}).rfind("-ERR value is not an integer", 0) == 0);
    run(srv, {"SET", "e", ""});
    CHECK(run(srv, {"INCR", "e"}).rfind("-ERR value is not an integer", 0) == 0);
    CHECK(run(srv, {"INCRBY", "x", "99999999999999999999"}).rfind("-ERR value is not an integer", 0) == 0);
    CHECK(run(srv, {"DECRBY", "x", "-9223372036854775808"}).rfind("-ERR", 0) == 0);
    CHECK_EQ(run(srv, {"EXISTS", "x"}), std::string(":0\r\n"));
}

TEST(pop_on_wrong_type_errors) {
    Server srv(quiet_config());
    run(srv, {"SET", "k", "v"});
    CHECK(run(srv, {"LPOP", "k"}).rfind("-WRONGTYPE", 0) == 0);
}

TEST(empty_containers_are_deleted) {
    Server srv(quiet_config());
    run(srv, {"RPUSH", "l", "a"});
    run(srv, {"LPOP", "l"});
    CHECK_EQ(run(srv, {"EXISTS", "l"}), std::string(":0\r\n"));
    run(srv, {"SADD", "s", "a"});
    run(srv, {"SREM", "s", "a"});
    CHECK_EQ(run(srv, {"TYPE", "s"}), std::string("+none\r\n"));
    run(srv, {"HSET", "h", "f", "v"});
    run(srv, {"HDEL", "h", "f"});
    CHECK_EQ(run(srv, {"EXISTS", "h"}), std::string(":0\r\n"));
    run(srv, {"ZADD", "z", "1", "m"});
    run(srv, {"ZREM", "z", "m"});
    CHECK_EQ(run(srv, {"EXISTS", "z"}), std::string(":0\r\n"));
    // A list key emptied and re-pushed as another type works.
    CHECK_EQ(run(srv, {"SET", "l", "now-a-string"}), std::string("+OK\r\n"));
}

TEST(zadd_validates_before_writing) {
    Server srv(quiet_config());
    CHECK(run(srv, {"ZADD", "z", "1", "a", "nope", "b"}).rfind("-ERR value is not a valid float", 0) == 0);
    CHECK_EQ(run(srv, {"EXISTS", "z"}), std::string(":0\r\n"));
    CHECK(run(srv, {"ZADD", "z", "nan", "a"}).rfind("-ERR", 0) == 0);
}

TEST(zset_scores_format_like_redis) {
    Server srv(quiet_config());
    run(srv, {"ZADD", "z", "1", "a", "2.5", "b", "-inf", "c"});
    CHECK_EQ(run(srv, {"ZSCORE", "z", "a"}), std::string("$1\r\n1\r\n"));
    CHECK_EQ(run(srv, {"ZSCORE", "z", "b"}), std::string("$3\r\n2.5\r\n"));
    CHECK_EQ(run(srv, {"ZSCORE", "z", "c"}), std::string("$4\r\n-inf\r\n"));
    CHECK_EQ(run(srv, {"ZINCRBY", "z", "0.5", "b"}), std::string("$1\r\n3\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}),
             std::string("*6\r\n$1\r\nc\r\n$4\r\n-inf\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$1\r\n3\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "0", "-1", "BOGUS"}), std::string("-ERR syntax error\r\n"));
}

TEST(zrange_ties_and_ranges) {
    Server srv(quiet_config());
    run(srv, {"ZADD", "z", "1", "c", "1", "a", "1", "b"});
    CHECK_EQ(run(srv, {"ZRANGE", "z", "0", "-1"}), std::string("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "-2", "-1"}), std::string("*2\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "5", "10"}), std::string("*0\r\n"));
    CHECK_EQ(run(srv, {"ZRANGE", "z", "-100", "0"}), std::string("*1\r\n$1\r\na\r\n"));
}

TEST(lrange_edge_cases) {
    Server srv(quiet_config());
    run(srv, {"RPUSH", "l", "a", "b", "c"});
    CHECK_EQ(run(srv, {"LRANGE", "l", "-100", "100"}), std::string("*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n"));
    CHECK_EQ(run(srv, {"LRANGE", "l", "2", "1"}), std::string("*0\r\n"));
    CHECK_EQ(run(srv, {"LRANGE", "l", "3", "5"}), std::string("*0\r\n"));
    CHECK_EQ(run(srv, {"LINDEX", "l", "-1"}), std::string("$1\r\nc\r\n"));
    CHECK_EQ(run(srv, {"LINDEX", "l", "3"}), std::string("$-1\r\n"));
}

TEST(expire_non_positive_deletes) {
    Server srv(quiet_config());
    run(srv, {"SET", "k", "v"});
    CHECK_EQ(run(srv, {"EXPIRE", "k", "-1"}), std::string(":1\r\n"));
    CHECK_EQ(run(srv, {"EXISTS", "k"}), std::string(":0\r\n"));
    CHECK_EQ(run(srv, {"EXPIRE", "k", "10"}), std::string(":0\r\n"));
}

TEST(keys_glob_patterns) {
    Server srv(quiet_config());
    run(srv, {"MSET", "hello", "1", "hallo", "1", "hxllo", "1", "hllo", "1", "h*llo", "1"});
    auto count = [&](const std::string& pat) {
        std::string reply = run(srv, {"KEYS", pat});
        return std::stoi(reply.substr(1));
    };
    CHECK_EQ(count("h?llo"), 4);
    CHECK_EQ(count("h*llo"), 5);
    CHECK_EQ(count("h[ae]llo"), 2);
    CHECK_EQ(count("h[^e]llo"), 3);
    CHECK_EQ(count("h[a-b]llo"), 1);
    CHECK_EQ(count("h\\*llo"), 1);
    CHECK_EQ(count("*"), 5);
}

TEST(arity_errors_match_redis) {
    Server srv(quiet_config());
    CHECK_EQ(run(srv, {"GET"}), std::string("-ERR wrong number of arguments for 'get' command\r\n"));
    CHECK_EQ(run(srv, {"get", "a", "b"}), std::string("-ERR wrong number of arguments for 'get' command\r\n"));
    CHECK_EQ(run(srv, {"MSET", "a"}), std::string("-ERR wrong number of arguments for 'mset' command\r\n"));
    CHECK_EQ(run(srv, {"HSET", "h", "f"}), std::string("-ERR wrong number of arguments for 'hset' command\r\n"));
    CHECK_EQ(run(srv, {"ping"}), std::string("+PONG\r\n"));
    CHECK_EQ(run(srv, {"SELECT", "0"}), std::string("+OK\r\n"));
    CHECK(run(srv, {"SELECT", "1"}).rfind("-ERR", 0) == 0);
}
