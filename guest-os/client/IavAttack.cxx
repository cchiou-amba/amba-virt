/*
 * IavAttack.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The Level 8 adversarial client for the IAV broker. It runs inside a guest and talks to the
 * broker over raw vsock, bypassing iav_virt.ko, the way a hostile program with root in the guest
 * would: malformed and oversized frames, forged and stale handles, ioctls that are not allowed,
 * out-of-window mappings, floods, slow readers, and disconnects in the middle of a request.
 *
 * It prints one JSON line per probe. A probe passes when the broker answers with one of the
 * outcomes it is allowed to give (a refusal, or a closed connection for a malformed frame). After
 * each group it checks that a well-formed client is still served. Whether the server itself stayed
 * healthy (threads, descriptors, memory, other guests) is observed from outside by the caller.
 *
 * Exit codes: 0 every probe behaved, 1 a probe failed, 2 usage, 3 the broker is not reachable.
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#include <string>
#include <vector>

#include <amba_virt.h>
#include <amba_virt_iav.h>
#include <amba_virt_iav_table.h>

#include <iav_ioctl.h>
#include <ambas_imgproc_ioctl_arch.h>

namespace {

struct Entry
{
    unsigned int cmd;
    const char *name;
    int kind;
    int dir;
    size_t size;
    bool allowed;
};

#define IAV_IOCTL(name, magic, kind, dir, size, cls, flags) {name, #name, kind, dir, (size), false},
Entry g_table[] = {
#include "iav_ioctl_table.inc"
};
#undef IAV_IOCTL

/* The ioctls the broker is meant to allow. Everything else must be refused. */
const char *const AllowedNames[] = {
    "IAV_IOC_GET_IAV_STATE", "IAV_IOC_GET_CHIP_ID", "IAV_IOC_QUERY_ENC_MODE_CAP",
    "IAV_IOC_QUERY_ENC_BUF_CAP", "IAV_IOC_QUERY_IAV_CAP", "IAV_IOC_GET_SYSTEM_RESOURCE",
    "IAV_IOC_GET_CHAN_CONFIG", "IAV_IOC_GET_CANVAS_CONFIG", "IAV_IOC_DRV_DSP_INFO",
    "IAV_IOC_QUERY_INFO", "IAV_IOC_QUERY_MEMBLOCK", "IAV_IOC_QUERY_DESC",
    "IAV_IOC_SET_POLL_EVENT_CFG", "IAV_IOC_CLEAR_POLL_EVENT_CFG",
    "IAV_IOC_WAIT_NEXT_IDSP_FRAME", NULL};

/* Allowed ioctls that are harmless to call with an all-zero argument. */
const char *const SafeCalls[] = {"IAV_IOC_GET_IAV_STATE", "IAV_IOC_GET_CHIP_ID", "IAV_IOC_QUERY_IAV_CAP",
                                 "IAV_IOC_GET_SYSTEM_RESOURCE", "IAV_IOC_DRV_DSP_INFO", NULL};

long long nowMs()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum Wire
{
    WireOk,
    WireTimeout,
    WireClosed,
    WireBad
};

class Conn
{
public:
    Conn() : _fd(-1), _seq(1) {}
    ~Conn() { close(); }

