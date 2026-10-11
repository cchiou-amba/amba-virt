/*
 * IavQualMain.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavQualMain.hxx"

#include "IavQualContext.hxx"
#include "IavQualOutput.hxx"
#include "IavQualResults.hxx"
#include "RealBackend.hxx"
#ifndef IAVQUAL_NO_LOOPBACK
#include "LoopbackBackend.hxx"
#endif
#ifdef IAVQUAL_WITH_MOCK
#include "MockBackend.hxx"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <memory>
#include <algorithm>
#include <string>
#include <vector>

#include <CppUTest/CommandLineTestRunner.h>
#include <CppUTest/TestRegistry.h>

namespace {

/* A runner that reports through IavQualOutput. */
class IavQualRunner : public CommandLineTestRunner
{
public:
    IavQualRunner(int ac, const char *const *av, TestRegistry *registry, IavQualResults *results,
                  IavQualMode mode) :
        CommandLineTestRunner(ac, av, registry),
        _results(results),
        _mode(mode)
    {
    }

protected:
    virtual TestOutput *createConsoleOutput()
    {
        return new IavQualOutput(_results, _mode);
    }

private:
    IavQualResults *_results;
    IavQualMode _mode;
};

/* mkdir -p for the directory part of path. */
void makeParentDirs(const std::string &path)
{
    std::string dir;

    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            dir = path.substr(0, i);
            mkdir(dir.c_str(), 0755);
        }
    }
}

#ifndef IAVQUAL_NO_LOOPBACK
/* Report why a loopback backend could not be set up: usually that the device does not open. */
IavBackend *deviceOpenFailure(IavBackend *probe, const IavQualConfig &config, int *exitCode)
{
    int fd = probe->open(config._device.c_str(), O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "iav-qual: cannot open %s: %s\n", config._device.c_str(), strerror(errno));
        *exitCode = IavQualExitOpenFailure;
    } else {
        probe->close(fd);
        fprintf(stderr, "iav-qual: the broker could not be set up on %s\n", config._device.c_str());
        *exitCode = IavQualExitEnvironment;
    }
    delete probe;
    return NULL;
}
#endif

/*
 * The backend named by --backend, or NULL. For a loopback backend whose device cannot be opened
 * *exit is set to IavQualExitOpenFailure and the open error is printed, as for the real backend.
 */
IavBackend *createBackend(const IavQualConfig &config, int *exitCode)
{
    *exitCode = IavQualExitUsage;
    if (config._backend == "real") {
        *exitCode = IavQualExitPass;
        return new RealBackend();
    }
#ifndef IAVQUAL_NO_LOOPBACK
    if (config._backend == "loopback") {
        /* The broker in this process, in front of the real driver. */
        LoopbackBackend *lb = new LoopbackBackend(new RealBackend(), config._device.c_str(), 1);

        if (!lb->ok()) {
            delete lb;
            return deviceOpenFailure(new RealBackend(), config, exitCode);
        }
        *exitCode = IavQualExitPass;
        return lb;
    }
#endif
#ifdef IAVQUAL_WITH_MOCK
    if (config._backend == "loopback-mock") {
        /* The broker in this process, in front of the mock driver, for the build host. */
        LoopbackBackend *lb = new LoopbackBackend(new MockBackend(MockBackend::VariantNone),
                                                  config._device.c_str(), 1);

        if (!lb->ok()) {
            delete lb;
            return deviceOpenFailure(new MockBackend(MockBackend::VariantNone), config, exitCode);
        }
        *exitCode = IavQualExitPass;
        return lb;
    }
    {
        MockBackend::Variant variant;

        if (MockBackend::variantFromName(config._backend.c_str(), &variant)) {
            *exitCode = IavQualExitPass;
            return new MockBackend(variant);
        }
    }
#endif
    return NULL;
}

int openCheck(IavBackend &backend, const IavQualConfig &config)
{
    int fd = backend.open(config._device.c_str(), O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "iav-qual: cannot open %s: %s\n", config._device.c_str(),
                strerror(errno));
        return IavQualExitOpenFailure;
    }
    backend.close(fd);
    return IavQualExitPass;
}

} // namespace

