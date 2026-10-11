/*
 * IavQualOutput.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVQUALOUTPUT_HXX
#define IAVQUAL_IAVQUALOUTPUT_HXX

#include "IavQualConfig.hxx"
#include "IavQualResults.hxx"

#include <CppUTest/TestOutput.h>

/*
 * The console output of CppUTest, plus one result row per finished test. In bench mode the
 * benchmark tests write their own rows and no test rows are written.
 */
class IavQualOutput : public ConsoleTestOutput
{
public:
    IavQualOutput(IavQualResults *results, IavQualMode mode);

    virtual void printCurrentTestStarted(const UtestShell &test);
    virtual void printFailure(const TestFailure &failure);
    virtual void printCurrentTestEnded(const TestResult &res);

protected:
    /*
     * The gate specs treat the text "error:" in a log as a compiler diagnostic, and CppUTest
     * prints "file:line: error: Failure in TEST(...)" by default. Print the same location
     * with a different word, so a failing test is not mistaken for a compile failure.
     */
    virtual void printEclipseErrorInFileOnLine(SimpleString file, size_t lineNumber);
    virtual void printVisualStudioErrorInFileOnLine(SimpleString file, size_t lineNumber);

private:
    IavQualResults *_results;
    IavQualMode _mode;
    char _group[96];
    char _test[96];
    char _detail[400];
    bool _failed;
    long long _startNs;
};

#endif /* IAVQUAL_IAVQUALOUTPUT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
