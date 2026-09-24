/*
 * tools/bringup/set_uart_pinmux.c
 *
 * Configures hardware IOMUX registers for Ambarella UART2 and UART3 on N1-655.
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

#define IOMUX_BASE_ADDR        0xffe4010000ULL
#define IOMUX_REGION_SIZE      0x1000

#define IOMUX_OFFSET(bank, i)  (((bank) * 0xc) + ((i) * 0x4))
#define IOMUX_CTRL_SET_OFFSET  (0xf0)

static void amb_set_altfunc(volatile uint8_t *iomux, uint32_t bank, uint32_t offset, uint32_t altfunc) {
    uint32_t i, data;
    for (i = 0; i < 3; i++) {
        volatile uint32_t *reg = (volatile uint32_t *)(iomux + IOMUX_OFFSET(bank, i));
        data = *reg;
        data &= ~(1U << offset);
        data |= (((altfunc >> i) & 1U) << offset);
        *reg = data;
    }

    volatile uint32_t *ctrl = (volatile uint32_t *)(iomux + IOMUX_CTRL_SET_OFFSET);
    *ctrl = 1U;
    *ctrl = 0U;
}

int main(int argc, char **argv) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    void *map = mmap(NULL, IOMUX_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IOMUX_BASE_ADDR);
    if (map == MAP_FAILED) {
        perror("mmap /dev/mem");
        close(fd);
        return 1;
    }

    volatile uint8_t *iomux = (volatile uint8_t *)map;

    printf("Configuring UART2 (GPIO 113..116 -> Function 1)...\n");
    /* UART2: Bank 3, offsets 17..20 (GPIO 113..116) -> altfunc 1 */
    amb_set_altfunc(iomux, 3, 17, 1);
    amb_set_altfunc(iomux, 3, 18, 1);
    amb_set_altfunc(iomux, 3, 19, 1);
    amb_set_altfunc(iomux, 3, 20, 1);

    printf("Configuring UART3 (GPIO 94..97 -> Function 3)...\n");
    /* UART3: Bank 2, offsets 30, 31 (GPIO 94, 95) -> altfunc 3 */
    amb_set_altfunc(iomux, 2, 30, 3);
    amb_set_altfunc(iomux, 2, 31, 3);
    /* UART3: Bank 3, offsets 0, 1 (GPIO 96, 97) -> altfunc 3 */
    amb_set_altfunc(iomux, 3, 0, 3);
    amb_set_altfunc(iomux, 3, 1, 3);

    printf("IOMUX pinmux configuration for UART2 and UART3 applied successfully.\n");

    munmap(map, IOMUX_REGION_SIZE);
    close(fd);
    return 0;
}