    bool connect()
    {
        struct sockaddr_vm sa;

        close();
        _fd = ::socket(AF_VSOCK, SOCK_STREAM, 0);
        if (_fd < 0) {
            return false;
        }
        memset(&sa, 0, sizeof(sa));
        sa.svm_family = AF_VSOCK;
        sa.svm_cid = AMBA_VIRT_VSOCK_CID;
        sa.svm_port = AMBA_VIRT_VSOCK_PORT;
        if (::connect(_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close();
            return false;
        }
        return true;
    }

    void close()
    {
        if (_fd >= 0) {
            ::close(_fd);
            _fd = -1;
        }
    }

    bool open() const { return _fd >= 0; }

    /* Raw bytes, no framing. */
    bool sendRaw(const void *p, size_t n)
    {
        const uint8_t *b = (const uint8_t *)p;

        while (n > 0) {
            ssize_t k = ::send(_fd, b, n, MSG_NOSIGNAL);

            if (k <= 0) {
                return false;
            }
            b += k;
            n -= (size_t)k;
        }
        return true;
    }

    bool sendFrame(const void *payload, uint32_t len)
    {
        return sendRaw(&len, sizeof(len)) && (len == 0 || sendRaw(payload, len));
    }

    /* One response frame, or why not. */
    Wire recvFrame(std::vector<uint8_t> &out, int timeoutMs)
    {
        uint32_t len = 0;
        Wire w = recvExact(&len, sizeof(len), timeoutMs);

        if (w != WireOk) {
            return w;
        }
        if (len == 0 || len > AMBA_VIRT_MAX_MSG) {
            return WireBad;
        }
        out.resize(len);
        return recvExact(&out[0], len, timeoutMs);
    }

    uint32_t nextSeq() { return _seq++; }

private:
    Wire recvExact(void *p, size_t n, int timeoutMs)
    {
        uint8_t *b = (uint8_t *)p;
        long long deadline = nowMs() + timeoutMs;

        while (n > 0) {
            struct pollfd pf;
            long long left = deadline - nowMs();
            ssize_t k;

            if (left < 0) {
                return WireTimeout;
            }
            pf.fd = _fd;
            pf.events = POLLIN;
            pf.revents = 0;
            if (::poll(&pf, 1, (int)left) <= 0) {
                return WireTimeout;
            }
            k = ::recv(_fd, b, n, 0);
            if (k == 0) {
                return WireClosed;
            }
            if (k < 0) {
                return errno == EINTR ? WireTimeout : WireClosed;
            }
            b += k;
            n -= (size_t)k;
        }
        return WireOk;
    }

    int _fd;
    uint32_t _seq;
};

/* A decoded answer. */
struct Answer
{
    Wire wire;
    bool parsed;
    struct amba_virt_iav_resp resp;
    std::vector<uint8_t> out;
};

/* Build and send a request. arg may be NULL. */
bool sendReq(Conn &c, uint32_t type, uint32_t proto, uint32_t op, uint64_t handle, uint32_t cmd,
             uint32_t argLen, uint64_t argValue, uint64_t arg2, const void *arg, size_t argBytes)
{
    std::vector<uint8_t> f(sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_iav_req) + argBytes);
    struct amba_virt_msg msg;
    struct amba_virt_iav_req req;

    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.seq = c.nextSeq();
    msg.shm_len = (uint32_t)(sizeof(req) + argBytes);
    memset(&req, 0, sizeof(req));
    req.proto = proto;
    req.op = op;
    req.handle = handle;
    req.cmd = cmd;
    req.arg_len = argLen;
    req.arg_value = argValue;
    req.arg2 = arg2;
    memcpy(&f[0], &msg, sizeof(msg));
    memcpy(&f[sizeof(msg)], &req, sizeof(req));
    if (argBytes) {
        memcpy(&f[sizeof(msg) + sizeof(req)], arg, argBytes);
    }
    return c.sendFrame(&f[0], (uint32_t)f.size());
}

Answer recvAnswer(Conn &c, int timeoutMs)
{
    Answer a;
    std::vector<uint8_t> f;

    memset(&a.resp, 0, sizeof(a.resp));
    a.parsed = false;
    a.wire = c.recvFrame(f, timeoutMs);
    if (a.wire != WireOk) {
        return a;
    }
    if (f.size() < sizeof(struct amba_virt_msg) + sizeof(a.resp)) {
        a.wire = WireBad;
        return a;
    }
    memcpy(&a.resp, &f[sizeof(struct amba_virt_msg)], sizeof(a.resp));
    if (a.resp.out_len <= f.size() - sizeof(struct amba_virt_msg) - sizeof(a.resp)) {
        a.out.assign(f.begin() + (long)(sizeof(struct amba_virt_msg) + sizeof(a.resp)),
                     f.begin() + (long)(sizeof(struct amba_virt_msg) + sizeof(a.resp) + a.resp.out_len));
        a.parsed = true;
    }
    return a;
}

