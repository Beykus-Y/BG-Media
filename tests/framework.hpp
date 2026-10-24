#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bgmedia::test {

struct TestCase {
    std::string name;
    std::function<void()> function;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, void (*function)()) {
        registry().push_back({name, function});
    }
};

struct Failure : std::runtime_error {
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

inline void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::ostringstream out;
        out << file << ':' << line << ": CHECK failed: " << expression;
        throw Failure(out.str());
    }
}

template <typename A, typename B>
void checkNear(A actual, B expected, double tolerance, const char* expression,
               const char* file, int line) {
    const double a = static_cast<double>(actual);
    const double b = static_cast<double>(expected);
    if (!(std::abs(a - b) <= tolerance)) {
        std::ostringstream out;
        out << file << ':' << line << ": CHECK_NEAR failed: " << expression
            << " (actual " << a << ", expected " << b << ", tolerance " << tolerance
            << ')';
        throw Failure(out.str());
    }
}

} // namespace bgmedia::test

#define BGMEDIA_TEST_CONCAT_INNER(a, b) a##b
#define BGMEDIA_TEST_CONCAT(a, b) BGMEDIA_TEST_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                          \
    static void BGMEDIA_TEST_CONCAT(bgmedia_test_fn_, __LINE__)();                \
    static ::bgmedia::test::Registrar                                            \
        BGMEDIA_TEST_CONCAT(bgmedia_test_reg_, __LINE__)(                         \
            #name, &BGMEDIA_TEST_CONCAT(bgmedia_test_fn_, __LINE__));             \
    static void BGMEDIA_TEST_CONCAT(bgmedia_test_fn_, __LINE__)()

#define CHECK(expression)                                                        \
    ::bgmedia::test::check(static_cast<bool>(expression), #expression, __FILE__, \
                           __LINE__)

#define CHECK_NEAR(actual, expected, tolerance)                                  \
    ::bgmedia::test::checkNear((actual), (expected), (tolerance),                \
                               #actual " ~= " #expected, __FILE__, __LINE__)

#define REQUIRE(expression) CHECK(expression)
