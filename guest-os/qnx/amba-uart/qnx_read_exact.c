/*
 * qnx_read_exact.c
 *
 * Exact N-byte reader from serial port for QNX.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <dev> <out_file> <bytes>\n", argv[0]);
        return 1;
    }
    const char *dev = argv[1];
    const char *out_path = argv[2];
    size_t target_len = (size_t)strtoul(argv[3], NULL, 0);

    int in_fd = open(dev, O_RDONLY | O_NOCTTY);
    if (in_fd < 0) {
        perror("open input");
        return 1;
    }

    struct termios tio;
    if (tcgetattr(in_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(in_fd, TCSANOW, &tio);
    }
    tcflush(in_fd, TCIFLUSH);

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open output");
        close(in_fd);
        return 1;
    }

    size_t total = 0;
    char buf[4096];
    while (total < target_len) {
        size_t to_read = target_len - total;
        if (to_read > sizeof(buf))
            to_read = sizeof(buf);
        ssize_t n = read(in_fd, buf, to_read);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            break;
        }
        write(out_fd, buf, n);
        total += n;
    }

    close(in_fd);
    close(out_fd);
    return (total == target_len) ? 0 : 2;
}
