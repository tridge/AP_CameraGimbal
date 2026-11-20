#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/external_uart.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

int ca_external_uart_open(const char *device)
{
    struct termios settings;
    int fd;

    if (device == NULL || device[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (tcgetattr(fd, &settings) < 0) goto fail;
    cfmakeraw(&settings);
    settings.c_cflag &= (tcflag_t)~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (cfsetispeed(&settings, B230400) < 0 ||
        cfsetospeed(&settings, B230400) < 0 ||
        tcsetattr(fd, TCSANOW, &settings) < 0 || tcflush(fd, TCIOFLUSH) < 0) {
        goto fail;
    }
    return fd;

fail:
    {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
}
