#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int failures = 0;

#define TEST(name) \
    void test_##name(); \
    static testing::Registrar registrar_##name(#name, test_##name); \
    void test_##name()

#define CHECK_EQ(a, b) \
    do { \
        auto va = (a); \
        auto vb = (b); \
        if (!(va == vb)) { \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ \
                      << " expected equal, got [" << va << "] vs [" << vb << "]\n"; \
            ++testing::failures; \
        } \
    } while (0)

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << " " #cond " was false\n"; \
            ++testing::failures; \
        } \
    } while (0)

}  // namespace testing
