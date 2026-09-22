/*
 * tools/bringup/iav_status.c
 *
 * Query and report current IAV video subsystem and DSP pipeline state.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef IAV_IOC_GET_IAV_STATE
#define IAV_IOC_GET_IAV_STATE _IOR('v', 0x01, uint32_t)
#endif

#ifndef IAV_STATE_INIT
enum {
    IAV_STATE_INIT = 0,
    IAV_STATE_IDLE = 1,
    IAV_STATE_PREVIEW = 2,
    IAV_STATE_ENCODING = 3,
    IAV_STATE_STILL_CAPTURE = 4,
    IAV_STATE_DECODING = 5,
    IAV_STATE_TRANSCODING = 6,
    IAV_STATE_DUPLEX = 7,
    IAV_STATE_EXITING_PREVIEW = 8,
    IAV_STATE_ACTIVATING_PREVIEW = 9,
};
#endif

static const char *state_to_str(int state) {
    switch (state) {
        case IAV_STATE_INIT: return "INIT";
        case IAV_STATE_IDLE: return "IDLE";
        case IAV_STATE_PREVIEW: return "PREVIEW";
        case IAV_STATE_ENCODING: return "ENCODING";
        case IAV_STATE_STILL_CAPTURE: return "STILL_CAPTURE";
        case IAV_STATE_DECODING: return "DECODING";
        case IAV_STATE_TRANSCODING: return "TRANSCODING";
        case IAV_STATE_DUPLEX: return "DUPLEX";
        case IAV_STATE_EXITING_PREVIEW: return "EXITING_PREVIEW";
        case IAV_STATE_ACTIVATING_PREVIEW: return "ACTIVATING_PREVIEW";
        default: return "UNKNOWN";
    }
}

int main(void) {
    int fd = open("/dev/iav", O_RDWR);
    if (fd < 0) {
        perror("open /dev/iav");
        return 1;
    }

    uint32_t state = 0;
    if (ioctl(fd, IAV_IOC_GET_IAV_STATE, &state) < 0) {
        perror("ioctl IAV_IOC_GET_IAV_STATE");
        close(fd);
        return 1;
    }

    printf("=====================================\n");
    printf("  IAV Device: /dev/iav [ONLINE]\n");
    printf("  IAV State : %d (%s)\n", (int)state, state_to_str((int)state));
    printf("=====================================\n");

    close(fd);
    return 0;
}
