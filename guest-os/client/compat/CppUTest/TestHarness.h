/*
 * TestHarness.h - Lightweight fallback CppUTest test harness for environments
 * without external CppUTest package (e.g. QNX Neutrino RTOS).
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef CPPUTEST_TEST_HARNESS_H
#define CPPUTEST_TEST_HARNESS_H

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace CppUTestMini {

struct TestFailureException {
};

struct TestCase {
    std::string group;
    std::string name;
    std::function<void()> run;
};

inline std::vector<TestCase> &getRegistry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int &getFailureCount() {
    static int failures = 0;
    return failures;
}

inline int &getCheckCount() {
    static int checks = 0;
    return checks;
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
                bool failed = false; \
                try { \
                    t.setup(); \
                    t.testBody(); \
                } catch (const CppUTestMini::TestFailureException &) { \
                    failed = true; \
                } catch (const std::exception &e) { \
                    std::cerr << __FILE__ << ":" << __LINE__ \
                              << ": Failure: unhandled exception: " << e.what() << "\n"; \
                    failed = true; \
                } catch (...) { \
                    std::cerr << __FILE__ << ":" << __LINE__ \
                              << ": Failure: unhandled exception\n"; \
                    failed = true; \
                } \
                try { \
                    t.teardown(); \
                } catch (...) { \
                    failed = true; \
                } \
                if (failed) { \
                    CppUTestMini::getFailureCount()++; \
                } \
            }}); \
        } \
    } s_reg_##groupName##_##testName; \
    void TEST_##groupName##_##testName::testBody()

#define CHECK(cond) do { \
    CppUTestMini::getCheckCount()++; \
    if (!(cond)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: CHECK(" #cond ")\n"; \
        throw CppUTestMini::TestFailureException(); \
    } \
} while (0)

#define CHECK_TRUE(cond) CHECK(cond)

#define CHECK_TRUE_TEXT(cond, text) do { \
    CppUTestMini::getCheckCount()++; \
    if (!(cond)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << (text) << " - CHECK_TRUE(" #cond ")\n"; \
        throw CppUTestMini::TestFailureException(); \
    } \
} while (0)

#define CHECK_EQUAL(expected, actual) do { \
    CppUTestMini::getCheckCount()++; \
    auto _exp = (expected); \
    auto _act = (actual); \
    if (_exp != _act) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: expected " << _exp << " but got " << _act << "\n"; \
        throw CppUTestMini::TestFailureException(); \
    } \
} while (0)

#define LONGS_EQUAL(expected, actual) CHECK_EQUAL((long)(expected), (long)(actual))

#define LONGS_EQUAL_TEXT(expected, actual, text) do { \
    CppUTestMini::getCheckCount()++; \
    long _exp = (long)(expected); \
    long _act = (long)(actual); \
    if (_exp != _act) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << (text) << " - expected " << _exp << " but got " << _act << "\n"; \
        throw CppUTestMini::TestFailureException(); \
    } \
} while (0)

#define STRCMP_EQUAL(expected, actual) do { \
    CppUTestMini::getCheckCount()++; \
    const char *_exp = (expected); \
    const char *_act = (actual); \
    std::string _expStr = _exp ? _exp : "(null)"; \
    std::string _actStr = _act ? _act : "(null)"; \
    if (!_exp || !_act || _expStr != _actStr) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: expected \"" << _expStr << "\" but got \"" << _actStr << "\"\n"; \
        throw CppUTestMini::TestFailureException(); \
    } \
} while (0)

#define FAIL(text) do { \
    CppUTestMini::getCheckCount()++; \
    std::cerr << __FILE__ << ":" << __LINE__ << ": Failure: " << (text) << "\n"; \
    throw CppUTestMini::TestFailureException(); \
} while (0)

#endif // CPPUTEST_TEST_HARNESS_H

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