Answer call(Conn &c, uint32_t op, uint64_t handle, uint32_t cmd, uint32_t argLen, uint64_t argValue,
            uint64_t arg2, const void *arg, size_t argBytes, int timeoutMs = 3000)
{
    Answer a;

    if (!sendReq(c, AMBA_VIRT_MSG_IAV_REQ, AMBA_VIRT_IAV_PROTO, op, handle, cmd, argLen, argValue, arg2,
                 arg, argBytes)) {
        a.wire = WireClosed;
        a.parsed = false;
        memset(&a.resp, 0, sizeof(a.resp));
        return a;
    }
    return recvAnswer(c, timeoutMs);
}

/* ---- reporting ---- */

int g_failures = 0;
int g_probes = 0;

void report(const char *group, const std::string &probe, bool ok, const char *outcome, const std::string &detail)
{
    g_probes++;
    if (!ok) {
        g_failures++;
    }
    printf("{\"group\":\"%s\",\"probe\":\"%s\",\"result\":\"%s\",\"outcome\":\"%s\",\"detail\":\"%s\"}\n",
           group, probe.c_str(), ok ? "pass" : "fail", outcome, detail.c_str());
    fflush(stdout);
}

std::string errnoText(int e)
{
    char b[48];

    snprintf(b, sizeof(b), "status=%d", e);
    return b;
}

/* A refusal is a non-zero status that is a negative errno. */
bool refused(const Answer &a)
{
    return a.wire == WireOk && a.parsed && a.resp.status < 0;
}

/* The broker closed the connection, which is an acceptable answer to a malformed frame. */
bool closedOrRefused(const Answer &a)
{
    return a.wire == WireClosed || a.wire == WireBad || refused(a);
}

/* A well-formed client is still served: open, read the state, close. */
bool healthy(std::string &why)
{
    Conn c;
    Answer a;
    uint64_t h;
    unsigned char arg[64];

    if (!c.connect()) {
        why = "cannot connect";
        return false;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0 || a.resp.handle == 0) {
        why = "open failed " + errnoText(a.resp.status);
        return false;
    }
    h = a.resp.handle;
    memset(arg, 0, sizeof(arg));
    a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_GET_IAV_STATE, 0, 0, 0, NULL, 0);
    (void)arg;
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        why = "state ioctl failed " + errnoText(a.resp.status);
        call(c, AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, 0, NULL, 0);
        return false;
    }
    a = call(c, AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        why = "close failed";
        return false;
    }
    return true;
}

void checkHealthy(const char *group)
{
    std::string why;
    bool ok = false;

    /* a briefly busy broker is allowed a few tries */
    for (int i = 0; i < 5 && !ok; i++) {
        ok = healthy(why);
        if (!ok) {
            usleep(500 * 1000);
        }
    }
    report(group, "still_serves_a_well_formed_client", ok, ok ? "served" : "not_served", ok ? "" : why);
}

/* ---- the probes ---- */

