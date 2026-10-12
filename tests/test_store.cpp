#include "dinodb/store.hpp"
#include "test_framework.hpp"

using namespace dinodb;

namespace {
void put_string(Store& s, const std::string& key, const std::string& val) {
    s.write(key, [&](Slot& slot) { slot.reset(ValueType::String).value.str = val; });
}

std::string zorder_members(const Value& v) {
    std::string order;
    for (auto& [score, member] : v.zorder()) order += member;
    return order;
}
}  // namespace

TEST(get_missing_key_returns_null) {
    Store s;
    CHECK(s.read("nope", [](const Entry* e) { return e == nullptr; }));
}

TEST(set_and_get_roundtrip) {
    Store s;
    put_string(s, "k", "hello");
    CHECK_EQ(s.read("k", [](const Entry* e) { return e ? e->value.str : std::string("<null>"); }),
             std::string("hello"));
}

TEST(del_removes_key) {
    Store s;
    put_string(s, "k", "v");
    CHECK(s.del("k"));
    CHECK(!s.exists("k"));
    CHECK(!s.del("k"));
}

TEST(expire_makes_key_disappear) {
    Store s;
    put_string(s, "k", "v");
    CHECK(s.pexpire("k", 0));
    CHECK(!s.exists("k"));
    CHECK(!s.pexpire("missing", 100));
}

TEST(ttl_reports_no_expiry) {
    Store s;
    put_string(s, "k", "v");
    CHECK_EQ(s.ttl_seconds("k"), int64_t{-1});
    CHECK_EQ(s.ttl_seconds("missing"), int64_t{-2});
}

TEST(huge_expire_does_not_overflow) {
    Store s;
    put_string(s, "k", "v");
    CHECK(s.expire("k", INT64_MAX));
    CHECK(s.exists("k"));
    CHECK(s.ttl_seconds("k") > 0);
}

TEST(emptied_container_is_deleted) {
    Store s;
    s.write("l", [](Slot& slot) { slot.get_or_create(ValueType::List).value.list().push_back("x"); });
    CHECK(s.exists("l"));
    s.write("l", [](Slot& slot) { slot.get()->value.list().pop_front(); });
    CHECK(!s.exists("l"));
}

TEST(sweep_removes_expired_keys) {
    Store s(4);
    for (int i = 0; i < 100; ++i) {
        put_string(s, "k" + std::to_string(i), "v");
        s.write("k" + std::to_string(i), [](Slot& slot) { slot.get()->expires_at = now(); });
    }
    put_string(s, "live", "v");
    size_t removed = 0;
    for (size_t i = 0; i < s.shard_count(); ++i) removed += s.sweep_expired_step();
    CHECK_EQ(removed, size_t{100});
    CHECK_EQ(s.size(), size_t{1});
}

TEST(zset_orders_by_score) {
    Value v;
    v.zset_put("b", 2.0);
    v.zset_put("a", 1.0);
    v.zset_put("c", 3.0);
    CHECK_EQ(zorder_members(v), std::string("abc"));
}

TEST(zset_put_updates_existing_member_order) {
    Value v;
    v.zset_put("a", 1.0);
    v.zset_put("b", 2.0);
    v.zset_put("a", 5.0);
    CHECK_EQ(zorder_members(v), std::string("ba"));
    CHECK_EQ(v.zorder().size(), size_t{2});
}

TEST(zset_ties_break_lexicographically) {
    Value v;
    v.zset_put("c", 1.0);
    v.zset_put("a", 1.0);
    v.zset_put("b", 1.0);
    CHECK_EQ(zorder_members(v), std::string("abc"));
    CHECK(v.zset_remove("b"));
    CHECK_EQ(zorder_members(v), std::string("ac"));
}
