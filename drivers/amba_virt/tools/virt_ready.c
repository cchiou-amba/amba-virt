/*
 * virt_ready.c
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "virt_ready.h"

bool virt_ready_shm_present(const char *dev_dir)
{
    DIR *d = opendir(dev_dir);
    struct dirent *e;
    bool found = false;

    if (!d)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, VIRT_READY_SHM_PREFIX, strlen(VIRT_READY_SHM_PREFIX)) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

int virt_ready_wait_shm(const char *dev_dir, unsigned int timeout_ms)
{
    struct timespec step = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
    unsigned int waited = 0;

    for (;;) {
        if (virt_ready_shm_present(dev_dir))
            return 0;
        if (waited >= timeout_ms)
            return -ETIMEDOUT;
        nanosleep(&step, NULL);
        waited += 50;
    }
}

void virt_ready_notify(int fd)
{
    char b = VIRT_READY_BYTE;
    ssize_t n;

    if (fd < 0)
        return;
    do {
        n = write(fd, &b, 1);
    } while (n < 0 && errno == EINTR);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
