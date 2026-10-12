/*
 * IavRefPipeline.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, primary class (state changing): drives the encode pipeline. It stops a running
 * MJPEG stream, changes the stream's JPEG quality while the stream is idle, starts it again and checks that
 * the frames now carry the new quality; then restores the original quality the same way and checks again.
 * The state after every step is recorded. A guest run is correct when its recorded sequence equals the
 * native run's, and the DSP is healthy afterwards (read from Dom0 by the gate).
 *
 * The pipeline belongs to everyone who uses it: this changes the stream for them. Run it on a board that is
 * set up for it (the frame tap not running).
 *
 *   IavRefPipeline [--stream <id>] [--quality <q>]
 *   IavRefPipeline --probe denied|hazard
 *
 * Exit 0 when every step succeeded and every check held; on a failure it tries to put the stream back.
 *
 * The probe modes are negative controls and change nothing even if a call were allowed (every argument is
 * harmless): `denied` issues calls only the primary guest may make and expects EPERM from each (run it in a guest
 * that is not the primary); `hazard` issues calls no guest may make and expects EPERM from each (run it anywhere).
 */

#include "IavRefCommon.hxx"

using namespace iavref;

namespace {

bool waitState(Device &dev, int want, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        if (dev.state() == want) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

/* Reads frames until the last `need` carry the quality `q`, or gives up. Returns the quality of the last frame. */
int framesCarry(Device &dev, long stream, int q, int need, int limit)
{
    int run = 0, last = -1;

    for (int i = 0; i < limit && run < need; i++) {
        struct iav_querydesc d;

        memset(&d, 0, sizeof(d));
        d.qid = IAV_DESC_FRAME;
        d.arg.frame.id = (u32)stream;
        d.arg.frame.time_ms = 2000;
        if (dev.call(IAV_IOC_QUERY_DESC, &d, "IAV_IOC_QUERY_DESC", true) != 0) {
            if (errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "IAV_IOC_QUERY_DESC: %s\n", strerror(errno));
            return -2;
        }
        last = (int)d.arg.frame.jpeg_quality;
        run = last == q ? run + 1 : 0;
    }
    return run >= need ? q : last;
}

} // namespace

struct Step {
    Device &dev;
    long stream;
    int failures;

    Step(Device &d, long s) : dev(d), stream(s), failures(0) {}

    void record(const char *name, int rc, int quality)
    {
        Row("step").add("name", name).add("rc", (long long)rc).add("state", (long long)dev.state())
            .add("quality", (long long)quality).print();
        if (rc != 0) {
            failures++;
        }
    }

    int getQuality()
    {
        struct iav_mjpeg_cfg c;

        memset(&c, 0, sizeof(c));
        c.id = (u32)stream;
        return dev.call(IAV_IOC_GET_MJPEG_CONFIG, &c, "IAV_IOC_GET_MJPEG_CONFIG") == 0 ? (int)c.quality : -1;
    }

