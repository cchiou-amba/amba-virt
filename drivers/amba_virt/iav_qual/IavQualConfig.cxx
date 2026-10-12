/*
 * IavQualConfig.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavQualConfig.hxx"

#include <cstdlib>
#include <cstring>
#include <ctime>

IavQualConfig::IavQualConfig() :
    _mode(IavQualModeTests),
    _device("/dev/iav"),
    _results(),
    _board("unknown"),
    _backend("real"),
    _runId(),
    _windowSkew(0),
    _seconds(60)
{
}

bool IavQualConfig::parse(int argc, char **argv, int first, std::string &error)
{
    for (int i = first; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "--mode") == 0 || strcmp(arg, "--results") == 0 ||
            strcmp(arg, "--board") == 0 || strcmp(arg, "--device") == 0 ||
            strcmp(arg, "--backend") == 0 || strcmp(arg, "--run-id") == 0 ||
            strcmp(arg, "--window-skew") == 0 || strcmp(arg, "--group") == 0 ||
            strcmp(arg, "--seconds") == 0 || strcmp(arg, "--role") == 0) {
            if (i + 1 >= argc) {
                error = std::string("missing value for ") + arg;
                return false;
            }
            value = argv[++i];
        } else {
            error = std::string("unknown option ") + arg;
            return false;
        }

        if (strcmp(arg, "--mode") == 0) {
            if (strcmp(value, "tests") == 0) {
                _mode = IavQualModeTests;
            } else if (strcmp(value, "bench") == 0) {
                _mode = IavQualModeBench;
            } else if (strcmp(value, "windows") == 0) {
                _mode = IavQualModeWindows;
            } else if (strcmp(value, "survive") == 0) {
                _mode = IavQualModeSurvive;
            } else {
                error = std::string("--mode must be tests, bench, windows or survive, not ") + value;
                return false;
            }
        } else if (strcmp(arg, "--results") == 0) {
            _results = value;
        } else if (strcmp(arg, "--board") == 0) {
            _board = value;
        } else if (strcmp(arg, "--device") == 0) {
            _device = value;
        } else if (strcmp(arg, "--backend") == 0) {
            _backend = value;
        } else if (strcmp(arg, "--group") == 0) {
            _group = value;
        } else if (strcmp(arg, "--role") == 0) {
            if (strcmp(value, "primary") != 0 && strcmp(value, "consumer") != 0) {
                error = "--role must be primary or consumer";
                return false;
            }
            _role = value;
        } else if (strcmp(arg, "--seconds") == 0) {
            _seconds = (unsigned int)strtoul(value, NULL, 10);
            if (_seconds == 0 || _seconds > 3600) {
                error = "--seconds must be 1 to 3600";
                return false;
            }
        } else if (strcmp(arg, "--window-skew") == 0) {
            char *end = NULL;

            _windowSkew = strtoull(value, &end, 0);
            if (end == value || *end != '\0' || (_windowSkew & 0xfffULL) != 0) {
                error = "--window-skew must be a page multiple";
                return false;
            }
        } else {
            _runId = value;
        }
    }

    if (_runId.empty()) {
        char buf[64];

        snprintf(buf, sizeof(buf), "%s-%ld", _board.c_str(), (long)time(NULL));
        _runId = buf;
    }
    return true;
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
