#ifndef CAMERA_APP_SYSTEM_STATS_H
#define CAMERA_APP_SYSTEM_STATS_H
#include "camera_app/binlog.h"
struct ca_system_stats {
    uint64_t total, idle;
    bool have_cpu;
};
/* Only call from the log writer: procfs, sensor and filesystem reads may block.
 * The first call establishes a CPU baseline; later calls report interval use. */
void ca_system_stats_sample(struct ca_system_stats *state, int log_fd,
                            struct ca_log_sys *sample);
#endif
