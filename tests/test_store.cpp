#include "dinodb/store.hpp"
#include "test_framework.hpp"

using namespace dinodb;

TEST(get_missing_key_returns_null) {
    Store s;
    CHECK(s.get_mut("nope") == nullptr);
}

TEST(set_and_get_roundtrip) {
    Store s;
    Entry& e = s.get_or_create("k", ValueType::String);
    e.value.str = "hello";
    Entry* got = s.get_mut("k");
    CHECK(got != nullptr);
    CHECK_EQ(got->value.str, std::string("hello"));
}

TEST(del_removes_key) {
    Store s;
    s.get_or_create("k", ValueType::String);
    CHECK(s.del("k"));
    CHECK(s.get_mut("k") == nullptr);
    CHECK(!s.del("k"));
}

TEST(expire_makes_key_disappear) {
    Store s;
    s.get_or_create("k", ValueType::String);
    CHECK(s.pexpire("k", 0));
    CHECK(s.get_mut("k") == nullptr);
}

TEST(ttl_reports_no_expiry) {
    Store s;
    s.get_or_create("k", ValueType::String);
    CHECK_EQ(s.ttl_seconds("k"), int64_t{-1});
    CHECK_EQ(s.ttl_seconds("missing"), int64_t{-2});
}

TEST(zset_orders_by_score) {
    Store s;
    Entry& e = s.get_or_create("z", ValueType::ZSet);
    e.value.zset_put("b", 2.0);
    e.value.zset_put("a", 1.0);
    e.value.zset_put("c", 3.0);
    std::string order;
    for (auto& [score, member] : e.value.zorder) order += member;
    CHECK_EQ(order, std::string("abc"));
}

TEST(zset_put_updates_existing_member_order) {
    Store s;
    Entry& e = s.get_or_create("z", ValueType::ZSet);
    e.value.zset_put("a", 1.0);
    e.value.zset_put("b", 2.0);
    e.value.zset_put("a", 5.0);
    std::string order;
    for (auto& [score, member] : e.value.zorder) order += member;
    CHECK_EQ(order, std::string("ba"));
    CHECK_EQ(e.value.zorder.size(), size_t{2});
}