int iavQualPrecheck(int argc, char **argv, int first)
{
    IavQualConfig config;
    std::string error;
    std::unique_ptr<IavBackend> backend;

    if (!config.parse(argc, argv, first, error)) {
        fprintf(stderr, "iav-qual: %s\n", error.c_str());
        return IavQualExitUsage;
    }
    {
        int why = IavQualExitUsage;

        backend.reset(createBackend(config, &why));
        if (!backend) {
            if (why == IavQualExitUsage) {
                fprintf(stderr, "iav-qual: unknown backend %s\n", config._backend.c_str());
            }
            return why;
        }
    }
    return openCheck(*backend, config);
}

int iavQualMain(int argc, char **argv, int first, IavBackend *injected)
{
    IavQualConfig config;
    IavQualResults results;
    std::string error;
    std::unique_ptr<IavBackend> owned;
    IavBackend *backend = injected;
    int rc;

    if (!config.parse(argc, argv, first, error)) {
        fprintf(stderr, "iav-qual: %s\n", error.c_str());
        return IavQualExitUsage;
    }
    if (config._results.empty()) {
        fprintf(stderr, "iav-qual: --results is required\n");
        return IavQualExitUsage;
    }
    if (backend == NULL) {
        int why = IavQualExitUsage;

        owned.reset(createBackend(config, &why));
        backend = owned.get();
        if (backend == NULL) {
            if (why == IavQualExitUsage) {
                fprintf(stderr, "iav-qual: unknown backend %s\n", config._backend.c_str());
            }
            return why;
        }
    }

    rc = openCheck(*backend, config);
    if (rc != IavQualExitPass) {
        return rc;
    }

    makeParentDirs(config._results);
    if (!results.open(config._results.c_str(), config._runId.c_str(), config._board.c_str(),
                      config._backend.c_str())) {
        fprintf(stderr, "iav-qual: cannot write %s: %s\n", config._results.c_str(),
                strerror(errno));
        return IavQualExitEnvironment;
    }
    IavQualContext::instance().configure(backend, &config, &results);

    {
        std::vector<std::string> groups;
        std::vector<const char *> av;
        const char *modeName = config._mode == IavQualModeBench ? "bench" :
                               config._mode == IavQualModeWindows ? "windows" :
                               config._mode == IavQualModeSurvive ? "survive" : "tests";

        /*
         * CppUTest ORs repeated group filters, so "everything except two groups" cannot be
         * written with -xsg. Select the groups to run by name instead, taken from the registry
         * so that a new group runs in tests mode without editing this list.
         */
        for (UtestShell *t = TestRegistry::getCurrentRegistry()->getFirstTest(); t != NULL;
             t = t->getNext()) {
            std::string g = t->getGroup().asCharString();
            bool wanted = config._mode == IavQualModeBench ? g == "IavBench" :
                          config._mode == IavQualModeWindows ? g == "IavWindow" :
                          config._mode == IavQualModeSurvive ? g == "IavSurvive" :
                          (g != "IavBench" && g != "IavWindow" && g != "IavSurvive");

            if (!config._group.empty()) {
                wanted = wanted && g == config._group;
            }
            if (wanted && std::find(groups.begin(), groups.end(), g) == groups.end()) {
                groups.push_back(g);
            }
        }
        av.push_back("iav-qual");
        av.push_back("-v");
        for (size_t i = 0; i < groups.size(); i++) {
            av.push_back("-sg");
            av.push_back(groups[i].c_str());
        }
        int ac = (int)av.size();

        av.push_back(NULL);
        printf("iav-qual: run_id=%s board=%s backend=%s device=%s mode=%s\n",
               config._runId.c_str(), config._board.c_str(), config._backend.c_str(),
               config._device.c_str(), modeName);

        IavQualRunner runner(ac, &av[0], TestRegistry::getCurrentRegistry(), &results,
                             config._mode);
        rc = runner.runAllTestsMain();
    }

    printf("iav-qual: %lu result rows written to %s\n", results.count(),
           config._results.c_str());
    results.close();
    IavQualContext::instance().configure(NULL, NULL, NULL);
    return rc == 0 ? IavQualExitPass : IavQualExitTestFailure;
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
