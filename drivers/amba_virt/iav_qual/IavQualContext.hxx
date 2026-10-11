/*
 * IavQualContext.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVQUALCONTEXT_HXX
#define IAVQUAL_IAVQUALCONTEXT_HXX

#include "IavBackend.hxx"
#include "IavQualConfig.hxx"
#include "IavQualResults.hxx"

#include <stdint.h>

/*
 * What the tests share: the backend, the configuration, the results file and the observations
 * of the test that is running. The observations live in a fixed buffer, with no heap use, so
 * the leak check at the end of each test sees nothing left over.
 */
class IavQualContext
{
public:
    static IavQualContext &instance();

    void configure(IavBackend *backend, const IavQualConfig *config, IavQualResults *results);

    IavBackend &backend() const { return *_backend; }
    const IavQualConfig &config() const { return *_config; }
    IavQualResults &results() const { return *_results; }

    /* Observations of the current test, written into the result row. */
    void note(const char *key, long long value);
    void noteText(const char *key, const char *value);
    /* The observations as a JSON object text; valid until the next reset. */
    const char *observations();
    void resetObservations();

    /* The return code and errno the current test reports in its result row. */
    void setResult(int rc, int err);
    int resultRc() const { return _rc; }
    int resultErrno() const { return _errno; }

    static long long nowNs();

private:
    IavQualContext();

    IavBackend *_backend;
    const IavQualConfig *_config;
    IavQualResults *_results;
    char _obs[3072];
    size_t _obsLen;
    char _obsOut[3200];
    int _rc;
    int _errno;
};

#endif /* IAVQUAL_IAVQUALCONTEXT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
