/*
 * StreamBroadcaster.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_STREAM_BROADCASTER_HXX
#define AMBA_VIRT_STREAM_BROADCASTER_HXX

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_STREAM_SUBSCRIBERS 4
#define DEFAULT_MAX_FRAME_SIZE (2 * 1024 * 1024)

struct stream_broadcaster {
    pthread_mutex_t stream_lock;
    pthread_cond_t stream_cond;
    uint32_t active_subscribers;
    bool is_first_wake;
    bool is_running;

    pthread_mutex_t frame_lock;
    pthread_cond_t frame_cond;
    uint64_t published_seq;
    uint8_t *frame_buffer;
    size_t frame_len;
    size_t max_frame_size;

    bool subscriber_active[MAX_STREAM_SUBSCRIBERS];
};

/* Initialize broadcaster and allocate frame buffer */
int stream_broadcaster_init(struct stream_broadcaster *sb, size_t max_frame_size);

/* Teardown broadcaster and free resources */
void stream_broadcaster_destroy(struct stream_broadcaster *sb);

/* Signal all threads to shutdown */
void stream_broadcaster_shutdown(struct stream_broadcaster *sb);

/*
 * Register a new streaming subscriber.
 * Capped at MAX_STREAM_SUBSCRIBERS (4).
 * Returns subscriber slot index on success (>= 0),
 * or -EBUSY / -EMFILE if capacity reached.
 */
int stream_broadcaster_register(struct stream_broadcaster *sb, int *out_sub_id);

/*
 * Unregister a streaming subscriber.
 * Automatically decrements active subscriber count.
 */
int stream_broadcaster_unregister(struct stream_broadcaster *sb, int sub_id);

/*
 * Worker wait helper:
 * If active_subscribers == 0, waits on stream_cond until a subscriber arrives or timeout.
 * Returns 0 if active subscribers exist,
 * -ETIMEDOUT if timeout elapsed with 0 subscribers,
 * -ESHUTDOWN if broadcaster is shutting down.
 */
int stream_broadcaster_wait_active(struct stream_broadcaster *sb, uint32_t timeout_ms);

/*
 * Publish a new complete frame to all waiting subscribers.
 * Atomically updates published_seq and signals frame_cond.
 * Resets is_first_wake to false.
 */
int stream_broadcaster_publish(struct stream_broadcaster *sb,
                              const void *data, size_t len,
                              uint64_t *out_seq);

/*
 * Subscriber fetch helper:
 * Waits for published_seq > *client_last_seq.
 * Copies latest frame to out_buf, updates *client_last_seq.
 * Slow consumers drop intermediate frames cleanly.
 * Returns 0 on success,
 * -ETIMEDOUT if timeout elapsed,
 * -ESHUTDOWN if broadcaster is shutting down,
 * -EMSGSIZE if out_buf capacity is insufficient.
 */
int stream_broadcaster_fetch(struct stream_broadcaster *sb,
                             uint64_t *client_last_seq,
                             void *out_buf, size_t max_len,
                             size_t *out_len,
                             uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AMBA_VIRT_STREAM_BROADCASTER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
