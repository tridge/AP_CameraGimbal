#pragma once

/* Background writes to the card. A slow card must never block the thread that
 * produces the data, so appends are queued and one worker per file writes
 * them out in large chunks and syncs every few seconds. */

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <new>
#include <vector>

class ca_card_writer {
public:
    static constexpr size_t CHUNK_BYTES = 1U << 20;
    static constexpr size_t QUEUE_MAX_BYTES = 64U << 20;
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
     * of the file only ever sees whole committed units. */
    int append(const void *data, size_t size)
    {
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        pthread_mutex_lock(&lock);
        int result = failure;
        if (result == 0) {
            if (queue.empty()) oldest_us = now_us();
            queue.insert(queue.end(), bytes, bytes + size);
            queued_end += size;
        }
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
        const bool result = !queue.empty() &&
            (queue.size() > QUEUE_MAX_BYTES || now_us() - oldest_us > stall_us);
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
    std::vector<uint8_t> queue;
    size_t committed = 0;       // leading bytes of queue the worker may write
    uint64_t queued_end = 0;    // file offset after everything appended
    uint64_t written = 0;       // file offset after everything written
    uint64_t oldest_us = 0;     // when the queue last went from empty to holding data
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
        committed = queue.size();
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

    static void *run(void *opaque)
    {
        static_cast<ca_card_writer *>(opaque)->work();
        return nullptr;
    }

    void work()
    {
        std::vector<uint8_t> chunk;
        uint64_t write_at = 0, sync_at = 0;
        bool dirty = false, synced = false;
        pthread_mutex_lock(&lock);
        while (failure == 0) {
            const uint64_t now = now_us();
            // the first data goes out at once so the new file is on the card early
            if (committed != 0 && (!synced || stopping || flushing != 0 ||
                                   committed >= CHUNK_BYTES || now >= write_at)) {
                if (committed == queue.size()) {
                    chunk.swap(queue);
                } else {
                    chunk.assign(queue.begin(), queue.begin() + committed);
                    queue.erase(queue.begin(), queue.begin() + committed);
                    oldest_us = now;
                }
                committed = 0;
                const uint64_t at = written;
                pthread_mutex_unlock(&lock);
                // Files downloads take this lock to snapshot a size between units
                int result = flock(fd, LOCK_EX) < 0 ? errno : 0;
                if (result == 0) {
                    result = write_all(fd, chunk.data(), chunk.size(), at);
                    (void)flock(fd, LOCK_UN);
                }
                const size_t size = chunk.size();
                chunk.clear();
                dirty = true;
                write_at = now_us() + WRITE_INTERVAL_US;
                pthread_mutex_lock(&lock);
                if (result != 0) failure = result;
                else written = at + size;
                pthread_cond_broadcast(&progress);
                continue;
            }
            if (!synced ? (dirty || stopping) : (dirty && (stopping || now >= sync_at))) {
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
 * in that window can leave the newest files incomplete. */
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
                for (const int queued : batch) { (void)fsync(queued); (void)::close(queued); }
                batch.clear();
            }
            return nullptr;
        };
        if (pthread_create(&thread, nullptr, worker, nullptr) == 0) {
            pthread_detach(thread);
            state.started = true;
        }
    }
    // no worker, or the card is far behind: do not hold descriptors open
    const bool queue = state.started && state.pending.size() < MAX_PENDING;
    if (queue) {
        state.pending.push_back(fd);
        pthread_cond_signal(&state.wake);
    }
    pthread_mutex_unlock(&state.lock);
    if (!queue) {
        if (!state.started) (void)fsync(fd);
        (void)::close(fd);
    }
}
