/*
 * tools/bringup/check_locks.c
 *
 * MAX96712 Deserializer & GMSL link diagnostic and continuous clock configuration utility.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

static int read_reg(int fd, unsigned char slv_addr, unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF) };
    struct i2c_msg msgs[2] = {
        { .addr = slv_addr, .flags = 0, .len = 2, .buf = buf },
        { .addr = slv_addr, .flags = I2C_M_RD, .len = 1, .buf = val }
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

static int write_reg(int fd, unsigned char slv_addr, unsigned short reg, unsigned char val) {
    unsigned char buf[3] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF), val };
    struct i2c_msg msg = { .addr = slv_addr, .flags = 0, .len = 3, .buf = buf };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

int main(int argc, char **argv) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open /dev/i2c-1"); return 1; }

    if (argc > 1 && strcmp(argv[1], "--set-cont-clk") == 0) {
        printf("Setting MAX96712 Reg 0x08A0 = 0x84 (Continuous MIPI Clock Mode)...\n");
        if (write_reg(fd, 0x29, 0x08A0, 0x84) < 0) perror("write 0x08A0");
        close(fd);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--set-rate-1000") == 0) {
        printf("Setting MAX96712 MIPI data rate to 1000 Mbps/lane...\n");
        write_reg(fd, 0x29, 0x1D00, 0xF4);
        write_reg(fd, 0x29, 0x1E00, 0xF4);
        write_reg(fd, 0x29, 0x0415, 0x2A);
        write_reg(fd, 0x29, 0x0418, 0x2A);
        write_reg(fd, 0x29, 0x1D00, 0xF5);
        write_reg(fd, 0x29, 0x1E00, 0xF5);
        close(fd);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--set-rate-800") == 0) {
        printf("Setting MAX96712 MIPI data rate to 800 Mbps/lane...\n");
        write_reg(fd, 0x29, 0x1D00, 0xF4);
        write_reg(fd, 0x29, 0x1E00, 0xF4);
        write_reg(fd, 0x29, 0x0415, 0x28);
        write_reg(fd, 0x29, 0x0418, 0x28);
        write_reg(fd, 0x29, 0x1D00, 0xF5);
        write_reg(fd, 0x29, 0x1E00, 0xF5);
        close(fd);
        return 0;
    }

    printf("=== MAX96712 (0x29) Registers ===\n");
    unsigned short check_regs[] = {
        0x0006, 0x000A, 0x000B, 0x000C, 0x0013, 0x001A, 0x001B, 0x001C, 0x001D,
        0x0020, 0x0021, 0x0022, 0x0023, 0x0028, 0x0029, 0x002A, 0x002B,
        0x0400, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x040A, 0x040B,
        0x0415, 0x0418, 0x041B, 0x041E,
        0x08A0, 0x08A2, 0x08A3, 0x08A4,
        0x090A, 0x090B, 0x090D, 0x090E, 0x092D,
        0x094A, 0x094B, 0x096D,
        0x098A, 0x098B, 0x09AD,
        0x09CA, 0x09CB, 0x09ED,
        0x0971, 0x0973, 0x09B1, 0x09B3,
        0x1D00, 0x1E00
    };
    for (size_t i = 0; i < sizeof(check_regs)/sizeof(check_regs[0]); i++) {
        unsigned char val = 0;
        if (read_reg(fd, 0x29, check_regs[i], &val) >= 0) {
            printf("  Reg 0x%04X = 0x%02X\n", check_regs[i], val);
        } else {
            printf("  Reg 0x%04X: read failed\n", check_regs[i]);
        }
    }

    printf("\n=== MAX9295A Serializer (0x42) Registers ===\n");
    unsigned short ser_regs[] = { 0x0000, 0x0001, 0x0002, 0x000D, 0x0010, 0x0011, 0x0042, 0x0043, 0x0308, 0x0311, 0x0314, 0x0318 };
    for (size_t i = 0; i < sizeof(ser_regs)/sizeof(ser_regs[0]); i++) {
        unsigned char val = 0;
        if (read_reg(fd, 0x42, ser_regs[i], &val) >= 0) {
            printf("  Ser Reg 0x%04X = 0x%02X\n", ser_regs[i], val);
        } else {
            printf("  Ser Reg 0x%04X: read failed\n", ser_regs[i]);
        }
    }

    printf("\n=== OS08A10 Sensor (0x36) Registers ===\n");
    unsigned short sen_regs[] = { 0x300A, 0x300B, 0x300C, 0x0100, 0x3800, 0x3801, 0x3802, 0x3803 };
    for (size_t i = 0; i < sizeof(sen_regs)/sizeof(sen_regs[0]); i++) {
        unsigned char val = 0;
        if (read_reg(fd, 0x36, sen_regs[i], &val) >= 0) {
            printf("  Sen Reg 0x%04X = 0x%02X\n", sen_regs[i], val);
        } else {
            printf("  Sen Reg 0x%04X: read failed\n", sen_regs[i]);
        }
    }

    close(fd);
    return 0;
}
