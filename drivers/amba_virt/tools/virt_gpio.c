/*
 * virt_gpio.c
 *
 * Drives a set of GPIO lines through the GPIO character device (uAPI v2).
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "virt_gpio.h"

static unsigned long long all_lines_mask(size_t count)
{
    return count >= 64 ? ~0ULL : ((1ULL << count) - 1ULL);
}

int virt_gpio_build_request(struct gpio_v2_line_request *req, const unsigned int *lines,
                            size_t count, int initial_value)
{
    unsigned long long mask;

    if (!req || !lines || count == 0 || count > GPIO_V2_LINES_MAX)
        return -EINVAL;

    memset(req, 0, sizeof(*req));
    mask = all_lines_mask(count);
    for (size_t i = 0; i < count; i++)
        req->offsets[i] = lines[i];
    req->num_lines = (__u32)count;
    snprintf(req->consumer, sizeof(req->consumer), "amba-virt-server");
    req->config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req->config.num_attrs = 1;
    req->config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req->config.attrs[0].attr.values = initial_value ? mask : 0ULL;
    req->config.attrs[0].mask = mask;
    return 0;
}

int virt_gpio_acquire(struct virt_gpio *gpio, const char *chip_path, const unsigned int *lines,
                      size_t count, int initial_value)
{
    struct gpio_v2_line_request req;
    int chip_fd;
    int rc;

    if (!gpio || !chip_path)
        return -EINVAL;
    if (gpio->line_fd >= 0)
        return -EBUSY;

    rc = virt_gpio_build_request(&req, lines, count, initial_value);
    if (rc < 0)
        return rc;

    chip_fd = open(chip_path, O_RDWR | O_CLOEXEC);
    if (chip_fd < 0)
        return -errno;

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        rc = -errno;
        close(chip_fd);
        return rc;
    }
    /* The line request descriptor stays valid after the chip descriptor closes. */
    close(chip_fd);

    gpio->line_fd = req.fd;
    gpio->count = (unsigned int)count;
    return 0;
}

int virt_gpio_set(struct virt_gpio *gpio, int value)
{
    struct gpio_v2_line_values vals;

    if (!gpio || gpio->line_fd < 0 || gpio->count == 0)
        return -EINVAL;

    memset(&vals, 0, sizeof(vals));
    vals.mask = all_lines_mask(gpio->count);
    vals.bits = value ? vals.mask : 0ULL;
    if (ioctl(gpio->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0)
        return -errno;
    return 0;
}

void virt_gpio_release(struct virt_gpio *gpio)
{
    if (!gpio)
        return;
    if (gpio->line_fd >= 0)
        close(gpio->line_fd);
    gpio->line_fd = -1;
    gpio->count = 0;
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