void probeFraming()
{
    const char *g = "framing";
    Conn c;
    Answer a;
    std::vector<uint8_t> junk(AMBA_VIRT_MAX_MSG);

    /* zero-length frame */
    if (c.connect()) {
        uint32_t zero = 0;

        c.sendRaw(&zero, sizeof(zero));
        a = recvAnswer(c, 1500);
        report(g, "zero_length_frame", a.wire != WireOk || refused(a),
               a.wire == WireOk ? "answered" : "closed_or_silent", "");
    }
    /* length far over the maximum */
    if (c.connect()) {
        uint32_t big = 0xffffffffu;

        c.sendRaw(&big, sizeof(big));
        c.sendRaw("x", 1);
        a = recvAnswer(c, 1500);
        report(g, "length_0xffffffff", closedOrRefused(a) || a.wire == WireTimeout, "closed_or_silent", "");
    }
    /* just over the maximum, with a body */
    if (c.connect()) {
        memset(&junk[0], 0x41, junk.size());
        uint32_t len = AMBA_VIRT_MAX_MSG + 1;

        c.sendRaw(&len, sizeof(len));
        c.sendRaw(&junk[0], junk.size());
        c.sendRaw("y", 1);
        a = recvAnswer(c, 1500);
        report(g, "length_max_plus_one", closedOrRefused(a) || a.wire == WireTimeout, "closed_or_silent", "");
    }
    /* header promises 100 bytes, 10 arrive, then the peer goes away */
    if (c.connect()) {
        uint32_t len = 100;

        c.sendRaw(&len, sizeof(len));
        c.sendRaw("0123456789", 10);
        c.close();
        report(g, "truncated_frame_then_close", true, "closed", "");
    }
    /* header only, then silence for a while, then the peer goes away */
    if (c.connect()) {
        uint32_t len = 64;

        c.sendRaw(&len, sizeof(len));
        usleep(3 * 1000 * 1000);
        c.close();
        report(g, "header_only_then_stall", true, "closed", "");
    }
    /* random bytes in valid frames */
    srand(12345);
    for (int round = 0; round < 200; round++) {
        size_t n = 1 + (size_t)(rand() % 600);

        if (!c.open() && !c.connect()) {
            break;
        }
        for (size_t i = 0; i < n; i++) {
            junk[i] = (uint8_t)rand();
        }
        if (!c.sendFrame(&junk[0], (uint32_t)n)) {
            c.close();
        }
        a = recvAnswer(c, 50);
        if (a.wire == WireClosed || a.wire == WireBad) {
            c.close();
        }
    }
    report(g, "random_frames_200", true, "sent", "");
    checkHealthy(g);
}