    /* stop, set quality, start, and check the frames. */
    bool change(int q)
    {
        struct iav_mjpeg_cfg c;
        int got;

        record("stop_encode", dev.callValue(IAV_IOC_STOP_ENCODE, 1UL << stream, "IAV_IOC_STOP_ENCODE"), -1);
        record("state_preview", waitState(dev, IAV_STATE_PREVIEW, 3000) ? 0 : 1, -1);
        memset(&c, 0, sizeof(c));
        c.id = (u32)stream;
        if (dev.call(IAV_IOC_GET_MJPEG_CONFIG, &c, "IAV_IOC_GET_MJPEG_CONFIG") != 0) {
            record("get_config", 1, -1);
            return false;
        }
        c.quality = (u32)q;
        record("set_quality", dev.call(IAV_IOC_SET_MJPEG_CONFIG, &c, "IAV_IOC_SET_MJPEG_CONFIG"), q);
        record("start_encode", dev.callValue(IAV_IOC_START_ENCODE, 1UL << stream, "IAV_IOC_START_ENCODE"), q);
        record("state_encoding", waitState(dev, IAV_STATE_ENCODING, 3000) ? 0 : 1, q);
        got = framesCarry(dev, stream, q, 5, 60);
        record("frames_carry_quality", got == q ? 0 : 1, got);
        return failures == 0;
    }
};

/* Calls with harmless arguments, each expected to fail with EPERM; one row per call. */
int probe(Device &dev, const char *which)
{
    int failures = 0;
    struct Call {
        const char *name;
        unsigned long cmd;
        bool value;
    };
    const bool denied = strcmp(which, "denied") == 0;
    unsigned char buf[1024];

    memset(buf, 0, sizeof(buf));
    if (denied) {
        struct iav_mjpeg_cfg c;
        const Call calls[] = {
            {"IAV_IOC_GET_MJPEG_CONFIG", IAV_IOC_GET_MJPEG_CONFIG, false},
            {"IAV_IOC_STOP_ENCODE", IAV_IOC_STOP_ENCODE, true},
            {"IAV_IOC_START_ENCODE", IAV_IOC_START_ENCODE, true},
        };

        memset(&c, 0, sizeof(c));
        c.id = 1;
        for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
            int rc = calls[i].value ? dev.callValue(calls[i].cmd, 0, calls[i].name, true)
                                    : dev.call(calls[i].cmd, &c, calls[i].name, true);
            int e = errno;
            bool ok = rc != 0 && e == EPERM;

            Row("probe").add("which", which).add("call", calls[i].name).add("rc", (long long)rc)
                .add("errno", (long long)(rc != 0 ? e : 0)).add("expected_eperm", (long long)ok).print();
            if (!ok) {
                failures++;
            }
        }
    } else {
        const Call calls[] = {
            {"IAV_IOC_GET_DEBUG_CONFIG", IAV_IOC_GET_DEBUG_CONFIG, false},
            {"IAV_IOC_ALLOC_MEM_PART", IAV_IOC_ALLOC_MEM_PART, false},
        };

        for (size_t i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
            int rc = dev.call(calls[i].cmd, buf, calls[i].name, true);
            int e = errno;
            bool ok = rc != 0 && e == EPERM;

            Row("probe").add("which", which).add("call", calls[i].name).add("rc", (long long)rc)
                .add("errno", (long long)(rc != 0 ? e : 0)).add("expected_eperm", (long long)ok).print();
            if (!ok) {
                failures++;
            }
        }
    }
    Row("summary").add("probe", which).add("failures", (long long)failures).print();
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const long stream = argValue(argc, argv, "--stream", 1);
    const long requested = argValue(argc, argv, "--quality", 0);
    Device dev;
    int s0, q0, qt;

    if (!dev.open()) {
        return 1;
    }
    {
        const char *which = argString(argc, argv, "--probe", NULL);

        if (which != NULL) {
            return probe(dev, which);
        }
    }
    Step st(dev, stream);

    s0 = dev.state();
    st.record("initial_state", (s0 == IAV_STATE_ENCODING) ? 0 : 1, -1);
    q0 = st.getQuality();
    st.record("initial_quality", q0 > 0 ? 0 : 1, q0);
    if (st.failures != 0) {
        fprintf(stderr, "the stream is not running as expected (state %d, quality %d)\n", s0, q0);
        return 1;
    }
    qt = requested > 0 ? (int)requested : (q0 >= 60 ? q0 - 30 : q0 + 30);
    if (qt == q0) {
        qt = q0 > 1 ? q0 - 1 : q0 + 1;
    }
    if (!st.change(qt)) {
        fprintf(stderr, "changing the quality to %d failed; restoring %d\n", qt, q0);
        st.change(q0);
        return 1;
    }
    if (!st.change(q0)) {
        fprintf(stderr, "restoring the quality %d failed\n", q0);
        return 1;
    }
    Row("summary").add("initial_quality", (long long)q0).add("changed_quality", (long long)qt)
        .add("final_state", (long long)dev.state()).add("failures", (long long)st.failures).print();
    return st.failures == 0 && dev.state() == s0 ? 0 : 1;
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
