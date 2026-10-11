/*
 * IavQualContext.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavQualContext.hxx"

#include <stdio.h>
#include <string.h>
#include <time.h>

IavQualContext::IavQualContext() :
    _backend(NULL),
    _config(NULL),
    _results(NULL),
    _obsLen(0),
    _rc(0),
    _errno(0)
{
    _obs[0] = '\0';
    _obsOut[0] = '\0';
}

IavQualContext &IavQualContext::instance()
{
    static IavQualContext ctx;

    return ctx;
}

void IavQualContext::configure(IavBackend *backend, const IavQualConfig *config,
                               IavQualResults *results)
{
    _backend = backend;
    _config = config;
    _results = results;
    resetObservations();
}

void IavQualContext::note(const char *key, long long value)
{
    int n;

    if (_obsLen + 80 >= sizeof(_obs)) {
        return;
    }
    n = snprintf(_obs + _obsLen, sizeof(_obs) - _obsLen, "%s\"%s\":%lld", _obsLen ? "," : "", key,
                 value);
    if (n > 0) {
        _obsLen += (size_t)n;
    }
}

void IavQualContext::noteText(const char *key, const char *value)
{
    size_t pos = _obsLen;

    if (_obsLen + 200 >= sizeof(_obs)) {
        return;
    }
    pos += (size_t)snprintf(_obs + pos, sizeof(_obs) - pos, "%s\"%s\":\"", pos ? "," : "", key);
    pos = IavQualResults::escapeInto(_obs, sizeof(_obs) - 2, pos, value);
    _obs[pos++] = '"';
    _obs[pos] = '\0';
    _obsLen = pos;
}

const char *IavQualContext::observations()
{
    snprintf(_obsOut, sizeof(_obsOut), "{%s}", _obs);
    return _obsOut;
}

void IavQualContext::resetObservations()
{
    _obs[0] = '\0';
    _obsLen = 0;
    _rc = 0;
    _errno = 0;
}

void IavQualContext::setResult(int rc, int err)
{
    _rc = rc;
    _errno = err;
}

long long IavQualContext::nowNs()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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
