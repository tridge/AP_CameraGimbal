#pragma once
#include <pthread.h>
#include <unistd.h>
#include <cerrno>

// Small owners for resources used by long-lived control and media services.
// No exceptions or allocation; initialization failures remain explicit.
class APC_Mutex {
public:
    APC_Mutex() : _error(pthread_mutex_init(&_mutex, nullptr)) {}
    ~APC_Mutex() { if (!_error) pthread_mutex_destroy(&_mutex); }
    APC_Mutex(const APC_Mutex &) = delete;
    APC_Mutex &operator=(const APC_Mutex &) = delete;
    int error() const { return _error; }
    void lock() { pthread_mutex_lock(&_mutex); }
    void unlock() { pthread_mutex_unlock(&_mutex); }
    pthread_mutex_t *native() { return &_mutex; }
private:
    pthread_mutex_t _mutex {};
    int _error;
};

class APC_LockGuard {
public:
    explicit APC_LockGuard(APC_Mutex &mutex) : _mutex(mutex) { _mutex.lock(); }
    ~APC_LockGuard() { _mutex.unlock(); }
    APC_LockGuard(const APC_LockGuard &) = delete;
    APC_LockGuard &operator=(const APC_LockGuard &) = delete;
private:
    APC_Mutex &_mutex;
};

class APC_Condition {
public:
    APC_Condition() : _error(pthread_cond_init(&_condition, nullptr)) {}
    ~APC_Condition() { if (!_error) pthread_cond_destroy(&_condition); }
    APC_Condition(const APC_Condition &) = delete;
    APC_Condition &operator=(const APC_Condition &) = delete;
    int error() const { return _error; }
    void signal() { pthread_cond_signal(&_condition); }
    int wait_until(APC_Mutex &mutex, const timespec &deadline)
    { return pthread_cond_timedwait(&_condition, mutex.native(), &deadline); }
private:
    pthread_cond_t _condition {};
    int _error;
};

class APC_FileDescriptor {
public:
    explicit APC_FileDescriptor(int fd = -1) : _fd(fd) {}
    ~APC_FileDescriptor() { reset(); }
    APC_FileDescriptor(const APC_FileDescriptor &) = delete;
    APC_FileDescriptor &operator=(const APC_FileDescriptor &) = delete;
    int get() const { return _fd; }
    int release() { int fd = _fd; _fd = -1; return fd; }
    void reset(int fd = -1)
    {
        const int saved = errno;
        if (_fd >= 0) close(_fd);
        _fd = fd;
        errno = saved;
    }
private:
    int _fd;
};
