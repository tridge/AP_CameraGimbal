#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* The retained mac.sh hashes the signed decimal chip UUID, without a newline.
 * Infinity6B0 MI_SYS functions do not take the SoC argument used on A8. */
#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    void *wrapper = dlopen("libcam_os_wrapper.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib = wrapper ? dlopen("libmi_sys.so", RTLD_NOW | RTLD_GLOBAL) : NULL;
    int (*init)(void) = lib ? (decltype(init))dlsym(lib, "MI_SYS_Init") : NULL;
    int (*read_uuid)(uint64_t *) = lib ? (decltype(read_uuid))dlsym(lib, "MI_SYS_ReadUuid") : NULL;
    if (!init || !read_uuid) {
        const char *error = dlerror();
        fprintf(stderr, "Cannot load chip UUID API: %s\n", error ? error : "missing symbol");
        return 1;
    }
    /* SDK diagnostics must never become part of the MAC-address input. */
    int saved = dup(STDOUT_FILENO);
    int null_fd = open("/dev/null", O_WRONLY);
    if (saved < 0 || null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) return 1;
    close(null_fd);
    uint64_t uuid = 0;
    int result = -1;
    for (unsigned attempt = 0; attempt < 10 && result != 0; attempt++) {
        if (attempt) usleep(200000);
        result = init();
        if (result == 0) result = read_uuid(&uuid);
    }
    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) return 1;
    close(saved);
    if (result != 0) {
        fprintf(stderr, "Cannot read chip UUID: 0x%x\n", result);
        return 1;
    }
    printf("%" PRId64, (int64_t)uuid);
    return 0;
}
