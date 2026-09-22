/*
 * tools/bringup/read_max_regs.c
 *
 * MAX96712 Deserializer register dumping utility over I2C.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

static int read_reg(int fd, unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF) };
    struct i2c_msg msgs[2] = {
        { .addr = 0x29, .flags = 0, .len = 2, .buf = buf },
        { .addr = 0x29, .flags = I2C_M_RD, .len = 1, .buf = val }
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

int main(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open /dev/i2c-1"); return 1; }
    unsigned char val = 0;
    
    unsigned short regs[] = { 0x0000, 0x0001, 0x0006, 0x000A, 0x000B, 0x000C, 0x000D, 0x001A, 0x0013, 0x0014 };
    printf("=== MAX96712 (0x29) Primary Registers ===\n");
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        if (read_reg(fd, regs[i], &val) >= 0) {
            printf("  Reg 0x%04X = 0x%02X\n", regs[i], val);
        } else {
            printf("  Reg 0x%04X = READ FAILED\n", regs[i]);
        }
    }
    close(fd);
    return 0;
}