void probeMessages()
{
    const char *g = "messages";
    Conn c;
    Answer a;
    uint64_t h = 0;

    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    /* a response sent as a request */
    sendReq(c, AMBA_VIRT_MSG_IAV_RESP, AMBA_VIRT_IAV_PROTO, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    a = recvAnswer(c, 800);
    report(g, "wrong_message_type", a.wire != WireOk || refused(a), a.wire == WireOk ? "answered" : "silent", "");

    if (!c.open()) {
        c.connect();
    }
    sendReq(c, AMBA_VIRT_MSG_IAV_REQ, 99, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    a = recvAnswer(c, 1500);
    report(g, "wrong_proto", closedOrRefused(a) || a.wire == WireTimeout, "refused_or_closed", errnoText(a.resp.status));

    const uint32_t badOps[] = {0, 7, 8, 255, 0xffffffffu};

    for (size_t i = 0; i < sizeof(badOps) / sizeof(badOps[0]); i++) {
        char name[32];

        if (!c.open() && !c.connect()) {
            break;
        }
        sendReq(c, AMBA_VIRT_MSG_IAV_REQ, AMBA_VIRT_IAV_PROTO, badOps[i], 0, 0, 0, 0, 0, NULL, 0);
        a = recvAnswer(c, 1500);
        snprintf(name, sizeof(name), "unknown_op_%u", badOps[i]);
        report(g, name, closedOrRefused(a) || a.wire == WireTimeout, "refused_or_closed", errnoText(a.resp.status));
    }

    /* arg_len that disagrees with the frame */
    const uint32_t lies[] = {1, 100, 4096, 0xffffffffu};

    if (!c.open() && !c.connect()) {
        return;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    h = (a.wire == WireOk && a.parsed && a.resp.status == 0) ? a.resp.handle : 0;
    report(g, "open_for_the_rest", h != 0, h ? "opened" : "refused", errnoText(a.resp.status));
    for (size_t i = 0; i < sizeof(lies) / sizeof(lies[0]); i++) {
        char name[40];

        if (!c.open() && !c.connect()) {
            break;
        }
        a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_GET_IAV_STATE, lies[i], 0, 0, NULL, 0, 1500);
        snprintf(name, sizeof(name), "arg_len_lie_%u", lies[i]);
        report(g, name, closedOrRefused(a), "refused_or_closed", errnoText(a.resp.status));
    }
    checkHealthy(g);
}

void probeHandles()
{
    const char *g = "handles";
    Conn c;
    Answer a;
    uint64_t h = 0;

    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        report(g, "open", false, "refused", errnoText(a.resp.status));
        return;
    }
    h = a.resp.handle;

    const uint64_t forged[] = {0, ~0ULL, h ^ 1, h ^ (1ULL << 63), h + 1, h - 1, h ^ 0xffffffff00000000ULL,
                               h ^ 0xffffULL, 1, 0x100000001ULL};

    for (size_t i = 0; i < sizeof(forged) / sizeof(forged[0]); i++) {
        char name[48];

        if (forged[i] == h) {
            continue;
        }
        a = call(c, AMBA_VIRT_IAV_OP_IOCTL, forged[i], IAV_IOC_GET_IAV_STATE, 0, 0, 0, NULL, 0);
        snprintf(name, sizeof(name), "forged_handle_%zu", i);
        report(g, name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
        a = call(c, AMBA_VIRT_IAV_OP_CLOSE, forged[i], 0, 0, 0, 0, NULL, 0);
        snprintf(name, sizeof(name), "forged_close_%zu", i);
        report(g, name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
        a = call(c, AMBA_VIRT_IAV_OP_POLL, forged[i], POLLIN, 0, 10, 0, NULL, 0);
        snprintf(name, sizeof(name), "forged_poll_%zu", i);
        report(g, name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
        a = call(c, AMBA_VIRT_IAV_OP_MAP, forged[i], 1, 0, 0x1000, 0x1000, NULL, 0);
        snprintf(name, sizeof(name), "forged_map_%zu", i);
        report(g, name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
    }

    /* the real handle still works after all that */
    a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_GET_IAV_STATE, 0, 0, 0, NULL, 0);
    report(g, "real_handle_still_works", a.wire == WireOk && a.parsed && a.resp.status == 0, "served", "");

    /* close twice, then use */
    a = call(c, AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, 0, NULL, 0);
    report(g, "close_once", a.wire == WireOk && a.parsed && a.resp.status == 0, "closed", "");
    a = call(c, AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, 0, NULL, 0);
    report(g, "close_twice", refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
    a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_GET_IAV_STATE, 0, 0, 0, NULL, 0);
    report(g, "use_after_close", refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
    checkHealthy(g);
}

void probeIoctls()
{
    const char *g = "ioctls";
    Conn c;
    Answer a;
    uint64_t h = 0;
    int allowedCount = 0, refusedCount = 0, wrongAllowed = 0, wrongRefused = 0;
    std::string leaked;

    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
        for (const char *const *n = AllowedNames; *n; n++) {
            if (strcmp(*n, g_table[i].name) == 0) {
                g_table[i].allowed = true;
            }
        }
    }
    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        report(g, "open", false, "refused", errnoText(a.resp.status));
        return;
    }
    h = a.resp.handle;

    /* An ioctl number that is not in the table. */
    a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, 0xdeadbeefu, 0, 0, 0, NULL, 0);
    report(g, "unknown_ioctl_number", refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
    a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, 0, 0, 0, 0, NULL, 0);
    report(g, "ioctl_number_zero", refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));

    /*
     * Every ioctl the driver serves, called with an all-zero argument of the table's size. Those on
     * the allow-list that are safe to call with zeros are called and must be served; the rest of
     * the allow-list is not called here (their zero arguments can block or change what a
     * later test sees). Everything not on the list must be refused.
     */
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
        const Entry &e = g_table[i];
        std::vector<uint8_t> arg;
        uint32_t argLen = 0;
        bool safe = false;

        if (e.allowed) {
            for (const char *const *n = SafeCalls; *n; n++) {
                if (strcmp(*n, e.name) == 0) {
                    safe = true;
                }
            }
            if (!safe) {
                continue;
            }
        }
        if (e.kind == IAV_ARG_PTR && (e.dir & IAV_DIR_W)) {
            if (e.size == 0 || e.size > AMBA_VIRT_IAV_MAX_ARG) {
                continue;
            }
            arg.assign(e.size, 0);
            argLen = (uint32_t)e.size;
        }
        a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, e.cmd, argLen, 0, 0, arg.empty() ? NULL : &arg[0], arg.size());
        if (a.wire != WireOk || !a.parsed) {
            /* a closed connection is not a served call; reconnect with a fresh session */
            c.connect();
            Answer o = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);

            h = (o.wire == WireOk && o.parsed && o.resp.status == 0) ? o.resp.handle : 0;
            if (!e.allowed) {
                refusedCount++;
            }
            continue;
        }
        if (e.allowed) {
            allowedCount++;
            if (a.resp.status != 0) {
                wrongRefused++;
                leaked += std::string("[refused-but-allowed ") + e.name + "]";
            }
        } else if (a.resp.status < 0) {
            refusedCount++;
        } else {
            wrongAllowed++;
            leaked += std::string("[ACCEPTED ") + e.name + "]";
        }
    }
    {
        char d[96];

        snprintf(d, sizeof(d), "refused=%d allowed_called=%d accepted_wrongly=%d refused_wrongly=%d",
                 refusedCount, allowedCount, wrongAllowed, wrongRefused);
        report(g, "table_sweep_everything_else_refused", wrongAllowed == 0 && wrongRefused == 0,
               wrongAllowed ? "ACCEPTED_A_DENIED_IOCTL" : "ok", std::string(d) + leaked);
    }

    /* Allowed ioctls with an argument length that does not match the table. */
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
        const Entry &e = g_table[i];
        const size_t lies[] = {1, 0, 8, 100000};

        if (!e.allowed || e.kind != IAV_ARG_PTR || e.size == 0 || e.size > AMBA_VIRT_IAV_MAX_ARG) {
            continue;
        }
        for (size_t k = 0; k < sizeof(lies) / sizeof(lies[0]); k++) {
            std::vector<uint8_t> arg(lies[k] > AMBA_VIRT_IAV_MAX_ARG ? 16 : lies[k], 0);
            char name[96];
            uint32_t argLen = (uint32_t)lies[k];

            if (lies[k] == e.size && (e.dir & IAV_DIR_W)) {
                continue;
            }
            if (!(e.dir & IAV_DIR_W) && lies[k] == 0) {
                continue;
            }
            a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, e.cmd, argLen, 0, 0, arg.empty() ? NULL : &arg[0],
                     arg.size(), 1500);
            if (a.wire != WireOk) {
                c.connect();
                Answer o = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);

                h = (o.wire == WireOk && o.parsed && o.resp.status == 0) ? o.resp.handle : 0;
                continue;
            }
            snprintf(name, sizeof(name), "wrong_arg_len_%s_%zu", e.name, lies[k]);
            report(g, name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
        }
    }
    checkHealthy(g);
}

