/*
 * TestHarness.h - Lightweight fallback CppUTest test harness for environments
 * without external CppUTest package (e.g. QNX Neutrino RTOS).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef CPPUTEST_TEST_HARNESS_H
#define CPPUTEST_TEST_HARNESS_H

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace CppUTestMini {

struct TestCase {
    std::string group;
    std::string name;
    std::function<void()> run;
};

inline std::vector<TestCase>& getRegistry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& getFailureCount() {
    static int failures = 0;
    return failures;
}

} // namespace CppUTestMini

struct Utest {
    virtual ~Utest() {}
    virtual void setup() {}
    virtual void teardown() {}
};

#define TEST_GROUP(groupName) \
    struct TEST_GROUP_##groupName : public Utest

#define TEST(groupName, testName) \
    struct TEST_##groupName##_##testName : public TEST_GROUP_##groupName { \
        void testBody(); \
    }; \
    static struct Register_##groupName##_##testName { \
        Register_##groupName##_##testName() { \
            CppUTestMini::getRegistry().push_back({ #groupName, #testName, []() { \
                TEST_##groupName##_##testName t; \
                t.setup(); \
                try { t.testBody(); } \
                catch (...) { CppUTestMini::getFailureCount()++; } \
                t.teardown(); \
            }}); \
        } \
    } s_reg_##groupName##_##testName; \
    void TEST_##groupName##_##testName::testBody()

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: CHECK(" #cond ")\n"; \
        CppUTestMini::getFailureCount()++; \
    } \
} while (0)

#define CHECK_TRUE(cond) CHECK(cond)

#define CHECK_TRUE_TEXT(cond, text) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << text << " - CHECK_TRUE(" #cond ")\n"; \
        CppUTestMini::getFailureCount()++; \
    } \
} while (0)

#define CHECK_EQUAL(expected, actual) do { \
    auto _exp = (expected); \
    auto _act = (actual); \
    if (_exp != _act) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: expected " << _exp << " but got " << _act << "\n"; \
        CppUTestMini::getFailureCount()++; \
    } \
} while (0)

#define LONGS_EQUAL(expected, actual) CHECK_EQUAL((long)(expected), (long)(actual))

#define LONGS_EQUAL_TEXT(expected, actual, text) do { \
    long _exp = (long)(expected); \
    long _act = (long)(actual); \
    if (_exp != _act) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << text << " - expected " << _exp << " but got " << _act << "\n"; \
        CppUTestMini::getFailureCount()++; \
    } \
} while (0)

#define STRCMP_EQUAL(expected, actual) do { \
    if (std::string(expected) != std::string(actual)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: expected \"" << (expected) << "\" but got \"" << (actual) << "\"\n"; \
        CppUTestMini::getFailureCount()++; \
    } \
} while (0)

#define FAIL(text) do { \
    std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << text << "\n"; \
    CppUTestMini::getFailureCount()++; \
} while (0)

#endif // CPPUTEST_TEST_HARNESS_H
