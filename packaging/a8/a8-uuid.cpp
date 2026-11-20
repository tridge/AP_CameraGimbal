#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* Prints the SigmaStar chip UUID as the vendor "cardv Uuid" does, so the
 * vendor mac.sh derives the same Ethernet MAC address. mac.sh persists
 * whatever it gets, so retry rather than answer with nothing. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    /* libmi_sys resolves its OS wrapper symbols from a preloaded library */
    void *wrapper = dlopen("libcam_os_wrapper.so", RTLD_NOW | RTLD_GLOBAL);
    void *lib = wrapper != NULL ? dlopen("libmi_sys.so", RTLD_NOW | RTLD_GLOBAL)
                                : NULL;
    /* both take the SoC id first, as the vendor app calls them */
    int (*init)(unsigned short) = lib != NULL ? (decltype(init))dlsym(lib, "MI_SYS_Init") : NULL;
    int (*read_uuid)(unsigned short, uint64_t *) =
        lib != NULL ? (decltype(read_uuid))dlsym(lib, "MI_SYS_ReadUuid") : NULL;
    uint64_t uuid = 0;
    int result = -1;
    int saved_stdout;
    int null_fd;

    if (init == NULL || read_uuid == NULL) {
        const char *error = dlerror();
        fprintf(stderr, "libmi_sys.so: %s\n", error ? error : "missing symbol");
        return 1;
    }
    /* libmi_sys prints its own errors on stdout, which mac.sh would hash */
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    null_fd = open("/dev/null", O_WRONLY);
    if (saved_stdout < 0 || null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0) {
        return 1;
    }
    for (unsigned attempt = 0; attempt < 10U && result != 0; attempt++) {
        if (attempt != 0U) usleep(200000);
        result = init(0);
        if (result != 0) {
            fprintf(stderr, "MI_SYS_Init failed: 0x%x\n", result);
            continue;
        }
        result = read_uuid(0, &uuid);
        if (result != 0) fprintf(stderr, "MI_SYS_ReadUuid failed: 0x%x\n", result);
    }
    fflush(stdout);
    close(null_fd);
    if (dup2(saved_stdout, STDOUT_FILENO) < 0) return 1;
    close(saved_stdout);
    if (result != 0) return 1;
    printf("%llu", (unsigned long long)uuid);
    return 0;
}
