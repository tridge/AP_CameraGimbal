#include "camera_app/APC_NetworkCapture.h"
#include "camera_app/log.h"
#include "apcam/APC_Resource.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

// Classic PCAP with Linux cooked v1 headers supports all network interfaces,
// including loopback, without libpcap or target-specific tcpdump binaries.
static constexpr uint32_t snaplen = 262144;
#ifndef CA_NETWORK_CAPTURE_FILE_BYTES
#define CA_NETWORK_CAPTURE_FILE_BYTES (16U * 1024U * 1024U)
#endif
static constexpr unsigned file_count = 4;
struct pcap_header {
    uint32_t magic;
    uint16_t major, minor;
    uint32_t zone, precision, snaplen, linktype;
};
struct packet_header {
    uint32_t seconds, microseconds, captured, original;
    uint16_t type, hardware, address_length;
    uint8_t address[8];
    uint16_t protocol;
};
static_assert(sizeof(pcap_header) == 24 && sizeof(packet_header) == 32, "PCAP layout");

static int write_packet(int fd, const packet_header &header, void *payload, size_t length)
{
    iovec parts[2] = {{const_cast<packet_header *>(&header), sizeof(header)}, {payload, length}};
    unsigned i = 0;
    while (i < 2) {
        ssize_t n = writev(fd, parts + i, 2 - i);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        size_t used = size_t(n);
        while (i < 2 && used >= parts[i].iov_len) used -= parts[i++].iov_len;
        if (i < 2) {
            parts[i].iov_base = static_cast<char *>(parts[i].iov_base) + used;
            parts[i].iov_len -= used;
        }
    }
    return 0;
}

static int new_file(int directory, unsigned slot)
{
    char name[32], temporary[64];
    snprintf(name, sizeof(name), "network-%u.pcap", slot);
    snprintf(temporary, sizeof(temporary), ".network-%u-%ld.tmp", slot, (long)getpid());
    // An old download retains its inode when a ring slot is replaced.
    (void)unlinkat(directory, temporary, 0);
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    const pcap_header header {0xa1b2c3d4, 2, 4, 0, 0, snaplen, 113};
    if (write(fd, &header, sizeof(header)) != sizeof(header) ||
        renameat(directory, temporary, directory, name) < 0) {
        const int saved = errno ? errno : EIO;
        ::close(fd);
        (void)unlinkat(directory, temporary, 0);
        errno = saved;
        return -1;
    }
    return fd;
}
#endif

int APC_NetworkCapture::init(const char *record_root, const char *ready_path)
{
    if (_started) { errno = EALREADY; return -1; }
    const char *slash = strrchr(record_root, '/');
    if (!slash || snprintf(_root, sizeof(_root), "%.*s/network", int(slash-record_root), record_root) >= int(sizeof(_root)) ||
        snprintf(_status_path, sizeof(_status_path), "%s.capture", ready_path) >= int(sizeof(_status_path))) {
        errno = ENAMETOOLONG; return -1;
    }
    int error = pthread_create(&_thread, nullptr, worker, this);
    if (error) { errno = error; return -1; }
    _started = true;
    return 0;
}

void APC_NetworkCapture::configure(bool enabled)
{
    if (!_started) return;
    pthread_mutex_lock(&_mutex);
    if (_enabled != enabled) {
        _enabled = enabled;
        ++_generation;
        pthread_cond_signal(&_wake);
    }
    pthread_mutex_unlock(&_mutex);
}

void APC_NetworkCapture::close()
{
    if (!_started) return;
    pthread_mutex_lock(&_mutex);
    _quit = true;
    pthread_cond_signal(&_wake);
    pthread_mutex_unlock(&_mutex);
    pthread_join(_thread, nullptr);
    _started = false;
}

bool APC_NetworkCapture::changed(uint64_t generation) const
{
    return _quit || _generation != generation;
}

void APC_NetworkCapture::status(bool error, const char *message)
{
    ca_log("network capture: %s", message);
    char temporary[PATH_MAX + 5];
    snprintf(temporary, sizeof(temporary), "%s.tmp", _status_path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    bool ok = fprintf(file, "%ld %u\n%s\n", (long)getpid(), error ? 1U : 0U, message) > 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporary, _status_path) < 0) (void)unlink(temporary);
}

void *APC_NetworkCapture::worker(void *opaque)
{
    static_cast<APC_NetworkCapture *>(opaque)->run();
    return nullptr;
}

void APC_NetworkCapture::run()
{
    uint64_t generation = 0;
    status(false, "Off");
    while (!_quit) {
        pthread_mutex_lock(&_mutex);
        while (!_quit && generation == _generation) pthread_cond_wait(&_wake, &_mutex);
        generation = _generation;
        const bool enabled = _enabled;
        pthread_mutex_unlock(&_mutex);
        if (_quit) break;
        if (!enabled) { status(false, "Off; saved capture files are available in Files / DCIM / network."); continue; }
        if (capture(generation) < 0) {
            char message[256];
            snprintf(message, sizeof(message), "Capture failed: %s. Disable and re-enable to retry.", strerror(errno));
            status(true, message);
        }
    }
    status(false, "Off");
}

