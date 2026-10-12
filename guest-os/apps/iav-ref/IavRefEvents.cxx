/*
 * IavRefEvents.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, events and timing class (read only): configures poll events for canvas 0, polls the
 * device and waits for each new frame, measuring how often frames arrive; waits for the next image DSP frame
 * with IAV_IOC_WAIT_NEXT_IDSP_FRAME; and times the round trip of a control call. All three are the calls an
 * application uses to pace itself, and the timings are the cost of a call on the environment it runs in
 * (the cost report compares the bare board with a guest).
 *
 *   IavRefEvents [--events <n>] [--waits <n>] [--calls <n>] [--verbose 1]
 *
 * Exit 0 when every wait returned and every event arrived within its bound.
 */

#include "IavRefCommon.hxx"

#include <algorithm>
#include <poll.h>
#include <vector>

using namespace iavref;

namespace {

void report(const char *what, std::vector<double> &us)
{
    std::sort(us.begin(), us.end());
    double sum = 0;

    for (size_t i = 0; i < us.size(); i++) {
        sum += us[i];
    }
    Row("timing").add("what", what).add("samples", (long long)us.size()).add("min_us", us.front())
        .add("p50_us", us[us.size() / 2]).add("p99_us", us[us.size() * 99 / 100]).add("max_us", us.back())
        .add("mean_us", sum / (double)us.size()).print();
}

} // namespace

int main(int argc, char **argv)
{
    const long events = argValue(argc, argv, "--events", 60);
    const long waits = argValue(argc, argv, "--waits", 60);
    const long calls = argValue(argc, argv, "--calls", 1000);
    const bool verbose = argValue(argc, argv, "--verbose", 0) != 0;
    Device dev;
    std::vector<double> gaps, ptsStep, pollUs, waitUs, callUs;
    struct iav_poll_event_config cfg;
    long long last = 0;
    unsigned long long lastPts = 0;
    long timeouts = 0, failed = 0;

    if (!dev.open()) {
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.valid_num = 1;
    cfg.desc_event[0].qid = IAV_DESC_CANVAS;
    cfg.desc_event[0].arg.canvas.canvas_id = 0;
    if (dev.call(IAV_IOC_SET_POLL_EVENT_CFG, &cfg, "IAV_IOC_SET_POLL_EVENT_CFG") != 0) {
        return 1;
    }
    /*
     * An event is the arrival of a frame with a new time stamp: poll wakes when the next frame is available (its
     * wait is the frame interval), and a blocking canvas query then returns that frame. Frame spacing is measured
     * from the completion of the queries and checked against the time stamps (90 kHz ticks).
     */
    for (long i = 0; i < events; i++) {
        struct pollfd p;
        struct iav_querydesc q;
        long long t0 = nowNs();

        p.fd = dev.fd();
        p.events = POLLIN;
        p.revents = 0;
        int rc = poll(&p, 1, 2000);

        if (rc == 0) {
            timeouts++;
            continue;
        }
        if (rc < 0 || (p.revents & POLLIN) == 0) {
            fprintf(stderr, "poll: %s (revents 0x%x)\n", rc < 0 ? strerror(errno) : "no POLLIN", p.revents);
            failed++;
            break;
        }
        pollUs.push_back((double)(nowNs() - t0) / 1000.0);
        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_CANVAS;
        q.arg.canvas.canvas_id = 0;
        q.arg.canvas.non_block_flag = 0;
        if (dev.call(IAV_IOC_QUERY_DESC, &q, "IAV_IOC_QUERY_DESC") != 0) {
            failed++;
            continue;
        }
        long long now = nowNs();

        if (last != 0) {
            gaps.push_back((double)(now - last) / 1000.0);
            ptsStep.push_back((double)(q.arg.canvas.yuv.mono_pts - lastPts));
        }
        last = now;
        lastPts = q.arg.canvas.yuv.mono_pts;
    }
    dev.call(IAV_IOC_CLEAR_POLL_EVENT_CFG, NULL, "IAV_IOC_CLEAR_POLL_EVENT_CFG");

    for (long i = 0; i < waits; i++) {
        long long t = nowNs();

        if (dev.callValue(IAV_IOC_WAIT_NEXT_IDSP_FRAME, 1, "IAV_IOC_WAIT_NEXT_IDSP_FRAME") != 0) {
            failed++;
            continue;
        }
        waitUs.push_back((double)(nowNs() - t) / 1000.0);
        if (verbose) {
            Row("wait").add("seq", (long long)i).add("us", waitUs.back()).print();
        }
    }

    for (long i = 0; i < calls; i++) {
        long long t = nowNs();
        u32 s = 0;

        if (dev.call(IAV_IOC_GET_IAV_STATE, &s, "IAV_IOC_GET_IAV_STATE") != 0) {
            failed++;
            break;
        }
        callUs.push_back((double)(nowNs() - t) / 1000.0);
    }

    if (!gaps.empty()) {
        double sum = 0;

        for (size_t i = 0; i < gaps.size(); i++) {
            sum += gaps[i];
        }
        Row("rate").add("events", (long long)gaps.size() + 1).add("mean_gap_us", sum / (double)gaps.size())
            .add("events_per_s", 1e6 / (sum / (double)gaps.size())).print();
        report("event_gap", gaps);
    }
    if (!pollUs.empty()) {
        report("poll_ready", pollUs);
    }
    if (!ptsStep.empty()) {
        report("frame_pts_step", ptsStep);
    }
    if (!waitUs.empty()) {
        report("wait_next_idsp_frame", waitUs);
    }
    if (!callUs.empty()) {
        report("get_iav_state_call", callUs);
    }
    Row("summary").add("events", (long long)gaps.size() + 1).add("event_timeouts", (long long)timeouts)
        .add("waits", (long long)waitUs.size()).add("calls", (long long)callUs.size()).add("failed", (long long)failed).print();
    if (failed != 0 || timeouts != 0 || gaps.empty() || waitUs.empty() || callUs.empty()) {
        fprintf(stderr, "events or waits did not complete (failed %ld, timeouts %ld)\n", failed, timeouts);
        return 1;
    }
    return 0;
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
