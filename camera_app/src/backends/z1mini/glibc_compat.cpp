/* The GNU 10.2 sysroot defaults to fcntl@GLIBC_2.28, while the camera has
 * glibc 2.25. The RTSP socket layer only uses descriptor/flag operations, whose
 * ABI has not changed. Bind these explicitly to the original ARM symbol.
 * Refuse file-lock commands rather than pass incompatible struct flock layouts.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

/* dlib uses double-precision exp/pow. The toolchain's libm defaults to
 * GLIBC_2.29 implementations; the original ARM symbols have the same calling
 * convention and are available on the camera's glibc 2.25.
 */
extern "C" double z1_old_exp(double);
extern "C" double z1_old_pow(double, double);
__asm__(".symver z1_old_exp,exp@GLIBC_2.4");
__asm__(".symver z1_old_pow,pow@GLIBC_2.4");
extern "C" double __wrap_exp(double value)
{
    return z1_old_exp(value);
}
extern "C" double __wrap_pow(double base, double exponent)
{
    return z1_old_pow(base, exponent);
}

extern "C" int z1_old_fcntl(int, int, ...);
__asm__(".symver z1_old_fcntl,fcntl@GLIBC_2.4");
extern "C" int __wrap_fcntl(int fd, int command, ...)
{
    switch (command) {
    case F_GETFL: case F_GETFD: case F_GETOWN:
        return z1_old_fcntl(fd, command);
    case F_SETFL: case F_SETFD: case F_SETOWN: case F_DUPFD: case F_DUPFD_CLOEXEC: {
        va_list ap; va_start(ap, command);
        int arg = va_arg(ap, int); va_end(ap);
        return z1_old_fcntl(fd, command, arg);
    }
    default:
        errno = ENOTSUP; return -1;
    }
}
