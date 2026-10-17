/*
 * CommandLineTestRunner.h - Lightweight fallback test runner for environments
 * without external CppUTest package (e.g. QNX Neutrino RTOS).
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef CPPUTEST_COMMAND_LINE_TEST_RUNNER_H
#define CPPUTEST_COMMAND_LINE_TEST_RUNNER_H

#include "TestHarness.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

class CommandLineTestRunner {
public:
    static void printUsage(const char *prog) {
        std::cerr << "Usage: " << (prog ? prog : "test-runner")
                  << " [-v] [-g <group>] [-sg <substr>] [-xg <substr>] [-lg] [-ln]\n";
    }

    static int RunAllTests(int argc, char **argv) {
        bool verbose = false;
        bool listGroups = false;
        bool listNames = false;
        std::vector<std::string> exactGroups;
        std::vector<std::string> substrGroups;
        std::vector<std::string> excludeGroups;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-v") {
                verbose = true;
            } else if (arg == "-lg") {
                listGroups = true;
            } else if (arg == "-ln") {
                listNames = true;
            } else if (arg == "-g") {
                if (i + 1 >= argc) {
                    printUsage(argv[0]);
                    return 2;
                }
                exactGroups.push_back(argv[++i]);
            } else if (arg == "-sg") {
                if (i + 1 >= argc) {
                    printUsage(argv[0]);
                    return 2;
                }
                substrGroups.push_back(argv[++i]);
            } else if (arg == "-xg") {
                if (i + 1 >= argc) {
                    printUsage(argv[0]);
                    return 2;
                }
                excludeGroups.push_back(argv[++i]);
            } else {
                printUsage(argv[0]);
                return 2;
            }
        }

        auto &tests = CppUTestMini::getRegistry();

        if (listGroups) {
            std::vector<std::string> seen;
            for (const auto &t : tests) {
                if (std::find(seen.begin(), seen.end(), t.group) == seen.end()) {
                    seen.push_back(t.group);
                    std::cout << t.group << "\n";
                }
            }
            return 0;
        }

        if (listNames) {
            for (const auto &t : tests) {
                std::cout << t.group << "." << t.name << "\n";
            }
            return 0;
        }

        CppUTestMini::getFailureCount() = 0;
        CppUTestMini::getCheckCount() = 0;

        size_t total = tests.size();
        size_t ran = 0;
        size_t filtered = 0;

        auto suiteStart = std::chrono::steady_clock::now();

        for (auto &t : tests) {
            bool included = exactGroups.empty() && substrGroups.empty();
            for (const auto &g : exactGroups) {
                if (t.group == g || t.group.find(g) != std::string::npos) {
                    included = true;
                    break;
                }
            }
            if (!included) {
                for (const auto &sg : substrGroups) {
                    if (t.group.find(sg) != std::string::npos) {
                        included = true;
                        break;
                    }
                }
            }
            if (included) {
                for (const auto &xg : excludeGroups) {
                    if (t.group.find(xg) != std::string::npos) {
                        included = false;
                        break;
                    }
                }
            }

            if (!included) {
                filtered++;
                continue;
            }

            ran++;
            int before = CppUTestMini::getFailureCount();
            auto testStart = std::chrono::steady_clock::now();
            t.run();
            auto testEnd = std::chrono::steady_clock::now();
            long long testMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                testEnd - testStart).count();

            if (verbose) {
                std::cout << "TEST(" << t.group << ", " << t.name << ") - " << testMs << " ms\n";
            }
            if (CppUTestMini::getFailureCount() == before) {
                if (!verbose) {
                    std::cout << ".";
                }
            } else {
                std::cout << "\n[FAILED] " << t.group << "." << t.name << "\n";
            }
        }

        auto suiteEnd = std::chrono::steady_clock::now();
        long long totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            suiteEnd - suiteStart).count();

        int failed = CppUTestMini::getFailureCount();
        int checks = CppUTestMini::getCheckCount();

        if (failed == 0) {
            std::cout << "\nOK (" << total << " tests, " << ran << " ran, "
                      << checks << " checks, 0 ignored, " << filtered
                      << " filtered out, " << totalMs << " ms)\n";
            return 0;
        } else {
            std::cout << "\nErrors (" << failed << " failures, " << total << " tests, "
                      << ran << " ran, " << checks << " checks, 0 ignored, "
                      << filtered << " filtered out, " << totalMs << " ms)\n";
            return 1;
        }
    }
};

#endif // CPPUTEST_COMMAND_LINE_TEST_RUNNER_H

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
