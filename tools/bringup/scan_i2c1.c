/*
 * tools/bringup/scan_i2c1.c
 *
 * I2C address scanner utility for probing connected sensors and serializers.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/i2c-1";
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open I2C device");
        return 1;
    }
    
    printf("Scanning %s with 0-byte write (I2C_SMBUS_QUICK):\n", dev);
    for (int a = 0x03; a <= 0x77; a++) {
        if (ioctl(fd, I2C_SLAVE_FORCE, a) < 0) continue;
        union i2c_smbus_data data;
        struct i2c_smbus_ioctl_data args = {
            .read_write = I2C_SMBUS_WRITE,
            .command = 0,
            .size = I2C_SMBUS_QUICK,
            .data = &data
        };
        if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
            printf("  ACK (write) at 7-bit 0x%02X (8-bit 0x%02X)\n", a, a << 1);
        }
    }
    
    printf("\nScanning %s with 1-byte write (write 0x00):\n", dev);
    for (int a = 0x03; a <= 0x77; a++) {
        unsigned char buf[1] = {0x00};
        struct i2c_msg msg = { .addr = (unsigned short)a, .flags = 0, .len = 1, .buf = buf };
        struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };
        if (ioctl(fd, I2C_RDWR, &rdwr) >= 0) {
            printf("  ACK (1-byte write) at 7-bit 0x%02X (8-bit 0x%02X)\n", a, a << 1);
        }
    }
    
    close(fd);
    return 0;
}