void probeMap()
{
    const char *g = "map";
    Conn c;
    Answer a;
    uint64_t h = 0;
    struct amba_virt_iav_window w[AMBA_VIRT_IAV_MAX_WINDOWS];
    unsigned int n = 0;

    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        report(g, "open", false, "refused", errnoText(a.resp.status));
        return;
    }
    h = a.resp.handle;
    a = call(c, AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire == WireOk && a.parsed && a.resp.status == 0 && a.out.size() % sizeof(w[0]) == 0) {
        n = (unsigned int)(a.out.size() / sizeof(w[0]));
        if (n > AMBA_VIRT_IAV_MAX_WINDOWS) {
            n = AMBA_VIRT_IAV_MAX_WINDOWS;
        }
        memcpy(w, &a.out[0], n * sizeof(w[0]));
    }
    report(g, "info", n > 0, n ? "windows_listed" : "no_windows", "");
    if (n == 0) {
        return;
    }
    {
        const uint64_t base = w[0].base, len = w[0].part_len;
        struct Case
        {
            const char *name;
            uint64_t addr, length;
            uint32_t prot;
        };
        const Case cases[] = {
            {"address_zero", 0, 0x1000, 1},
            {"address_below_base", base - 0x1000, 0x1000, 1},
            {"address_inside_not_base", base + 0x1000, 0x1000, 1},
            {"address_unaligned", base + 1, 0x1000, 1},
            {"address_all_ones", ~0ULL, 0x1000, 1},
            {"length_zero", base, 0, 1},
            {"length_unaligned", base, 0x1001, 1},
            {"length_beyond_window", base, len + 0x1000, 1},
            {"length_overflow", base, ~0ULL & ~0xfffULL, 1},
            {"address_plus_length_wraps", ~0ULL & ~0xfffULL, 0x2000, 1},
            {"prot_exec", base, 0x1000, 4},
            {"prot_none", base, 0x1000, 0},
            {"prot_write_only", base, 0x1000, 2},
            {"prot_high_bits", base, 0x1000, 0x80000001u},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            a = call(c, AMBA_VIRT_IAV_OP_MAP, h, cases[i].prot, 0, cases[i].addr, cases[i].length, NULL, 0);
            report(g, cases[i].name, refused(a), refused(a) ? "refused" : "ACCEPTED", errnoText(a.resp.status));
        }
        /* The one valid request must still work. */
        a = call(c, AMBA_VIRT_IAV_OP_MAP, h, 1, 0, base, len, NULL, 0);
        report(g, "valid_map_authorized", a.wire == WireOk && a.parsed && a.resp.status == 0, "authorized",
               errnoText(a.resp.status));
    }
    checkHealthy(g);
}

