/*
 * TestStreamBroadcaster.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../StreamBroadcaster.hxx"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>

TEST_GROUP(StreamBroadcaster)
{
    struct stream_broadcaster sb;

    void setup() override
    {
        LONGS_EQUAL(0, stream_broadcaster_init(&sb, 1024 * 1024));
    }

    void teardown() override
    {
        stream_broadcaster_destroy(&sb);
    }
};

TEST(StreamBroadcaster, StateTransitions)
{
    /* 0 subscribers -> idle / sleeping state */
    LONGS_EQUAL(0, sb.active_subscribers);
    CHECK_FALSE(sb.is_first_wake);

    /* Register Subscriber 1 -> active, first wake */
    int sub1 = -1;
    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &sub1));
    LONGS_EQUAL(0, sub1);
    LONGS_EQUAL(1, sb.active_subscribers);
    CHECK(sb.is_first_wake);

    /* Publish Frame 1 */
    const char frame1_data[] = "FRAME_1_PAYLOAD";
    uint64_t seq1 = 0;
    LONGS_EQUAL(0, stream_broadcaster_publish(&sb, frame1_data, sizeof(frame1_data), &seq1));
    LONGS_EQUAL(1, seq1);
    CHECK_FALSE(sb.is_first_wake);

    /* Sub 1 fetches Frame 1 */
    uint64_t sub1_seq = 0;
    char fetch_buf[256] = { 0 };
    size_t fetch_len = 0;
    LONGS_EQUAL(0, stream_broadcaster_fetch(&sb, &sub1_seq, fetch_buf, sizeof(fetch_buf), &fetch_len, 100));
    LONGS_EQUAL(1, sub1_seq);
    LONGS_EQUAL(sizeof(frame1_data), fetch_len);
    STRCMP_EQUAL(frame1_data, fetch_buf);

    /* Register Subscriber 2 */
    int sub2 = -1;
    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &sub2));
    LONGS_EQUAL(1, sub2);
    LONGS_EQUAL(2, sb.active_subscribers);

    /* Publish Frame 2 */
    const char frame2_data[] = "FRAME_2_PAYLOAD";
    uint64_t seq2 = 0;
    LONGS_EQUAL(0, stream_broadcaster_publish(&sb, frame2_data, sizeof(frame2_data), &seq2));
    LONGS_EQUAL(2, seq2);

    /* Both subscribers receive Frame 2 */
    uint64_t sub2_seq = 0;
    LONGS_EQUAL(0, stream_broadcaster_fetch(&sb, &sub1_seq, fetch_buf, sizeof(fetch_buf), &fetch_len, 100));
    LONGS_EQUAL(2, sub1_seq);
    STRCMP_EQUAL(frame2_data, fetch_buf);

    LONGS_EQUAL(0, stream_broadcaster_fetch(&sb, &sub2_seq, fetch_buf, sizeof(fetch_buf), &fetch_len, 100));
    LONGS_EQUAL(2, sub2_seq);
    STRCMP_EQUAL(frame2_data, fetch_buf);

    /* Unregister Subscriber 1 */
    LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, sub1));
    LONGS_EQUAL(1, sb.active_subscribers);

    /* Unregister Subscriber 2 */
    LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, sub2));
    LONGS_EQUAL(0, sb.active_subscribers);

    /* Worker wait times out with 0 subscribers */
    LONGS_EQUAL(-ETIMEDOUT, stream_broadcaster_wait_active(&sb, 20));
}

TEST(StreamBroadcaster, CapacityLimit)
{
    int subs[5];

    /* Register max 4 subscribers */
    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, stream_broadcaster_register(&sb, &subs[i]));
    }
    LONGS_EQUAL(4, sb.active_subscribers);

    /* 5th registration must be rejected with -EBUSY */
    LONGS_EQUAL(-EBUSY, stream_broadcaster_register(&sb, &subs[4]));

    /* Unregister one slot and re-register */
    LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, subs[1]));
    LONGS_EQUAL(3, sb.active_subscribers);

    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &subs[1]));
    LONGS_EQUAL(4, sb.active_subscribers);

    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, subs[i]));
    }
    LONGS_EQUAL(0, sb.active_subscribers);
}

