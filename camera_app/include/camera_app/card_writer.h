#pragma once

/* Background writes to the card. A slow card must never block the thread that
 * produces the data, so appends are queued and one worker per file writes
 * them out in large chunks and syncs every few seconds. */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <new>
#include <vector>

class ca_card_writer {
public:
    static constexpr size_t CHUNK_BYTES = 1U << 20;
    static constexpr size_t QUEUE_MAX_BYTES = 64U << 20;
    static constexpr size_t KEEP_BYTES = 4U << 20;   // buffer kept between writes
    static constexpr uint64_t WRITE_INTERVAL_US = 1000000;
    static constexpr uint64_t SYNC_INTERVAL_US = 5000000;
    static constexpr uint64_t STALL_US = 5000000;

    /* fd stays the caller's and must outlive close(). directory_fd, if not -1,
     * is synced with the first data and closed here. offset is the file size. */
    static ca_card_writer *open(int fd, uint64_t offset, int directory_fd = -1)
    {
        ca_card_writer *w = new (std::nothrow) ca_card_writer;
        if (w == nullptr) { errno = ENOMEM; return nullptr; }
        w->fd = fd;
        w->directory_fd = directory_fd;
        w->written = w->queued_end = offset;
        pthread_condattr_t attr;
        int result = pthread_mutex_init(&w->lock, nullptr);
        if (result != 0) { delete w; errno = result; return nullptr; }
        result = pthread_condattr_init(&attr);
        if (result == 0) {
#ifndef __APPLE__
            result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
            if (result == 0) result = pthread_cond_init(&w->wake, &attr);
            if (result == 0) {
                result = pthread_cond_init(&w->progress, &attr);
                if (result != 0) pthread_cond_destroy(&w->wake);
            }
            pthread_condattr_destroy(&attr);
        }
        if (result == 0) {
            result = pthread_create(&w->thread, nullptr, run, w);
            if (result != 0) {
                pthread_cond_destroy(&w->wake);
                pthread_cond_destroy(&w->progress);
            }
        }
        if (result != 0) {
            pthread_mutex_destroy(&w->lock);
            delete w;
            errno = result;
            return nullptr;
        }
        return w;
    }

    /* Queue bytes. The worker leaves them alone until commit(), so a reader
     * of the file only ever sees whole committed units. Fails with ENOMEM
     * rather than ending the process if the queue cannot grow. */
    int append(const void *data, size_t size)
    {
        pthread_mutex_lock(&lock);
        int result = failure;
        if (result == 0 && !queue.add(data, size)) result = ENOMEM;
        if (result == 0) queued_end += size;
        pthread_mutex_unlock(&lock);
        if (result != 0) { errno = result; return -1; }
        return 0;
    }

    void commit()
    {
        pthread_mutex_lock(&lock);
        commit_locked();
        pthread_mutex_unlock(&lock);
    }

    /* The card is not keeping up; the caller should drop data, not queue it. */
    bool stalled()
    {
        pthread_mutex_lock(&lock);
        // only committed data counts: bytes awaiting commit() are the caller's
        const bool result = queue.size > QUEUE_MAX_BYTES ||
            (committed != 0 && now_us() - committed_us > stall_us);
        pthread_mutex_unlock(&lock);
        return result;
    }

    void set_stall_us(uint64_t us)
    {
        pthread_mutex_lock(&lock);
        stall_us = us;
        pthread_mutex_unlock(&lock);
    }

    int error()
    {
        pthread_mutex_lock(&lock);
        const int result = failure;
        pthread_mutex_unlock(&lock);
        return result;
    }

    /* Wait until everything appended so far is written (not synced). */
    int flush()
    {
        pthread_mutex_lock(&lock);
        commit_locked();
        const uint64_t target = queued_end;
        flushing++;
        pthread_cond_signal(&wake);
        while (failure == 0 && written < target) pthread_cond_wait(&progress, &lock);
        flushing--;
        const int result = failure;
        pthread_mutex_unlock(&lock);
        if (result != 0) { errno = result; return -1; }
        return 0;
    }

    /* Overwrite bytes already written, after everything queued. The final
     * sync at close covers it. */
    int patch(uint64_t offset, const void *data, size_t size)
    {
        if (flush() < 0) return -1;
        int result = lock_file();
        if (result == 0) {
            result = write_all(fd, static_cast<const uint8_t *>(data), size, offset);
            (void)flock(fd, LOCK_UN);
        }
        pthread_mutex_lock(&lock);
        patched = true;
        if (result != 0 && failure == 0) failure = result;
        pthread_mutex_unlock(&lock);
        if (result != 0) { errno = result; return -1; }
        return 0;
    }