void probeBlocking()
{
    const char *g = "blocking";
    Conn c;
    Answer a;
    uint64_t h = 0;
    long long t0;

    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    a = call(c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
    if (a.wire != WireOk || !a.parsed || a.resp.status != 0) {
        report(g, "open", false, "refused", errnoText(a.resp.status));
        return;
    }
    h = a.resp.handle;
    /* a huge poll timeout is cut to a slice */
    t0 = nowMs();
    a = call(c, AMBA_VIRT_IAV_OP_POLL, h, POLLIN, 0, 0xffffffffffffffffULL, 0, NULL, 0, 4000);
    report(g, "poll_with_an_enormous_timeout_is_bounded", a.wire == WireOk && nowMs() - t0 < 1500, "bounded",
           std::to_string(nowMs() - t0) + " ms");
    t0 = nowMs();
    a = call(c, AMBA_VIRT_IAV_OP_POLL, h, POLLIN, 0, 3600000, 0, NULL, 0, 4000);
    report(g, "poll_one_hour_is_bounded", a.wire == WireOk && nowMs() - t0 < 1500, "bounded",
           std::to_string(nowMs() - t0) + " ms");
    /* a frame descriptor query with time_ms 0 (forever) and with 0x7fffffff */
    {
        struct iav_querydesc q;

        memset(&q, 0, sizeof(q));
        q.qid = IAV_DESC_FRAME;
        q.arg.frame.time_ms = 0;
        t0 = nowMs();
        a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_QUERY_DESC, sizeof(q), 0, 0, &q, sizeof(q), 6000);
        report(g, "frame_query_forever_is_bounded", a.wire == WireOk && nowMs() - t0 < 4000, "bounded",
               std::to_string(nowMs() - t0) + " ms");
        q.arg.frame.time_ms = 0x7fffffff;
        t0 = nowMs();
        a = call(c, AMBA_VIRT_IAV_OP_IOCTL, h, IAV_IOC_QUERY_DESC, sizeof(q), 0, 0, &q, sizeof(q), 6000);
        report(g, "frame_query_huge_timeout_is_bounded", a.wire == WireOk && nowMs() - t0 < 4000, "bounded",
               std::to_string(nowMs() - t0) + " ms");
    }
    call(c, AMBA_VIRT_IAV_OP_CLOSE, h, 0, 0, 0, 0, NULL, 0);
    checkHealthy(g);
}

