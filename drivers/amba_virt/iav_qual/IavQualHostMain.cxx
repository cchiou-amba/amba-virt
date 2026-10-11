/*
 * IavQualHostMain.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Entry point of the build-host suite, which runs against the mock driver.
 */

#include "IavQualMain.hxx"

int main(int argc, char **argv)
{
    return iavQualMain(argc, argv, 1, NULL);
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
