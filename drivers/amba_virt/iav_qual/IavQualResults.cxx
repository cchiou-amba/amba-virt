/*
 * IavQualResults.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "IavQualResults.hxx"

#include <string.h>
#include <unistd.h>

IavQualResults::IavQualResults() :
    _file(NULL),
    _seq(0)
{
    pthread_mutex_init(&_lock, NULL);
    _runId[0] = _board[0] = _backend[0] = '\0';
}

IavQualResults::~IavQualResults()
{
    close();
    pthread_mutex_destroy(&_lock);
}

bool IavQualResults::open(const char *path, const char *runId, const char *board,
                          const char *backend)
{
    close();
    _file = fopen(path, "w");
    if (_file == NULL) {
        return false;
    }
    _seq = 0;
    snprintf(_runId, sizeof(_runId), "%s", runId);
    snprintf(_board, sizeof(_board), "%s", board);
    snprintf(_backend, sizeof(_backend), "%s", backend);
    return true;
}

void IavQualResults::close()
{
    if (_file != NULL) {
        fflush(_file);
        fsync(fileno(_file));
        fclose(_file);
        _file = NULL;
    }
}

unsigned long IavQualResults::count() const
{
    return _seq;
}

size_t IavQualResults::escapeInto(char *buf, size_t cap, size_t pos, const char *s)
{
    for (; s != NULL && *s != '\0' && pos + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
            buf[pos++] = (char)c;
        } else if (c == '\n') {
            buf[pos++] = '\\';
            buf[pos++] = 'n';
        } else if (c == '\t') {
            buf[pos++] = '\\';
            buf[pos++] = 't';
        } else if (c < 0x20 || c >= 0x7f) {
            buf[pos++] = '?';
        } else {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return pos;
}

void IavQualResults::writeRow(const char *row)
{
    if (_file == NULL) {
        return;
    }
    fputs(row, _file);
    fputc('\n', _file);
    fflush(_file);
}

void IavQualResults::writeTest(const char *group, const char *test, bool passed,
                               long long durationUs, int rc, int err, const char *obsJson,
                               const char *detail)
{
    char row[4096];
    char det[512];

    det[0] = '\0';
    escapeInto(det, sizeof(det), 0, detail != NULL ? detail : "");

    pthread_mutex_lock(&_lock);
    _seq++;
    snprintf(row, sizeof(row),
             "{\"seq\":%lu,\"run_id\":\"%s\",\"board\":\"%s\",\"backend\":\"%s\","
             "\"group\":\"%s\",\"test\":\"%s\",\"status\":\"%s\",\"duration_us\":%lld,"
             "\"rc\":%d,\"errno\":%d,\"obs\":%s,\"detail\":\"%s\"}",
             _seq, _runId, _board, _backend, group, test, passed ? "pass" : "fail", durationUs,
             rc, err, obsJson != NULL ? obsJson : "{}", det);
    writeRow(row);
    pthread_mutex_unlock(&_lock);
}

void IavQualResults::writeBench(const char *bench, const char *unit, long long value,
                                const char *workload)
{
    char row[1024];
    char wl[384];

    wl[0] = '\0';
    escapeInto(wl, sizeof(wl), 0, workload != NULL ? workload : "");

    pthread_mutex_lock(&_lock);
    _seq++;
    snprintf(row, sizeof(row),
             "{\"seq\":%lu,\"run_id\":\"%s\",\"board\":\"%s\",\"backend\":\"%s\","
             "\"bench\":\"%s\",\"unit\":\"%s\",\"value\":%lld,\"workload\":\"%s\"}",
             _seq, _runId, _board, _backend, bench, unit, value, wl);
    writeRow(row);
    pthread_mutex_unlock(&_lock);
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
