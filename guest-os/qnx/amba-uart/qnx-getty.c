/*
 * qnx-getty.c
 *
 * Dedicated getty / login supervisor for Ambarella QNX 8.0 HVM serial console.
 * Spawns login -f root on /dev/ser3 with full session leadership (setsid),
 * controlling terminal (/dev/tty), and proper termios line discipline.
 * Automatically respawns upon session termination.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_DEV "/dev/ser3"

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : DEFAULT_DEV;

	while (1) {
		/* Wait for serial device to appear */
		while (access(dev, F_OK) != 0) {
			sleep(1);
		}

		pid_t pid = fork();
		if (pid == 0) {
			/* Child: become session leader and acquire controlling TTY */
			setsid();

			int fd = open(dev, O_RDWR);
			if (fd < 0) {
				_exit(1);
			}

			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO) {
				close(fd);
			}

			/* Acquire controlling terminal for this session */
			tcsetsid(STDIN_FILENO, getpid());
			tcsetpgrp(STDIN_FILENO, getpid());

			/* Configure standard interactive termios */
			struct termios tio;
			if (tcgetattr(STDIN_FILENO, &tio) == 0) {
				tio.c_cflag &= ~(IHFLOW | OHFLOW | CSIZE | PARENB | PARODD | CSTOPB);
				tio.c_cflag |= (CS8 | CREAD | CLOCAL);
				tio.c_iflag |= ICRNL;
				tio.c_iflag &= ~(IXON | IXOFF);
				tio.c_oflag |= (OPOST | ONLCR);
				tio.c_lflag |= (ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN);
				tcsetattr(STDIN_FILENO, TCSANOW, &tio);
			}

			/* Set environment */
			setenv("TERM", "qansi", 1);
			setenv("HOME", "/root", 1);
			setenv("USER", "root", 1);
			setenv("SHELL", "/proc/boot/sh", 1);

			/* Print welcome banner to console */
			dprintf(STDOUT_FILENO, "\r\nQNX Neutrino RTOS 8.0 (Ambarella CV3-AD655)\r\nTerminal: %s\r\n\r\n", dev);

			/* Launch interactive root login shell directly */
			execl("/proc/boot/sh", "sh", "-l", (char *)NULL);
			execl("/system/bin/login", "login", "-f", "root", (char *)NULL);
			_exit(1);
		} else if (pid > 0) {
			/* Parent supervisor: wait for session to exit */
			int status = 0;
			waitpid(pid, &status, 0);
			sleep(1);
		} else {
			sleep(1);
		}
	}

	return 0;
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
