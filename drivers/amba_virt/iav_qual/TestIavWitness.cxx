/*
 * TestIavWitness.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reads live canvas frames through the descriptor and the mapping, and records each frame's
 * timestamp (mono_pts, which identifies a frame across descriptors) and a hash of its Y plane. A Dom0 witness (the candidate server, reading the
 * same frames through its own mapping) logs the same pair, so a gate can check that a frame the
 * guest read is the frame the DSP wrote.
 */

#include "IavFrameHash.hxx"
#include "IavTestSupport.hxx"

#include <stdint.h>
#include <stdio.h>

#include <CppUTest/TestHarness.h>

using namespace iavtest;



TEST_GROUP(IavWitness)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavWitness, FramesHaveTimestampsAndDistinctContent)
{
    ScopedMap map;
    unsigned long addr = 0, length = 0;
    unsigned long long pts[24];
    unsigned long long hashes[24];
    int got = 0, distinct = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    CHECK_TRUE(map.map(_f.fd(), addr, length, PROT_READ));
    for (int tries = 0; tries < 400 && got < 24; tries++) {
        struct iav_querydesc q;
        size_t yBytes;

        if (queryCanvas(_f.fd(), true, &q) != 0) {
            continue;
        }
        yBytes = (size_t)q.arg.canvas.yuv.pitch * q.arg.canvas.yuv.height;
        if (yBytes == 0 || q.arg.canvas.yuv.y_addr_offset + yBytes > length) {
            continue;
        }
        if (got > 0 && pts[got - 1] == q.arg.canvas.yuv.mono_pts) {
            continue;
        }
        pts[got] = q.arg.canvas.yuv.mono_pts;
        hashes[got] = iavFrameHash(map.bytes() + q.arg.canvas.yuv.y_addr_offset, yBytes);
        got++;
    }
    CHECK_TEXT(got >= 8, "fewer than eight distinct frames were read");
    for (int i = 0; i < got; i++) {
        char key[24];

        snprintf(key, sizeof(key), "pts_%d", i);
        note(key, (long long)pts[i]);
        snprintf(key, sizeof(key), "hash_%d", i);
        note(key, (long long)hashes[i]);
        if (i == 0 || hashes[i] != hashes[0]) {
            distinct++;
        }
    }
    CHECK_TEXT(distinct >= 2, "every frame read had the same content");
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
