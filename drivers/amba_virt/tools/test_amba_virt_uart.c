/*
 * test_amba_virt_uart.c
 *
 * Unit test harness for Ambarella Virtual UART Host Lease Driver.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include "../include/uapi/amba_virt_uart_uapi.h"

static int test_uart_node(const char *dev_node, uint32_t expected_id, uint64_t expected_base)
{
    printf("\n=== Testing %s (Expected ID: %u, Base: 0x%llx) ===\n",
           dev_node, expected_id, (unsigned long long)expected_base);

    int fd = open(dev_node, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "FAIL: open(%s) failed: %s\n", dev_node, strerror(errno));
        return -1;
    }
    printf("PASS: open(%s) succeeded (fd=%d)\n", dev_node, fd);

    /* 1. Test GET_INFO */
    struct amba_virt_uart_info info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, AMBA_VIRT_UART_IOC_GET_INFO, &info) < 0) {
        fprintf(stderr, "FAIL: ioctl(GET_INFO) failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("PASS: ioctl(GET_INFO) returned: ID=%u, PhysBase=0x%llx, Size=0x%x, IRQ=%u, IRQs=%llu, ACKs=%llu\n",
           info.uart_id, (unsigned long long)info.phys_base, info.size, info.irq,
           (unsigned long long)info.irq_count, (unsigned long long)info.ack_count);

    if (info.uart_id != expected_id || info.phys_base != expected_base || info.size != 4096) {
        fprintf(stderr, "FAIL: info mismatch! (got ID=%u base=0x%llx size=%u)\n",
                info.uart_id, (unsigned long long)info.phys_base, info.size);
        close(fd);
        return -1;
    }

    /* 2. Test MMIO mmap */
    void *mmio = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmio == MAP_FAILED) {
        fprintf(stderr, "FAIL: mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("PASS: mmap succeeded (virt=%p)\n", mmio);

    /* Read first 4 register words (Ambarella UART: 0x00=RB/TH, 0x04=IER, 0x08=IIR/FCR, 0x0c=LCR) */
    volatile uint32_t *regs = (volatile uint32_t *)mmio;
    uint32_t r_ier = regs[1];
    uint32_t r_iir = regs[2];
    uint32_t r_lcr = regs[3];
    printf("      Register Read (offsets 0x04..0x0c): IER=0x%08x, IIR=0x%08x, LCR=0x%08x\n",
           r_ier, r_iir, r_lcr);

    /* 3. Test SET_EVENTFD */
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        fprintf(stderr, "FAIL: eventfd failed: %s\n", strerror(errno));
        munmap(mmio, info.size);
        close(fd);
        return -1;
    }
    if (ioctl(fd, AMBA_VIRT_UART_IOC_SET_EVENTFD, &efd) < 0) {
        fprintf(stderr, "FAIL: ioctl(SET_EVENTFD) failed: %s\n", strerror(errno));
        close(efd);
        munmap(mmio, info.size);
        close(fd);
        return -1;
    }
    printf("PASS: ioctl(SET_EVENTFD) bound eventfd %d\n", efd);

    /* 4. Test ACK_IRQ */
    if (ioctl(fd, AMBA_VIRT_UART_IOC_ACK_IRQ) < 0) {
        fprintf(stderr, "FAIL: ioctl(ACK_IRQ) failed: %s\n", strerror(errno));
        close(efd);
        munmap(mmio, info.size);
        close(fd);
        return -1;
    }
    printf("PASS: ioctl(ACK_IRQ) unmask executed\n");

    /* 5. Test Mutual Exclusion (second open should return EBUSY) */
    int fd2 = open(dev_node, O_RDWR);
    if (fd2 >= 0) {
        fprintf(stderr, "FAIL: second open(%s) succeeded but expected EBUSY!\n", dev_node);
        close(fd2);
        close(efd);
        munmap(mmio, info.size);
        close(fd);
        return -1;
    }
    if (errno == EBUSY) {
        printf("PASS: second open returned EBUSY (mutual exclusion verified)\n");
    } else {
        printf("WARN: second open returned %s instead of EBUSY\n", strerror(errno));
    }

    /* Cleanup */
    close(efd);
    munmap(mmio, info.size);
    close(fd);
    printf("PASS: %s teardown clean\n", dev_node);
    return 0;
}

int main(int argc, char **argv)
{
    printf("=== Ambarella Virtual UART Host Driver Test Suite ===\n");
    int failed = 0;

    if (test_uart_node("/dev/amba_virt_uart2", 2, 0xffe0018000ULL) < 0) {
        failed++;
    }

    if (test_uart_node("/dev/amba_virt_uart3", 3, 0xffe0019000ULL) < 0) {
        failed++;
    }

    if (failed == 0) {
        printf("\n>>> ALL AMBA_VIRT_UART TESTS PASSED SUCCESSFULLY! <<<\n");
        return 0;
    } else {
        fprintf(stderr, "\n>>> %d TEST(S) FAILED <<<\n", failed);
        return 1;
    }
}
