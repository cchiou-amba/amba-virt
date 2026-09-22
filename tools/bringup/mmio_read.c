/*
 * tools/bringup/mmio_read.c
 *
 * Direct physical memory / MMIO 32-bit register reader via /dev/mem.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <phys_addr_hex>\n", argv[0]);
        return 1;
    }

    uint64_t target_addr = strtoull(argv[1], NULL, 0);
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    uint64_t page_base = target_addr & ~(page_size - 1);
    uint64_t page_offset = target_addr - page_base;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    void *map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    volatile uint32_t *reg = (volatile uint32_t *)((uint8_t *)map + page_offset);
    uint32_t val = *reg;

    printf("0x%016lx = 0x%08x\n", (unsigned long)target_addr, val);

    munmap(map, page_size);
    close(fd);
    return 0;
}
