/*
 * CommandLineTestRunner.h - Lightweight fallback test runner for environments
 * without external CppUTest package (e.g. QNX Neutrino RTOS).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef CPPUTEST_COMMAND_LINE_TEST_RUNNER_H
#define CPPUTEST_COMMAND_LINE_TEST_RUNNER_H

#include "TestHarness.h"
#include <iostream>

class CommandLineTestRunner {
public:
    static int RunAllTests(int argc, char **argv) {
        (void)argc;
        (void)argv;
        auto &tests = CppUTestMini::getRegistry();
        size_t total = tests.size();
        size_t passed = 0;
        std::cout << "Running " << total << " tests...\n";
        for (auto &t : tests) {
            int before = CppUTestMini::getFailureCount();
            t.run();
            if (CppUTestMini::getFailureCount() == before) {
                passed++;
                std::cout << ".";
            } else {
                std::cout << "\n[FAILED] " << t.group << "::" << t.name << "\n";
            }
        }
        std::cout << "\nOK (" << passed << " tests, " << passed << " ran)\n";
        return CppUTestMini::getFailureCount() == 0 ? 0 : 1;
    }
};

#endif // CPPUTEST_COMMAND_LINE_TEST_RUNNER_H
