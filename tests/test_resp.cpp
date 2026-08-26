#include "dinodb/resp.hpp"
#include "test_framework.hpp"

using namespace dinodb;

TEST(parses_multibulk_request) {
    resp::Parser p;
    std::string raw = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
    p.feed(raw.data(), raw.size());
    auto req = p.next();
    CHECK(req.has_value());
    CHECK_EQ(req->size(), size_t{3});
    CHECK_EQ((*req)[0], std::string("SET"));
    CHECK_EQ((*req)[1], std::string("k"));
    CHECK_EQ((*req)[2], std::string("v"));
    CHECK(!p.next().has_value());
}

TEST(parses_inline_request) {
    resp::Parser p;
    std::string raw = "PING\r\n";
    p.feed(raw.data(), raw.size());
    auto req = p.next();
    CHECK(req.has_value());
    CHECK_EQ(req->size(), size_t{1});
    CHECK_EQ((*req)[0], std::string("PING"));
}

TEST(handles_partial_feed_across_calls) {
    resp::Parser p;
    std::string part1 = "*2\r\n$3\r\nGET\r\n$1";
    std::string part2 = "\r\nk\r\n";
    p.feed(part1.data(), part1.size());
    CHECK(!p.next().has_value());
    p.feed(part2.data(), part2.size());
    auto req = p.next();
    CHECK(req.has_value());
    CHECK_EQ(req->size(), size_t{2});
    CHECK_EQ((*req)[1], std::string("k"));
}

TEST(parses_two_requests_in_one_feed) {
    resp::Parser p;
    std::string raw = "PING\r\nPING\r\n";
    p.feed(raw.data(), raw.size());
    CHECK(p.next().has_value());
    CHECK(p.next().has_value());
    CHECK(!p.next().has_value());
}

TEST(bulk_string_encoding) {
    CHECK_EQ(resp::bulk_string("hi"), std::string("$2\r\nhi\r\n"));
    CHECK_EQ(resp::nil_bulk(), std::string("$-1\r\n"));
    CHECK_EQ(resp::integer(42), std::string(":42\r\n"));
}
