/*
 * IavQualMain.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVQUALMAIN_HXX
#define IAVQUAL_IAVQUALMAIN_HXX

#include "IavBackend.hxx"
#include "IavQualConfig.hxx"

/*
 * Run the qualification suite. The options follow argv[first]. If backend is NULL the backend
 * named by --backend is created (real, and the mock variants in host builds). Returns an
 * IavQualExit code.
 */
int iavQualMain(int argc, char **argv, int first, IavBackend *backend);

/*
 * Parse the options and open the device, without running anything. Used by the server entry to
 * refuse early, before it stops the production worker. Returns an IavQualExit code.
 */
int iavQualPrecheck(int argc, char **argv, int first);

#endif /* IAVQUAL_IAVQUALMAIN_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
