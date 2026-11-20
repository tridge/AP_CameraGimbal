#pragma once

#include <atomic>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>

// A dedicated worker owns packet reception and SD I/O. The control loop only
// changes the requested state; packet bursts and slow storage cannot block it.
class APC_NetworkCapture {
public:
    APC_NetworkCapture() = default;
    ~APC_NetworkCapture() { close(); pthread_cond_destroy(&_wake); pthread_mutex_destroy(&_mutex); }
    APC_NetworkCapture(const APC_NetworkCapture &) = delete;
    APC_NetworkCapture &operator=(const APC_NetworkCapture &) = delete;
    int init(const char *record_root, const char *ready_path);
    void configure(bool enabled);
    void close();

private:
    static void *worker(void *opaque);
    void run();
    int capture(uint64_t generation);
    void status(bool error, const char *message);
    bool changed(uint64_t generation) const;
    pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t _wake = PTHREAD_COND_INITIALIZER;
    pthread_t _thread {};
    bool _started = false;
    bool _enabled = false; // guarded by _mutex
    std::atomic<bool> _quit {false};
    std::atomic<uint64_t> _generation {0};
    char _root[PATH_MAX] {};
    char _status_path[PATH_MAX] {};
};
