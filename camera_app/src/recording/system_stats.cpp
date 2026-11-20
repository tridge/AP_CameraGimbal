#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/system_stats.h"
#include "apcam/target.h"
#include "apcam/soc_temperature.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>

static bool parse_cpu(const char *line, uint64_t *total, uint64_t *idle)
{
    unsigned long long user=0, nice=0, system=0, wait=0, irq=0, softirq=0, steal=0, rest=0;
    if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &rest, &wait, &irq, &softirq, &steal) < 4) return false;
    /* guest/guest_nice are already counted in user/nice. I/O wait isn't busy. */
    *idle = rest + wait;
    *total = user + nice + system + rest + wait + irq + softirq + steal;
    return true;
}

static bool read_cpu(uint64_t *total, uint64_t *idle)
{
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    char line[512];
    bool ok = fgets(line, sizeof(line), file) && parse_cpu(line, total, idle);
    fclose(file);
    return ok;
}

static float cpu_load(struct ca_system_stats *state, uint64_t total, uint64_t idle)
{
    float result = NAN;
    if (state->have_cpu && total > state->total && idle >= state->idle &&
        idle - state->idle <= total - state->total) {
        result = 100.0 * (1.0 - (double)(idle - state->idle) / (total - state->total));
    }
    state->total = total;
    state->idle = idle;
    state->have_cpu = true;
    return result;
}

static void read_memory(FILE *file, struct ca_log_sys *sample)
{
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        unsigned long long kib;
        if (sscanf(line, "MemFree: %llu kB", &kib) == 1 && kib <= UINT64_MAX / 1024) {
            sample->mem_free = (uint64_t)kib * 1024;
            sample->valid |= CA_SYS_MEM_FREE;
        } else if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1 && kib <= UINT64_MAX / 1024) {
            sample->mem_available = (uint64_t)kib * 1024;
            sample->valid |= CA_SYS_MEM_AVAILABLE;
        }
    }
}

#if !defined(CAMERA_APP_SITL) && !defined(CAMERA_APP_HOST) && APCAM_TARGET == APCAM_TARGET_MT11
static float mt11_temperature(const volatile uint32_t *registers)
{
    double total = 0;
    /* Same SS928V100 on-die channels and calibration as the web Status page. */
    for (unsigned channel = 0; channel < 3; channel++) {
        uint32_t raw = registers[(0x8 + channel * 0x100) / 4] & 0x3ff;
        if (raw < 100 || raw > 900) return NAN;
        total += ((double)raw - 146) * 165 / 718 - 40;
    }
    return total / 3;
}
#endif

static float cpu_temperature(void)
{
#if !defined(CAMERA_APP_SITL) && !defined(CAMERA_APP_HOST) && APCAM_TARGET == APCAM_TARGET_MT11
    int fd = open("/dev/mem", O_RDONLY | O_CLOEXEC | O_SYNC);
    if (fd < 0) return NAN;
    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x1102e000);
    close(fd);
    if (map == MAP_FAILED) return NAN;
    float value = mt11_temperature((const volatile uint32_t*)(map));
    munmap(map, 4096);
    return value;
#elif !defined(CAMERA_APP_SITL) && !defined(CAMERA_APP_HOST) && defined(SOC_TEMPERATURE_PATH)
    int value=0;
#ifdef SOC_TEMPERATURE_LABELLED_CELSIUS
    constexpr bool labelled=true;
#else
    constexpr bool labelled=false;
#endif
    return apcam_read_soc_temperature(SOC_TEMPERATURE_PATH,labelled,&value) ? value / 1000.0f : NAN;
#else
    /* Other cameras have no supported reader. SITL must not access camera
     * registers or mislabel a host motherboard sensor as CPU temperature. */
    return NAN;
#endif
}

void ca_system_stats_sample(struct ca_system_stats *state, int log_fd,
                            struct ca_log_sys *sample)
{
    *sample = (struct ca_log_sys){.time_us=ca_binlog_time_us(),
                                .cpu_temp=cpu_temperature(), .cpu_load=NAN};
    if (isfinite(sample->cpu_temp)) sample->valid |= CA_SYS_TEMP;
    uint64_t total, idle;
    if (read_cpu(&total, &idle)) {
        sample->cpu_load = cpu_load(state, total, idle);
        if (isfinite(sample->cpu_load)) sample->valid |= CA_SYS_CPU;
    } else state->have_cpu = false;
    FILE *file = fopen("/proc/meminfo", "r");
    if (file) { read_memory(file, sample); fclose(file); }
    struct statvfs fs;
    if (fstatvfs(log_fd, &fs) == 0 && fs.f_frsize &&
        (uint64_t)fs.f_bavail <= UINT64_MAX / fs.f_frsize) {
        sample->sd_free = (uint64_t)fs.f_bavail * fs.f_frsize;
        sample->valid |= CA_SYS_SD_FREE;
    }
}