TEST(StreamBroadcaster, SlowSubscriberFrameDrop)
{
    int fast_sub = -1, slow_sub = -1;
    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &fast_sub));
    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &slow_sub));

    uint64_t fast_seq = 0;
    uint64_t slow_seq = 0;
    char buf[64];
    size_t out_len = 0;

    /* Publish 30 frames */
    for (int i = 1; i <= 30; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "FRAME_%02d", i);
        LONGS_EQUAL(0, stream_broadcaster_publish(&sb, msg, strlen(msg) + 1, nullptr));

        /* Fast subscriber consumes every frame */
        LONGS_EQUAL(0, stream_broadcaster_fetch(&sb, &fast_seq, buf, sizeof(buf), &out_len, 50));
        LONGS_EQUAL((uint64_t)i, fast_seq);
    }

    /* Slow subscriber has not fetched since start. Now it fetches once */
    LONGS_EQUAL(0, stream_broadcaster_fetch(&sb, &slow_seq, buf, sizeof(buf), &out_len, 50));

    /* Slow subscriber skipped intermediate frames and jumped directly to newest seq 30 */
    LONGS_EQUAL(30, slow_seq);
    STRCMP_EQUAL("FRAME_30", buf);

    stream_broadcaster_unregister(&sb, fast_sub);
    stream_broadcaster_unregister(&sb, slow_sub);
}

TEST(StreamBroadcaster, AbruptDisconnectReap)
{
    int sub = -1;
    LONGS_EQUAL(0, stream_broadcaster_register(&sb, &sub));
    LONGS_EQUAL(1, sb.active_subscribers);

    /* Abrupt disconnect while waiting */
    LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, sub));
    LONGS_EQUAL(0, sb.active_subscribers);

    /* Double unregister does not underflow */
    LONGS_EQUAL(0, stream_broadcaster_unregister(&sb, sub));
    LONGS_EQUAL(0, sb.active_subscribers);
}

struct WorkerCtx {
    struct stream_broadcaster *sb;
    std::atomic<bool> stop_flag;
    std::atomic<uint32_t> frames_published;
    std::atomic<uint32_t> frames_consumed[4];
};

static void *producer_routine(void *arg)
{
    auto *ctx = static_cast<WorkerCtx *>(arg);
    while (!ctx->stop_flag.load()) {
        if (stream_broadcaster_wait_active(ctx->sb, 10) == 0) {
            char frame_data[64];
            uint32_t cur = ctx->frames_published.fetch_add(1);
            snprintf(frame_data, sizeof(frame_data), "CYCLE_FRAME_%u", cur);
            stream_broadcaster_publish(ctx->sb, frame_data, strlen(frame_data) + 1, nullptr);
            usleep(500); /* 2000 FPS stress */
        }
    }
    return nullptr;
}

static void *consumer_routine(void *arg)
{
    struct ThreadParam {
        WorkerCtx *ctx;
        int id;
    };
    auto *param = static_cast<ThreadParam *>(arg);
    WorkerCtx *ctx = param->ctx;
    int id = param->id;
    delete param;

    int sub_id = -1;
    if (stream_broadcaster_register(ctx->sb, &sub_id) != 0)
        return nullptr;

    uint64_t last_seq = 0;
    char buf[128];
    size_t out_len = 0;

    while (!ctx->stop_flag.load()) {
        int rc = stream_broadcaster_fetch(ctx->sb, &last_seq, buf, sizeof(buf), &out_len, 20);
        if (rc == 0) {
            ctx->frames_consumed[id].fetch_add(1);
        }
    }

    stream_broadcaster_unregister(ctx->sb, sub_id);
    return nullptr;
}

TEST(StreamBroadcaster, Concurrency100Cycles)
{
    WorkerCtx ctx;
    ctx.sb = &sb;
    ctx.stop_flag.store(false);
    ctx.frames_published.store(0);
    for (int i = 0; i < 4; i++) ctx.frames_consumed[i].store(0);

    pthread_t prod_thread;
    pthread_create(&prod_thread, nullptr, producer_routine, &ctx);

    pthread_t cons_threads[4];
    for (int i = 0; i < 4; i++) {
        struct ThreadParam { WorkerCtx *ctx; int id; };
        auto *param = new ThreadParam{ &ctx, i };
        pthread_create(&cons_threads[i], nullptr, consumer_routine, param);
    }

    /* Run concurrency stress for 100ms */
    usleep(100000);

    /* Signal threads to finish */
    ctx.stop_flag.store(true);
    stream_broadcaster_shutdown(&sb);

    pthread_join(prod_thread, nullptr);
    for (int i = 0; i < 4; i++) {
        pthread_join(cons_threads[i], nullptr);
    }

    CHECK(ctx.frames_published.load() > 0);
    for (int i = 0; i < 4; i++) {
        CHECK(ctx.frames_consumed[i].load() > 0);
    }
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
