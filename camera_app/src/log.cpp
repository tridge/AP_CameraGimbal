#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void ca_log(const char *format, ...)
{
    struct timespec now;
    struct tm local;
    char timestamp[32];
    va_list arguments;

    (void)clock_gettime(CLOCK_REALTIME, &now);
    (void)localtime_r(&now.tv_sec, &local);
    (void)strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(stderr, "[%s.%03ld] camera-app: ", timestamp, now.tv_nsec / 1000000L);
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}