    /* Write and sync what is queued, then stop. Frees the writer. */
    int close()
    {
        pthread_mutex_lock(&lock);
        commit_locked();
        stopping = true;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        pthread_join(thread, nullptr);
        pthread_cond_destroy(&wake);
        pthread_cond_destroy(&progress);
        pthread_mutex_destroy(&lock);
        if (directory_fd >= 0) ::close(directory_fd);
        const int result = failure;
        delete this;
        if (result != 0) { errno = result; return -1; }
        return 0;
    }

private:
    int fd = -1, directory_fd = -1;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake, progress;
    // malloc-backed so that running out of memory is an error, not an abort
    struct Bytes {
        uint8_t *data = nullptr;
        size_t size = 0, capacity = 0;
        ~Bytes() { free(data); }
        bool add(const void *bytes, size_t length)
        {
            if (length > capacity - size) {
                // grow in steps: doubling could reserve far more than is queued
                const size_t wanted = size + length + CHUNK_BYTES;
                uint8_t *grown = wanted > size ? static_cast<uint8_t *>(realloc(data, wanted)) : nullptr;
                if (grown == nullptr) return false;
                data = grown;
                capacity = wanted;
            }
            memcpy(data + size, bytes, length);
            size += length;
            return true;
        }
        void swap(Bytes &other)
        {
            uint8_t *d = data; data = other.data; other.data = d;
            size_t n = size; size = other.size; other.size = n;
            n = capacity; capacity = other.capacity; other.capacity = n;
        }
        void clear()
        {
            size = 0;
            if (capacity > KEEP_BYTES) { free(data); data = nullptr; capacity = 0; }
        }
    };
    Bytes queue;
    size_t committed = 0;       // leading bytes of queue the worker may write
    uint64_t committed_us = 0;  // when the oldest unwritten committed byte was committed
    bool patched = false;       // patch() wrote since the last sync
    uint64_t queued_end = 0;    // file offset after everything appended
    uint64_t written = 0;       // file offset after everything written
    uint64_t stall_us = STALL_US;
    unsigned flushing = 0;
    bool stopping = false;
    int failure = 0;

