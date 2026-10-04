/*
 * StreamBroadcaster.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "StreamBroadcaster.hxx"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

int stream_broadcaster_init(struct stream_broadcaster *sb, size_t max_frame_size)
{
    if (!sb)
        return -EINVAL;

    memset(sb, 0, sizeof(*sb));
    sb->max_frame_size = max_frame_size > 0 ? max_frame_size : DEFAULT_MAX_FRAME_SIZE;
    sb->frame_buffer = (uint8_t *)malloc(sb->max_frame_size);
    if (!sb->frame_buffer)
        return -ENOMEM;

    pthread_mutex_init(&sb->stream_lock, nullptr);
    pthread_cond_init(&sb->stream_cond, nullptr);
    pthread_mutex_init(&sb->frame_lock, nullptr);
    pthread_cond_init(&sb->frame_cond, nullptr);

    sb->is_running = true;
    sb->is_first_wake = false;
    sb->active_subscribers = 0;
    sb->published_seq = 0;
    sb->frame_len = 0;

    return 0;
}

void stream_broadcaster_destroy(struct stream_broadcaster *sb)
{
    if (!sb) return;

    stream_broadcaster_shutdown(sb);

    pthread_mutex_destroy(&sb->stream_lock);
    pthread_cond_destroy(&sb->stream_cond);
    pthread_mutex_destroy(&sb->frame_lock);
    pthread_cond_destroy(&sb->frame_cond);

    if (sb->frame_buffer) {
        free(sb->frame_buffer);
        sb->frame_buffer = nullptr;
    }
}

void stream_broadcaster_shutdown(struct stream_broadcaster *sb)
{
    if (!sb) return;

    pthread_mutex_lock(&sb->stream_lock);
    sb->is_running = false;
    pthread_cond_broadcast(&sb->stream_cond);
    pthread_mutex_unlock(&sb->stream_lock);

    pthread_mutex_lock(&sb->frame_lock);
    pthread_cond_broadcast(&sb->frame_cond);
    pthread_mutex_unlock(&sb->frame_lock);
}

int stream_broadcaster_register(struct stream_broadcaster *sb, int *out_sub_id)
{
    if (!sb || !out_sub_id)
        return -EINVAL;

    pthread_mutex_lock(&sb->stream_lock);
    if (!sb->is_running) {
        pthread_mutex_unlock(&sb->stream_lock);
        return -ESHUTDOWN;
    }

    if (sb->active_subscribers >= MAX_STREAM_SUBSCRIBERS) {
        pthread_mutex_unlock(&sb->stream_lock);
        return -EBUSY;
    }

    int slot = -1;
    for (int i = 0; i < MAX_STREAM_SUBSCRIBERS; i++) {
        if (!sb->subscriber_active[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&sb->stream_lock);
        return -EMFILE;
    }

    sb->subscriber_active[slot] = true;
    if (sb->active_subscribers == 0) {
        sb->is_first_wake = true;
    }
    sb->active_subscribers++;

    pthread_cond_signal(&sb->stream_cond);
    pthread_mutex_unlock(&sb->stream_lock);

    *out_sub_id = slot;
    return 0;
}

int stream_broadcaster_unregister(struct stream_broadcaster *sb, int sub_id)
{
    if (!sb || sub_id < 0 || sub_id >= MAX_STREAM_SUBSCRIBERS)
        return -EINVAL;

    pthread_mutex_lock(&sb->stream_lock);
    if (sb->subscriber_active[sub_id]) {
        sb->subscriber_active[sub_id] = false;
        if (sb->active_subscribers > 0)
            sb->active_subscribers--;
    }
    pthread_mutex_unlock(&sb->stream_lock);

    return 0;
}

int stream_broadcaster_wait_active(struct stream_broadcaster *sb, uint32_t timeout_ms)
{
    if (!sb)
        return -EINVAL;

    pthread_mutex_lock(&sb->stream_lock);
    while (sb->is_running && sb->active_subscribers == 0) {
        if (timeout_ms == 0) {
            pthread_cond_wait(&sb->stream_cond, &sb->stream_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            int rc = pthread_cond_timedwait(&sb->stream_cond, &sb->stream_lock, &ts);
            if (rc == ETIMEDOUT && sb->active_subscribers == 0) {
                pthread_mutex_unlock(&sb->stream_lock);
                return -ETIMEDOUT;
            }
        }
    }

    if (!sb->is_running) {
        pthread_mutex_unlock(&sb->stream_lock);
        return -ESHUTDOWN;
    }

    pthread_mutex_unlock(&sb->stream_lock);
    return 0;
}

int stream_broadcaster_publish(struct stream_broadcaster *sb,
                              const void *data, size_t len,
                              uint64_t *out_seq)
{
    if (!sb || !data)
        return -EINVAL;
    if (len > sb->max_frame_size)
        return -EMSGSIZE;

    pthread_mutex_lock(&sb->frame_lock);
    if (!sb->is_running) {
        pthread_mutex_unlock(&sb->frame_lock);
        return -ESHUTDOWN;
    }

    memcpy(sb->frame_buffer, data, len);
    sb->frame_len = len;
    sb->published_seq++;

    /* Reset first wake predicate upon publishing frame */
    pthread_mutex_lock(&sb->stream_lock);
    sb->is_first_wake = false;
    pthread_mutex_unlock(&sb->stream_lock);

    if (out_seq)
        *out_seq = sb->published_seq;

    pthread_cond_broadcast(&sb->frame_cond);
    pthread_mutex_unlock(&sb->frame_lock);

    return 0;
}

int stream_broadcaster_fetch(struct stream_broadcaster *sb,
                             uint64_t *client_last_seq,
                             void *out_buf, size_t max_len,
                             size_t *out_len,
                             uint32_t timeout_ms)
{
    if (!sb || !client_last_seq || !out_buf || !out_len)
        return -EINVAL;

    pthread_mutex_lock(&sb->frame_lock);
    while (sb->is_running && sb->published_seq <= *client_last_seq) {
        if (timeout_ms == 0) {
            pthread_cond_wait(&sb->frame_cond, &sb->frame_lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            int rc = pthread_cond_timedwait(&sb->frame_cond, &sb->frame_lock, &ts);
            if (rc == ETIMEDOUT && sb->published_seq <= *client_last_seq) {
                pthread_mutex_unlock(&sb->frame_lock);
                return -ETIMEDOUT;
            }
        }
    }

    if (!sb->is_running) {
        pthread_mutex_unlock(&sb->frame_lock);
        return -ESHUTDOWN;
    }

    if (sb->frame_len > max_len) {
        pthread_mutex_unlock(&sb->frame_lock);
        return -EMSGSIZE;
    }

    memcpy(out_buf, sb->frame_buffer, sb->frame_len);
    *out_len = sb->frame_len;
    *client_last_seq = sb->published_seq;

    pthread_mutex_unlock(&sb->frame_lock);
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
