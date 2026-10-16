/*
 * MockIavDriver.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBAVIRT_TEST_MOCKIAVDRIVER_HXX
#define AMBAVIRT_TEST_MOCKIAVDRIVER_HXX

#include <stdint.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "iav_broker.h"
}

/*
 * A stand-in for the real IAV driver that doubles as the oracle of the broker tests. It records a
 * violation when anything reaches it that the broker must never forward: an ioctl that is not on
 * the allow-list, a call on a descriptor that is not open, a wait with no bound, a dma-buf fd
 * flag, or a poll event outside the allowed set. A test that sees a violation has found a hole
 * in the broker's checks. Time is a fake clock that only sleep_ms and advance() move, so the
 * bounded waits are deterministic.
 */
class MockIavDriver
{
public:
    MockIavDriver();

    /* The ops table to give the broker. */
    struct iav_broker_ops ops();

    /* What the next calls do. */
    void failOpensWith(int err);                 /* 0: opens succeed */
    void setEagainCount(int n);                  /* non-blocking descriptor queries fail n times */
    void failIoctl(unsigned long cmd, int err);  /* the ioctl returns -1 with err; err 0 clears */
    void setPollReady(bool ready);
    void advance(uint64_t ms);
    void setSeed(uint32_t seed);
    /* Let the ioctls of the primary set reach the driver without a violation. */
    void allowPrimarySet(bool on) { _primarySet = on; }
    /* The guest id the broker is told is the primary client; 0: none. */
    void setPrimaryCid(uint32_t cid) { _primaryCid = cid; }
    /* Keep a copy of the argument of the next call of cmd that reaches the driver (len bytes). */
    void captureArgOf(unsigned long cmd, size_t len);
    std::vector<uint8_t> capturedArg();
    /* The pipeline state the pinning checks compare with (GET_CHAN_CONFIG, GET_SYSTEM_RESOURCE). */
    void setChannel(unsigned int chanId, bool preDec, unsigned int vsrcId, bool swCe);
    void setChanMap(uint32_t chanMap);
    /* Where the decode partition is now, and where ENABLE_PREVIEW puts it (0, 0: it stays). */
    void setDecodePartition(uint64_t base, uint64_t len);
    /* The reported lengths of IAV_PART_BLUR and IAV_PART_SMOOTH_BLUR (0, absent, by default, as on the boards). */
    void setBlurPartitions(uint64_t blurLen, uint64_t smoothBlurLen);
    /* max_stream_num and stream_map of GET_SYSTEM_RESOURCE (3 and 0x7 by default). */
    void setStreams(unsigned int maxStreams, uint32_t streamMap);
    void moveDecodeOnPreview(uint64_t base, uint64_t len);
    /*
     * The pinned private pool behind the arenas (QUERY_MEMBLOCK of PoolPid answers it), and the ranges of it, as
     * offsets from its start, that the guest under test owns: a buffer in the pool outside every such range is a
     * violation (another guest's slice, or the claim page). setPool(.., 0) makes the pool absent.
     */
    void setPool(uint64_t base, uint64_t len);
    void allowArena(uint64_t start, uint64_t len);
    void clearArena();
    /*
     * The aux aperture (Envelope F of plan_iav_usb_yolo): the broker is told (aperture_of) that guest `cid` has these
     * entries (pid, base, len, offset) in a BAR of `size`, and QUERY_MEMBLOCK answers each entry's partition as given.
     * The oracle accepts a table or buffer in one of these partitions only within its entry. setAperture(0, ..) removes
     * it; setAuxPartition moves or adds a partition without touching what the broker is told (to test pinning).
     */
    void setAperture(uint32_t cid, uint64_t size, uint64_t epoch, const struct iav_broker_aux_entry *e, unsigned int n);
    void setAuxPartition(uint32_t pid, uint64_t base, uint64_t len);
    /*
     * What the broker reads at any offset of the decode partition when it checks a JPEG (read_dec): by default a
     * baseline 4:2:0 header the check admits. setDecodeContent replaces it; failDecodeRead makes the read fail.
     */
    void setDecodeContent(const std::vector<uint8_t> &bytes);
    void failDecodeRead(bool fail);
    static std::vector<uint8_t> jpegHeader(unsigned int hv0 = 0x22, unsigned int hv1 = 0x11, unsigned int hv2 = 0x11,
                                           unsigned int sofMarker = 0xC0, unsigned int restart = 0, bool dqt = true,
                                           bool dht = true);
    /* The standard primary aperture: MASK, WARP, QPMATRIX, CE_GO_MAP, CANVAS_OVERLAY, ARB_BLUR, YUV_REMAP. */
    void setStandardAperture(uint32_t cid);
    static const unsigned int AuxCount = 7;
    static const struct iav_broker_aux_entry StandardAux[AuxCount];
    static const uint64_t StandardAuxSize = 0x4000000ull;
    static const uint64_t StandardAuxEpoch = 5;
    /* Run something inside the next open, before it returns, to race it against a disconnect. */
    void setOpenHook(std::function<void()> hook);

