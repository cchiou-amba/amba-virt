/*
 * IavQualConfig.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVQUALCONFIG_HXX
#define IAVQUAL_IAVQUALCONFIG_HXX

#include <string>

enum IavQualMode
{
    IavQualModeTests,
    IavQualModeBench,
    IavQualModeWindows,
    IavQualModeSurvive
};

/* Exit codes of the qualification runner. */
enum IavQualExit
{
    IavQualExitPass = 0,
    IavQualExitTestFailure = 1,
    IavQualExitUsage = 2,
    IavQualExitOpenFailure = 3,
    IavQualExitEnvironment = 4
};

class IavQualConfig
{
public:
    IavQualConfig();

    /*
     * Parse the arguments that follow the entry flag. Returns false and fills error on a bad
     * or unknown option.
     */
    bool parse(int argc, char **argv, int first, std::string &error);

    IavQualMode _mode;
    std::string _device;
    std::string _results;
    std::string _board;
    std::string _backend;
    std::string _runId;
    /* run only this group (a debugging aid; the gates run every group) */
    std::string _group;
    /*
     * The role of the client being tested through the virtual /dev/iav: "primary" (the guest that
     * may configure the pipeline), "consumer" (every other guest) or empty (not stated: the
     * role-dependent tests record what they see and judge nothing).
     */
    std::string _role;
    /* survive mode: how long to keep calling, in seconds */
    unsigned int _seconds;
    /* negative control for the window tests: shift the DSP window by this many bytes */
    unsigned long long _windowSkew;
};

#endif /* IAVQUAL_IAVQUALCONFIG_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