int APC_NetworkCapture::capture(uint64_t generation)
{
#ifndef __linux__
    (void)generation;
    errno = ENOTSUP;
    return -1;
#else
    APC_FileDescriptor socket_fd(socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, htons(ETH_P_ALL)));
    if (socket_fd.get() < 0) return -1;
    int enabled = 1, buffer_size = 4 * 1024 * 1024;
    if (setsockopt(socket_fd.get(), SOL_SOCKET, SO_TIMESTAMP, &enabled, sizeof(enabled)) < 0) return -1;
    (void)setsockopt(socket_fd.get(), SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    if (mkdir(_root, 0755) < 0 && errno != EEXIST) return -1;
    APC_FileDescriptor directory(open(_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) return -1;
    APC_FileDescriptor lock(openat(directory.get(), ".capture.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0 || flock(lock.get(), LOCK_EX | LOCK_NB) < 0) return -1;
    unsigned slot = 0;
    // FAT timestamps can tie across several slots. Persist the next slot so
    // restarting capture cannot overwrite a newer file ahead of an older one.
    char cursor;
    const bool have_cursor = pread(lock.get(), &cursor, 1, 0) == 1 && cursor >= '0' && unsigned(cursor - '0') < file_count;
    if (have_cursor) slot = unsigned(cursor - '0');
    struct timespec oldest {LONG_MAX, 0};
    for (unsigned i = 0; !have_cursor && i < file_count; i++) {
        char name[32];
        snprintf(name, sizeof(name), "network-%u.pcap", i);
        struct stat st;
        if (fstatat(directory.get(), name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno != ENOENT) return -1;
            slot = i; break;
        }
        if (st.st_mtim.tv_sec < oldest.tv_sec ||
            (st.st_mtim.tv_sec == oldest.tv_sec && st.st_mtim.tv_nsec < oldest.tv_nsec)) {
            oldest = st.st_mtim; slot = i;
        }
    }
    APC_FileDescriptor file;
    // Capture storage never shares the protocol/control thread's allocation.
    uint8_t *payload = static_cast<uint8_t *>(malloc(snaplen - 16));
    if (!payload) { errno = ENOMEM; return -1; }
    status(false, "Capturing all interfaces; waiting for packets.");
    size_t size = 0;
    int result = 0;
    uint64_t packets = 0, dropped = 0;
    struct timespec last_sync {};
    while (!changed(generation)) {
        pollfd poll_socket {socket_fd.get(), POLLIN, 0};
        int ready = poll(&poll_socket, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { result = -1; break; }
        if (poll_socket.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; result = -1; break; }
        if (ready) {
            sockaddr_ll address {};
            union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(timeval))]; } ancillary {};
            iovec part {payload, snaplen - 16};
            msghdr message {};
            message.msg_name = &address; message.msg_namelen = sizeof(address);
            message.msg_iov = &part; message.msg_iovlen = 1;
            message.msg_control = ancillary.bytes; message.msg_controllen = sizeof(ancillary.bytes);
            ssize_t n = recvmsg(socket_fd.get(), &message, MSG_TRUNC);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < 0) { result = -1; break; }
            timeval stamp {};
            for (cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP && c->cmsg_len >= CMSG_LEN(sizeof(stamp)))
                    memcpy(&stamp, CMSG_DATA(c), sizeof(stamp));
            }
            if (!stamp.tv_sec) gettimeofday(&stamp, nullptr);
            const size_t length = size_t(n) < snaplen - 16 ? size_t(n) : snaplen - 16;
            if (file.get() < 0 || size + sizeof(packet_header) + length > CA_NETWORK_CAPTURE_FILE_BYTES) {
                if (file.get() >= 0 && fsync(file.get()) < 0) { result = -1; break; }
                file.reset(new_file(directory.get(), slot));
                if (file.get() < 0) { result = -1; break; }
                size = sizeof(pcap_header);
                char text[128];
                snprintf(text, sizeof(text), "Capturing all interfaces to network-%u.pcap", slot);
                status(false, text);
                slot = (slot + 1) % file_count;
                cursor = char('0' + slot);
                if (pwrite(lock.get(), &cursor, 1, 0) != 1 || fsync(lock.get()) < 0) { result = -1; break; }
            }
            packet_header header {uint32_t(stamp.tv_sec), uint32_t(stamp.tv_usec), uint32_t(length + 16), uint32_t(n + 16),
                                  htons(address.sll_pkttype), htons(address.sll_hatype), htons(address.sll_halen), {}, address.sll_protocol};
            memcpy(header.address, address.sll_addr, sizeof(header.address));
            if (write_packet(file.get(), header, payload, length) < 0) {
                const int saved = errno;
                if (ftruncate(file.get(), off_t(size)) < 0)
                    ca_log("network capture: cannot discard incomplete packet: %s", strerror(errno));
                errno = saved; result = -1; break;
            }
            size += sizeof(header) + length;
            ++packets;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > last_sync.tv_sec) {
            if (file.get() >= 0 && fsync(file.get()) < 0) { result = -1; break; }
            tpacket_stats stats {};
            socklen_t length = sizeof(stats);
            if (getsockopt(socket_fd.get(), SOL_PACKET, PACKET_STATISTICS, &stats, &length) == 0) dropped += stats.tp_drops;
            last_sync = now;
        }
    }
    int saved = errno;
    if (file.get() >= 0 && fsync(file.get()) < 0 && result == 0) { result = -1; saved = errno; }
    tpacket_stats stats {};
    socklen_t stats_length = sizeof(stats);
    if (getsockopt(socket_fd.get(), SOL_PACKET, PACKET_STATISTICS, &stats, &stats_length) == 0)
        dropped += stats.tp_drops;
    free(payload);
    ca_log("network capture stopped: %llu packets, %llu kernel drops", (unsigned long long)packets, (unsigned long long)dropped);
    errno = saved;
    return result;
#endif
}
