/*
 * tools/bar_probe.c
 *
 * Cross-guest PCI ivshmem BAR memory isolation verification probe.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printf("Usage: %s <resource_path> <write|read|fill|check_fill> [arg]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    const char *cmd = argv[2];

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    size_t len = 1024 * 1024; /* 1 MB */
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    int ret = 0;
    if (strcmp(cmd, "write") == 0 && argc >= 4) {
        memset(map, 0, 1024);
        strncpy((char *)map, argv[3], 1023);
        asm volatile("dsb sy" ::: "memory");
        printf("WROTE [%s]: '%s'\n", path, argv[3]);
    } else if (strcmp(cmd, "read") == 0) {
        char buf[256] = {0};
        strncpy(buf, (char *)map, sizeof(buf) - 1);
        printf("READ  [%s]: '%s'\n", path, buf);
    } else if (strcmp(cmd, "fill") == 0 && argc >= 4) {
        uint8_t byte = (uint8_t)strtoul(argv[3], NULL, 0);
        memset(map, byte, len);
        asm volatile("dsb sy" ::: "memory");
        printf("FILLED [%s] with 0x%02x (size: %zu KB)\n", path, byte, len / 1024);
    } else if (strcmp(cmd, "check_fill") == 0 && argc >= 4) {
        uint8_t byte = (uint8_t)strtoul(argv[3], NULL, 0);
        uint8_t *p = (uint8_t *)map;
        int mismatch = 0;
        for (size_t i = 0; i < len; i++) {
            if (p[i] != byte) {
                printf("MISMATCH [%s] at offset %zu: expected 0x%02x, got 0x%02x\n",
                       path, i, byte, p[i]);
                mismatch = 1;
                ret = 2;
                break;
            }
        }
        if (!mismatch) {
            printf("PASSED [%s]: 100%% uniform 0x%02x verified across %zu KB\n",
                   path, byte, len / 1024);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        ret = 1;
    }

    munmap(map, len);
    close(fd);
    return ret;
}
