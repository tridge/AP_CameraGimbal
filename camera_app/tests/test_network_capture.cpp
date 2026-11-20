#include "camera_app/APC_NetworkCapture.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ca_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

// A line-oriented controller for the real capture worker. Test-only file sizes
// make rotation practical without creating megabytes of network traffic in CI.
int main(int argc, char **argv)
{
    assert(argc == 3);
    APC_NetworkCapture capture;
    assert(capture.init(argv[1], argv[2]) == 0);
    char line[32];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcmp(line, "on\n")) capture.configure(true);
        else if (!strcmp(line, "off\n")) capture.configure(false);
        else if (!strcmp(line, "quit\n")) break;
        else assert(false);
    }
    return 0;
}
