/*
 * drivers/amba_virt/tools/test/test_main.cxx
 *
 * Main entry point for drivers/amba_virt/tools CppUTest test suite.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/CommandLineTestRunner.h"

int main(int ac, char **av)
{
    return CommandLineTestRunner::RunAllTests(ac, av);
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
