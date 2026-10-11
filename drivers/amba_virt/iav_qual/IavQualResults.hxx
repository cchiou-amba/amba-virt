/*
 * IavQualResults.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAVQUAL_IAVQUALRESULTS_HXX
#define IAVQUAL_IAVQUALRESULTS_HXX

#include <pthread.h>
#include <stdio.h>

/*
 * The results file: JSON Lines, one object per line, no header row. seq is unique and
 * consecutive from 1. Rows are built in fixed buffers and flushed as they are written, so a
 * crash keeps everything up to the last completed row. Nothing here allocates from the heap,
 * because it runs inside tests that are checked for leaks.
 */
class IavQualResults
{
public:
    IavQualResults();
    ~IavQualResults();

    /* Create or truncate the file. The identity strings go into every row. */
    bool open(const char *path, const char *runId, const char *board, const char *backend);
    void close();

    /* obsJson is a complete JSON object text, for example {"x":1}. */
    void writeTest(const char *group, const char *test, bool passed, long long durationUs,
                   int rc, int err, const char *obsJson, const char *detail);
    void writeBench(const char *bench, const char *unit, long long value, const char *workload);

    unsigned long count() const;

    /* Append s to buf as an escaped JSON string body (no quotes). Returns the new length. */
    static size_t escapeInto(char *buf, size_t cap, size_t pos, const char *s);

private:
    void writeRow(const char *row);

    FILE *_file;
    pthread_mutex_t _lock;
    unsigned long _seq;
    char _runId[96];
    char _board[96];
    char _backend[48];
};

#endif /* IAVQUAL_IAVQUALRESULTS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
