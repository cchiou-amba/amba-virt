/*
 * tools/bringup/test_oneshot_ser.c
 *
 * MAX96712 Deserializer one-shot reset and remote serializer I2C probe utility.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

static int write_des_reg(int fd, unsigned short reg, unsigned char val) {
    unsigned char buf[3] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF), val };
    struct i2c_msg msg = { .addr = 0x29, .flags = 0, .len = 3, .buf = buf };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

static int read_des_reg(int fd, unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF) };
    struct i2c_msg msgs[2] = {
        { .addr = 0x29, .flags = 0, .len = 2, .buf = buf },
        { .addr = 0x29, .flags = I2C_M_RD, .len = 1, .buf = val }
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

static int read_ser_reg(int fd, unsigned char addr, unsigned short reg, unsigned char *val) {
    unsigned char buf[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xFF) };
    struct i2c_msg msgs[2] = {
        { .addr = addr, .flags = 0, .len = 2, .buf = buf },
        { .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val }
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };
    return ioctl(fd, I2C_RDWR, &rdwr);
}

int main(void) {
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open /dev/i2c-1"); return 1; }
    
    printf("1. Setting 0x0006 = 0x02 (Link B only, GMSL2 mode)...\n");
    write_des_reg(fd, 0x0006, 0x02);
    
    printf("2. Triggering Link One-Shot Reset (0x0018 = 0x0F)...\n");
    write_des_reg(fd, 0x0018, 0x0F);
    
    printf("3. Waiting 100ms for link lock...\n");
    usleep(100000);
    
    unsigned char lock_status = 0;
    read_des_reg(fd, 0x001B, &lock_status);
    printf("Link B status (0x001B) = 0x%02X (Locked: %d)\n", lock_status, (lock_status >> 3) & 1);
    
    printf("4. Testing I2C read to serializer candidates...\n");
    unsigned char candidate_addrs[] = { 0x40, 0x4A, 0x4B, 0x62, 0x68, 0x6C, 0x34, 0x36 };
    for (size_t i = 0; i < sizeof(candidate_addrs); i++) {
        unsigned char a = candidate_addrs[i];
        unsigned char dev_id = 0;
        int ret0 = read_ser_reg(fd, a, 0x0000, &dev_id);
        int retD = read_ser_reg(fd, a, 0x000D, &dev_id);
        if (ret0 >= 0 || retD >= 0) {
            printf("  >>> SUCCESS! Responded at 7-bit 0x%02X (8-bit 0x%02X): reg0/regD = 0x%02X <<<\n",
                a, a << 1, dev_id);
        } else {
            printf("  No response at 7-bit 0x%02X (8-bit 0x%02X)\n", a, a << 1);
        }
    }
    
    close(fd);
    return 0;
}