    /* Make the next WAIT_NEXT_IDSP_FRAME block until release() is called. */
    void blockNextWait();
    void waitUntilBlocked();
    void release();

    /* Observations. */
    int openFds();
    unsigned long totalOpens();
    unsigned long totalCloses();
    unsigned long ioctlCalls();
    unsigned long reached(unsigned long cmd);
    uint64_t now();
    std::vector<std::string> violations();
    void clearViolations();

    /* The window layout the broker is told about, matching QUERY_MEMBLOCK here. */
    static const uint64_t DspBase = 0x40000000ull;
    static const uint64_t DspLen = 0x400000ull;
    static const uint64_t BsbBase = 0x84300000ull;
    static const uint64_t BsbLen = 0xa00000ull;
    static const uint64_t BsbWindow = 0x1000000ull;
    static const uint64_t DecBase = 0x91900000ull;
    static const uint64_t DecLen = 0x600000ull;
    static const uint64_t DecWindow = 0x800000ull;
    static const uint64_t OvlBase = 0x28000000ull;
    static const uint64_t OvlLen = 0x800000ull;
    static const uint32_t PoolPid = 193;
    static const uint64_t PoolBase = 0x94000000ull;
    static const uint64_t PoolLen = 0x20000000ull;
    static const uint64_t SliceBytes = 0x4000000ull;
    static const unsigned int BlockMaxMs = 2000;

private:
    static int cOpen(void *ctx);
    static int cClose(void *ctx, int fd);
    static int cIoctl(void *ctx, int fd, unsigned long cmd, unsigned long arg);
    static int cPoll(void *ctx, int fd, int events, int timeoutMs, int *revents);
    static void cSleep(void *ctx, unsigned int ms);
    static uint32_t cRandom(void *ctx);
    static uint64_t cNow(void *ctx);

    int doOpen();
    int doClose(int fd);
    int doIoctl(int fd, unsigned long cmd, unsigned long arg);
    int doPoll(int fd, int events, int timeoutMs, int *revents);
    void violation(const std::string &what);
    std::string imageViolation(unsigned long cmd, unsigned long arg) const;
    uint64_t windowedLen(uint32_t pid) const;
    bool fdOpen(int fd);

    std::mutex _lock;
    std::vector<int> _fds;
    std::vector<unsigned int> _pollConfigured;
    int _nextFd;
    int _openErr;
    int _eagain;
    unsigned long _failCmd;
    unsigned long _capCmd;
    size_t _capLen;
    std::vector<uint8_t> _capBuf;
    bool _primarySet;
    uint32_t _primaryCid;
    static int cIsPrimary(void *ctx, uint32_t cid);
    static int cReadDec(void *ctx, uint64_t off, void *buf, size_t len);
    static int cApertureOf(void *ctx, uint32_t cid, struct iav_broker_aperture *out);
    std::string apertureViolation(unsigned long cmd, unsigned long arg) const;
    uint64_t auxLen(uint32_t pid) const;
    bool overlayInside(const struct iav_overlay_insert *o, bool canvas, uint32_t id) const;
    bool blurInside(const struct iav_blur_insert *b) const;
    int auxPartIndex(uint32_t pid) const;
    std::vector<uint8_t> _decContent;
    bool _decFail;
    uint32_t _apertureCid;
    struct iav_broker_aperture _aperture;
    static const unsigned int MaxAuxParts = 16;
    uint32_t _auxPid[MaxAuxParts];
    uint64_t _auxBase[MaxAuxParts], _auxLen[MaxAuxParts];
    unsigned int _nauxParts;
    int _failErr;
    bool _pollReady;
    uint64_t _now;
    unsigned long _opens, _closes, _ioctls;
    static const int ReachedSlots = 16;
    unsigned long _reachedCount[ReachedSlots];
    static int slotOf(unsigned long cmd);
    std::vector<std::string> _violations;
    unsigned int _waitCounter;
    uint32_t _rnd;
    std::function<void()> _openHook;
    struct Chan
    {
        bool preDec;
        unsigned int vsrc;
        bool swCe;
    };
    static const unsigned int MaxChans = 8;
    Chan _chan[MaxChans];
    uint32_t _chanMap;
    uint64_t _decBase, _decLen, _decNextBase, _decNextLen;
    uint64_t _blurLen, _smoothBlurLen;
    uint64_t _poolBase, _poolLen;
    static const unsigned int MaxArena = 4;
    uint64_t _arenaStart[MaxArena], _arenaLen[MaxArena];
    unsigned int _narena;
    bool arenaInside(unsigned __int128 off, unsigned __int128 len) const;
    bool bufInside(uint32_t pid, unsigned __int128 off, unsigned __int128 len, bool dst) const;
    unsigned int _maxStreams;
    uint32_t _streamMap;
    std::mutex _blockMutex;
    std::condition_variable _blockCv;
    bool _blockNext;
    bool _blocked;
    bool _released;
};

#endif /* AMBAVIRT_TEST_MOCKIAVDRIVER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
