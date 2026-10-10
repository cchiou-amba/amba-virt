/*
 * virt_gpio.h
 *
 * Drives a set of GPIO lines through the GPIO character device (uAPI v2).
 *
 * The line numbers come from the caller. A v2 output is only driven while its
 * line request descriptor is open, so the descriptor lives in struct virt_gpio
 * until virt_gpio_release().
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _VIRT_GPIO_H_
#define _VIRT_GPIO_H_

#include <stddef.h>
#include <linux/gpio.h>

struct virt_gpio {
    int line_fd;
    unsigned int count;
};

#define VIRT_GPIO_INIT { -1, 0 }

/*
 * Fills req for count output lines, all at initial_value (0 or 1).
 * Returns 0, or -EINVAL if count is 0 or above GPIO_V2_LINES_MAX.
 * No ioctl is issued.
 */
int virt_gpio_build_request(struct gpio_v2_line_request *req, const unsigned int *lines,
                            size_t count, int initial_value);

/* Opens chip_path, requests the lines as outputs at initial_value. Returns 0 or -errno. */
int virt_gpio_acquire(struct virt_gpio *gpio, const char *chip_path, const unsigned int *lines,
                      size_t count, int initial_value);

/* Sets every acquired line to value (0 or 1). Returns 0 or -errno. */
int virt_gpio_set(struct virt_gpio *gpio, int value);

/* Releases the lines. Safe to call on a struct that holds nothing. */
void virt_gpio_release(struct virt_gpio *gpio);

#endif /* _VIRT_GPIO_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
