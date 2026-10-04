/*
 * drivers/amba_virt/tools/iav_tap_abi.h
 *
 * ABI definitions for the IAV Frame Tap bounded ring buffer.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_TAP_ABI_H
#define IAV_TAP_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IAV_TAP_RING_SLOTS          4
#define IAV_TAP_MAX_PAYLOAD_SIZE    (1920 * 1080 * 3 / 2) /* 1080p YUV420 */
#define IAV_TAP_MAX_JPEG_SIZE       (512 * 1024)          /* 512 KB MJPEG frame */

enum iav_tap_slot_state {
    IAV_TAP_SLOT_EMPTY     = 0,
    IAV_TAP_SLOT_WRITING   = 1,
    IAV_TAP_SLOT_PUBLISHED = 2,
    IAV_TAP_SLOT_HELD      = 3,
};

struct iav_tap_slot {
    uint32_t state;
    uint32_t refcount;
    uint64_t seq;
    uint64_t generation;
    uint64_t sof_count;
    uint64_t dsp_pts;
    uint64_t mono_pts;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t fourcc;
    uint32_t nbytes;
    uint64_t copy_time_us;
    uint32_t jpeg_size;
    uint8_t  jpeg_payload[IAV_TAP_MAX_JPEG_SIZE] __attribute__((aligned(4096)));
    uint8_t  payload[IAV_TAP_MAX_PAYLOAD_SIZE] __attribute__((aligned(4096)));
};

struct iav_tap_ring {
    struct iav_tap_slot slots[IAV_TAP_RING_SLOTS];
    uint64_t published_count;
    uint64_t drop_count;      /* frames seen but not published: ring full */
    uint64_t missed_count;    /* frames the descriptor query never returned */
    uint32_t active_width;
    uint32_t active_height;
    uint32_t active_pitch;
    uint32_t active_fourcc;
};

#ifdef __cplusplus
}
#endif

#endif /* IAV_TAP_ABI_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