    static uint64_t now_us()
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return uint64_t(now.tv_sec) * 1000000U + uint64_t(now.tv_nsec) / 1000U;
    }

    void commit_locked()
    {
        const size_t before = committed;
        committed = queue.size;
        if (committed != before && before == 0) committed_us = now_us();
        if (committed != before && (before == 0 || (before < CHUNK_BYTES && committed >= CHUNK_BYTES)))
            pthread_cond_signal(&wake);
    }

    void wait_until(uint64_t deadline_us)
    {
#ifdef __APPLE__
        // Darwin condition variables have no selectable clock.
        const uint64_t now = now_us(), left = deadline_us > now ? deadline_us - now : 0;
        const struct timespec relative = {time_t(left / 1000000U), long(left % 1000000U) * 1000};
        pthread_cond_timedwait_relative_np(&wake, &lock, &relative);
#else
        const struct timespec deadline = {time_t(deadline_us / 1000000U),
                                          long(deadline_us % 1000000U) * 1000};
        pthread_cond_timedwait(&wake, &lock, &deadline);
#endif
    }

    static int write_all(int fd, const uint8_t *data, size_t size, uint64_t at)
    {
        size_t done = 0;
        while (done < size) {
            const ssize_t n = pwrite(fd, data + done, size - done, off_t(at + done));
            if (n > 0) done += size_t(n);
            else if (n < 0 && errno == EINTR) continue;
            else return n == 0 ? EIO : errno;
        }
        return 0;
    }

    int lock_file()
    {
        while (flock(fd, LOCK_EX) < 0)
            if (errno != EINTR) return errno;
        return 0;
    }

    static void *run(void *opaque)
    {
        static_cast<ca_card_writer *>(opaque)->work();
        return nullptr;
    }

    void work()
    {
        Bytes chunk;
        uint64_t write_at = 0, sync_at = 0;
        bool dirty = false, synced = false;
        pthread_mutex_lock(&lock);
        while (failure == 0) {
            const uint64_t now = now_us();
            if (patched) { patched = false; dirty = true; }
            // an overdue sync goes first: steady writes must not postpone it
            const bool overdue = synced && dirty && !stopping && now >= sync_at;
            // the first data goes out at once so the new file is on the card early
            if (!overdue && committed != 0 &&
                (!synced || stopping || flushing != 0 || committed >= CHUNK_BYTES || now >= write_at)) {
                if (committed == queue.size) {
                    chunk.swap(queue);
                } else {
                    const size_t tail = queue.size - committed;
                    if (!chunk.add(queue.data, committed)) { failure = ENOMEM; break; }
                    memmove(queue.data, queue.data + committed, tail);
                    queue.size = tail;
                }
                committed = 0;
                const uint64_t at = written;
                pthread_mutex_unlock(&lock);
                // Files downloads take this lock to snapshot a size between units
                int result = lock_file();
                if (result == 0) {
                    result = write_all(fd, chunk.data, chunk.size, at);
                    // a full card must not leave part of a unit at the end
                    if (result != 0 && ftruncate(fd, off_t(at)) < 0) { /* the write error is what is reported */ }
                    (void)flock(fd, LOCK_UN);
                }
                const size_t size = chunk.size;
                chunk.clear();
                dirty = true;
                write_at = now_us() + WRITE_INTERVAL_US;
                pthread_mutex_lock(&lock);
                if (result != 0) failure = result;
                else written = at + size;
                pthread_cond_broadcast(&progress);
                continue;
            }
            if (overdue || (!synced ? (dirty || stopping) : (dirty && stopping))) {
                pthread_mutex_unlock(&lock);
                int result = fdatasync(fd) < 0 ? errno : 0;
                if (result == 0 && directory_fd >= 0) {
                    if (fsync(directory_fd) < 0) result = errno;
                    else { ::close(directory_fd); directory_fd = -1; }
                }
                dirty = false;
                synced = true;
                // a full interval after completion: a slow card is not hammered
                sync_at = now_us() + SYNC_INTERVAL_US;
                pthread_mutex_lock(&lock);
                if (result != 0) failure = result;
                continue;
            }
            if (stopping) break;
            if (committed != 0) wait_until(dirty && sync_at < write_at ? sync_at : write_at);
            else if (dirty) wait_until(sync_at);
            else pthread_cond_wait(&wake, &lock);
        }
        pthread_cond_broadcast(&progress);
        pthread_mutex_unlock(&lock);
    }
};

/* fsync and close fd from a background thread, a batch every few seconds, so
 * a photo save never waits for the card. Takes ownership of fd. A power cut
 * in that window can leave the newest files incomplete; a late write error is
 * only logged, as the file has already been reported saved. */
inline void ca_sync_close(int fd)
{
    const int result = fsync(fd) < 0 ? errno : 0;
    const int closed = ::close(fd) < 0 ? errno : 0;
    if (result != 0 || closed != 0)
        fprintf(stderr, "card: a saved file may be incomplete: %s\n", strerror(result ? result : closed));
}

inline void ca_sync_later(int fd)
{
    struct State {
        pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
        std::vector<int> pending;
        bool started = false;
    };
    static State state;
    static const size_t MAX_PENDING = 64;
    pthread_mutex_lock(&state.lock);
    if (!state.started) {
        pthread_t thread;
        const auto worker = [](void *) -> void * {
            std::vector<int> batch;
            for (;;) {
                pthread_mutex_lock(&state.lock);
                while (state.pending.empty()) pthread_cond_wait(&state.wake, &state.lock);
                pthread_mutex_unlock(&state.lock);
                const struct timespec interval = {time_t(ca_card_writer::SYNC_INTERVAL_US / 1000000U), 0};
                nanosleep(&interval, nullptr);
                pthread_mutex_lock(&state.lock);
                batch.swap(state.pending);
                pthread_mutex_unlock(&state.lock);
                for (const int queued : batch) ca_sync_close(queued);
                batch.clear();
            }
            return nullptr;
        };
        if (pthread_create(&thread, nullptr, worker, nullptr) == 0) {
            pthread_detach(thread);
            state.started = true;
        }
    }
    // no worker, or the card is far behind: sync here, as before batching
    const bool queue = state.started && state.pending.size() < MAX_PENDING;
    if (queue) {
        state.pending.push_back(fd);
        pthread_cond_signal(&state.wake);
    }
    pthread_mutex_unlock(&state.lock);
    if (!queue) ca_sync_close(fd);
}