void probeExhaustion()
{
    const char *g = "exhaustion";
    std::vector<Conn *> conns;
    std::vector<uint64_t> handles;
    int opened = 0, refusedOpens = 0;

    /* open sessions until the broker refuses */
    {
        Conn *c = new Conn();

        if (c->connect()) {
            for (int i = 0; i < 4000; i++) {
                Answer a = call(*c, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0, 1500);

                if (a.wire != WireOk) {
                    break;
                }
                if (a.parsed && a.resp.status == 0) {
                    handles.push_back(a.resp.handle);
                    opened++;
                } else {
                    refusedOpens++;
                    if (refusedOpens > 3) {
                        break;
                    }
                }
            }
            report(g, "session_quota_is_enforced", refusedOpens > 0, refusedOpens ? "refused_at_limit" : "NO_LIMIT",
                   "opened=" + std::to_string(opened));
            for (size_t i = 0; i < handles.size(); i++) {
                call(*c, AMBA_VIRT_IAV_OP_CLOSE, handles[i], 0, 0, 0, 0, NULL, 0, 1500);
            }
        } else {
            report(g, "connect", false, "no_connection", "");
        }
        conns.push_back(c);
    }
    for (size_t i = 0; i < conns.size(); i++) {
        delete conns[i];
    }
    checkHealthy(g);

    /* open and vanish: the broker must reclaim sessions of a client that is gone */
    for (int i = 0; i < 300; i++) {
        Conn c;

        if (!c.connect()) {
            usleep(20 * 1000);
            continue;
        }
        sendReq(c, AMBA_VIRT_MSG_IAV_REQ, AMBA_VIRT_IAV_PROTO, AMBA_VIRT_IAV_OP_OPEN, 0, 0, 0, 0, 0, NULL, 0);
        c.close();
    }
    report(g, "open_then_vanish_300", true, "sent", "");
    usleep(3 * 1000 * 1000);
    checkHealthy(g);
}

/* Fire requests as fast as possible without reading a single answer, then read nothing for a while. */
void probeFloodAndSlowReader()
{
    const char *g = "flood";
    Conn c;
    long long t0;
    int sent = 0;

    if (!c.connect()) {
        report(g, "connect", false, "no_connection", "");
        return;
    }
    t0 = nowMs();
    while (nowMs() - t0 < 4000) {
        if (!sendReq(c, AMBA_VIRT_MSG_IAV_REQ, AMBA_VIRT_IAV_PROTO, AMBA_VIRT_IAV_OP_INFO, 0, 0, 0, 0, 0, NULL, 0)) {
            break;
        }
        sent++;
    }
    report(g, "info_flood_without_reading", true, "sent", "requests=" + std::to_string(sent));
    /* the caller samples the server now; hold the connection open without reading */
    usleep(4 * 1000 * 1000);
    c.close();
    usleep(1 * 1000 * 1000);
    checkHealthy(g);
}

} // namespace

int main(int argc, char **argv)
{
    std::string only;
    std::string why;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--only framing|messages|handles|ioctls|map|blocking|exhaustion|flood]\n", argv[0]);
            return 2;
        }
    }
    {
        Conn c;

        if (!c.connect()) {
            fprintf(stderr, "iav-attack: cannot reach the broker over vsock: %s\n", strerror(errno));
            return 3;
        }
    }
    if (!healthy(why)) {
        fprintf(stderr, "iav-attack: the broker does not serve a well-formed client to begin with: %s\n",
                why.c_str());
        return 3;
    }
    if (only.empty() || only == "framing") probeFraming();
    if (only.empty() || only == "messages") probeMessages();
    if (only.empty() || only == "handles") probeHandles();
    if (only.empty() || only == "ioctls") probeIoctls();
    if (only.empty() || only == "map") probeMap();
    if (only.empty() || only == "blocking") probeBlocking();
    if (only.empty() || only == "exhaustion") probeExhaustion();
    if (only.empty() || only == "flood") probeFloodAndSlowReader();
    printf("{\"summary\":true,\"probes\":%d,\"failures\":%d}\n", g_probes, g_failures);
    return g_failures ? 1 : 0;
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
