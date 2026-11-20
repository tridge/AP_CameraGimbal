/* RAM-only diagnostic: observe successful libc UART reads/writes, never inject.
 * No MI/ISP calls. Bounds apply to this log alone, not vendor resource limits. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#ifndef ZT30_TRACE_UART
#define ZT30_TRACE_UART "/dev/ttyS1"
#endif
#ifndef ZT30_TRACE_PROGRAM
#define ZT30_TRACE_PROGRAM "/customer/siyi_app_359"
#endif
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#include "zt30_trace_output.h"
static struct zt30_trace_output trace_output={.fd=-1};
#define TRACE_LIMIT (2U * 1024U * 1024U)
struct record { uint64_t ns; uint32_t tid; int32_t fd; uint32_t length; uint8_t direction; uint8_t padding[3]; };
_Static_assert(sizeof(struct record) == 24, "trace record ABI");
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int trace_fd = -1;
static dev_t uart_device;
static pid_t owner;
static uint32_t used;
static uint32_t disabled;
_Static_assert(__atomic_always_lock_free(4, 0), "lock-free record reservation");

/* Raw pwrite64 is neither interposed nor a pthread cancellation point. ARM EABI
 * aligns its 64-bit offset to an even register pair; syscall does not do that. */
static ssize_t write_at(const void *data, size_t length, uint32_t offset)
{
    if (!zt30_trace_output_matches(&trace_output)) return -1;
#ifdef __arm__
    return syscall(SYS_pwrite64, trace_fd, data, length, 0, offset, 0);
#else
    return syscall(SYS_pwrite64, trace_fd, data, length, (long long)offset);
#endif
}
static void emit(int fd, char direction, const void *data, size_t length, uint64_t ns)
{
    uint8_t buffer[24 + 4096];
    struct record rec = { .ns = ns, .tid = (uint32_t)syscall(SYS_gettid), .fd = fd };
    size_t remaining = length;
    /* Lock-free slots also work if a vendor signal handler writes to the UART. */
    do {
        uint32_t offset, next;
        size_t part = remaining > 4096 ? 4096 : remaining;
        ssize_t n;
        if (__atomic_load_n(&disabled, __ATOMIC_RELAXED)) return;
        offset = __atomic_load_n(&used, __ATOMIC_RELAXED);
        for (;;) {
            if (offset + sizeof(rec) + part > TRACE_LIMIT - sizeof(rec)) {
                direction = 'L'; part = 0;
            }
            next = offset + sizeof(rec) + (uint32_t)part;
            if (next > TRACE_LIMIT) return;
            if (__atomic_compare_exchange_n(&used, &offset, next, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
        }
        rec.length = (uint32_t)part; rec.direction = (uint8_t)direction;
        memcpy(buffer, &rec, sizeof(rec));
        if (part) memcpy(buffer + sizeof(rec), data, part);
        do { n = write_at(buffer, sizeof(rec) + part, offset); } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)(sizeof(rec) + part)) {
            __atomic_store_n(&disabled, 1, __ATOMIC_RELAXED);
            /* Invalidate the magic even when a short write left a plausible tail. */
            write_at("!", 1, 0);
            return;
        }
        if (direction == 'L') { __atomic_store_n(&disabled, 1, __ATOMIC_RELAXED); return; }
        if (part) data = (const uint8_t *)data + part;
        remaining -= part;
    } while (remaining);
}

static void observe(int fd, char direction, const void *data, ssize_t length)
{
    int saved = errno;
    struct stat st;
    struct timespec now;
    /* fstat avoids mistaking a reused descriptor for the UART. No ptrace stops. */
    if (owner && owner == getpid() && length > 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
        st.st_rdev == uart_device && clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        emit(fd, direction, data, (size_t)length, (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec);
    errno = saved;
}

ssize_t read(int fd, void *data, size_t size)
{
    ssize_t n = real_read ? real_read(fd, data, size) : syscall(SYS_read, fd, data, size);
    observe(fd, 'R', data, n);
    return n;
}
ssize_t write(int fd, const void *data, size_t size)
{
    ssize_t n = real_write ? real_write(fd, data, size) : syscall(SYS_write, fd, data, size);
    observe(fd, 'W', data, n);
    return n;
}

__attribute__((constructor)) static void start_trace(void)
{
    const char *path = getenv("APCG_UART_TRACE");
    const char *prefix = "/tmp/apcg-zt30.session.";
    char parent[PATH_MAX], resolved[PATH_MAX], program[PATH_MAX];
    ssize_t program_size;
    struct stat st;
    struct statfs fs;
    size_t base = strlen(prefix), i;
    int saved = errno;
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    program_size = readlink("/proc/self/exe", program, sizeof(program) - 1);
    if (program_size < 0) goto done;
    program[program_size] = 0;
    /* env -i and vendor-exec also load this library; only the fixed vendor logs. */
    if (strcmp(program, ZT30_TRACE_PROGRAM) != 0) goto done;
    if (!path || strncmp(path, prefix, base) != 0 || strlen(path) != base + 6 + strlen("/uart.bin")) goto done;
    for (i = base; i < base + 6; ++i)
        if (!((path[i] >= 'a' && path[i] <= 'z') || (path[i] >= 'A' && path[i] <= 'Z') || (path[i] >= '0' && path[i] <= '9'))) goto done;
    if (strcmp(path + base + 6, "/uart.bin") != 0) goto done;
    memcpy(parent, path, base + 6); parent[base + 6] = 0;
    if (!realpath(parent, resolved) || strcmp(parent, resolved) != 0 || statfs(parent, &fs) != 0) goto done;
#ifndef ZT30_TRACE_HOST_TEST
    if ((unsigned long)fs.f_type != 0x01021994UL) goto done; /* tmpfs only */
#endif
    if (stat(ZT30_TRACE_UART, &st) != 0 || !S_ISCHR(st.st_mode)) goto done;
    uart_device = st.st_rdev;
    trace_fd = zt30_trace_output_open(&trace_output, path);
    if (trace_fd < 0) goto done;
    if (syscall(SYS_write, trace_fd, "ZT30URT1", 8) != 8) {
        syscall(SYS_close, trace_fd); trace_fd = -1; goto done;
    }
    used = 8; owner = getpid();
done:
    errno = saved;
}
__attribute__((destructor)) static void end_trace(void)
{
    int saved = errno;
    if (owner == getpid()) emit(-1, 'E', NULL, 0, 0);
    /* Process exit closes this fd; avoid closing beneath a concurrent observer. */
    errno = saved;
}
