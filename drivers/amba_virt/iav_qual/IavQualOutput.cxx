/*
 * IavQualOutput.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavQualOutput.hxx"

#include "IavQualContext.hxx"

#include <stdio.h>
#include <string.h>

#include <CppUTest/TestFailure.h>
#include <CppUTest/Utest.h>

IavQualOutput::IavQualOutput(IavQualResults *results, IavQualMode mode) :
    _results(results),
    _mode(mode),
    _failed(false),
    _startNs(0)
{
    _group[0] = _test[0] = _detail[0] = '\0';
}

void IavQualOutput::printCurrentTestStarted(const UtestShell &test)
{
    ConsoleTestOutput::printCurrentTestStarted(test);
    snprintf(_group, sizeof(_group), "%s", test.getGroup().asCharString());
    snprintf(_test, sizeof(_test), "%s", test.getName().asCharString());
    _detail[0] = '\0';
    _failed = false;
    IavQualContext::instance().resetObservations();
    _startNs = IavQualContext::nowNs();
}

void IavQualOutput::printFailure(const TestFailure &failure)
{
    ConsoleTestOutput::printFailure(failure);
    if (!_failed) {
        snprintf(_detail, sizeof(_detail), "%s:%d: %s", failure.getFileName().asCharString(),
                 (int)failure.getFailureLineNumber(), failure.getMessage().asCharString());
    }
    _failed = true;
}

void IavQualOutput::printEclipseErrorInFileOnLine(SimpleString file, size_t lineNumber)
{
    print(file.asCharString());
    print(":");
    print(lineNumber);
    print(": FAILED:");
}

void IavQualOutput::printVisualStudioErrorInFileOnLine(SimpleString file, size_t lineNumber)
{
    printEclipseErrorInFileOnLine(file, lineNumber);
}

void IavQualOutput::printCurrentTestEnded(const TestResult &res)
{
    long long durationUs = (IavQualContext::nowNs() - _startNs) / 1000;
    IavQualContext &ctx = IavQualContext::instance();

    ConsoleTestOutput::printCurrentTestEnded(res);
    if (_mode != IavQualModeBench && _results != NULL) {
        _results->writeTest(_group, _test, !_failed, durationUs, ctx.resultRc(),
                            ctx.resultErrno(), ctx.observations(), _detail);
    }
    ctx.resetObservations();
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
