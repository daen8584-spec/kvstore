#pragma once

// Minimal test harness: TEST registers a function, CHECK records a failure and continues,
// REQUIRE stops the current test.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

struct Abort {};

inline void report(const char* file, int line, const std::string& message) {
    ++failures();
    std::cerr << file << ":" << line << ": " << message << "\n";
}

}  // namespace test

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)
#define TEST(name)                                                                                       \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                                       \
    static const test::Register TEST_CONCAT(test_reg_, __LINE__)(name, TEST_CONCAT(test_fn_, __LINE__)); \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) test::report(__FILE__, __LINE__, "CHECK failed: " #expr); \
    } while (0)

#define REQUIRE(expr)                                                   \
    do {                                                                \
        if (!(expr)) {                                                  \
            test::report(__FILE__, __LINE__, "REQUIRE failed: " #expr); \
            throw test::Abort{};                                        \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        const auto& check_a_ = (a);                                                                   \
        const auto& check_b_ = (b);                                                                   \
        if (!(check_a_ == check_b_)) {                                                                \
            std::ostringstream check_message_;                                                        \
            check_message_ << "CHECK_EQ failed: " #a " == " #b " (" << check_a_ << " vs " << check_b_ \
                           << ")";                                                                    \
            test::report(__FILE__, __LINE__, check_message_.str());                                   \
        }                                                                                             \
    } while (0)
