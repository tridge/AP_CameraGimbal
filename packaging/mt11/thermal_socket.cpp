/*
 * Raw-thermal compatibility service used while the vendor camera application
 * is active.  MAVProxy accepts this uncompressed filename+timestamp+Y16 wire
 * format as well as the older zlib-compressed helper format.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_PORT 7345U
#define THERMAL_DIR "/mnt/DCIM/capture"
#define FRAME_BYTES (640U * 512U * 2U)
#define SEND_CHUNK_BYTES 1024U
#define TARGET_SEND_TIME_US 500000U

struct __attribute__((packed)) raw_header {
    char filename[128];
    double timestamp;
};

struct watch {
    int descriptor;
    char *path;
};

struct state {
    const char *root;
    int inotify_fd;
    struct watch *watches;
    size_t watch_count;
    size_t watch_capacity;
    bool have_latest;
    char latest[PATH_MAX];
    int latest_key[7];
};

static bool has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}
/* Milliseconds in MT11 filenames are not zero padded, so lexical comparison
 * gives the wrong order for values such as 8 and 609. */
static bool capture_key(const char *path, int key[7])
{
    const char *name = strrchr(path, '/');
    int consumed = 0;

    name = name == NULL ? path : name + 1;
    if (sscanf(name, "%d-%d-%d_%d-%d-%d_%d_I.bin%n",
               &key[0], &key[1], &key[2], &key[3], &key[4], &key[5],
               &key[6], &consumed) != 7) {
        return false;
    }
    return name[consumed] == '\0';
}

static int compare_key(const int left[7], const int right[7])
{
    unsigned index;

    for (index = 0; index < 7; index++) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    return 0;
}

static void consider_file(struct state *state, const char *path)
{
    struct stat attributes;
    int key[7];

    if (!has_suffix(path, "_I.bin") || lstat(path, &attributes) != 0 ||
        !S_ISREG(attributes.st_mode) || attributes.st_size != FRAME_BYTES ||
        !capture_key(path, key)) {
        return;
    }
    if (state->have_latest && compare_key(key, state->latest_key) <= 0) {
        return;
    }
    if (snprintf(state->latest, sizeof(state->latest), "%s", path) >=
        (int)sizeof(state->latest)) {
        return;
    }
    memcpy(state->latest_key, key, sizeof(key));
    state->have_latest = true;
}

static const char *watch_path(const struct state *state, int descriptor)
{
    size_t index;

    for (index = 0; index < state->watch_count; index++) {
        if (state->watches[index].descriptor == descriptor) {
            return state->watches[index].path;
        }
    }
    return NULL;
}

static bool is_watched(const struct state *state, const char *path)
{
    size_t index;

    for (index = 0; index < state->watch_count; index++) {
        if (strcmp(state->watches[index].path, path) == 0) return true;
    }
    return false;
}

static void remove_watch(struct state *state, int descriptor)
{
    size_t index;

    for (index = 0; index < state->watch_count; index++) {
        if (state->watches[index].descriptor == descriptor) {
            free(state->watches[index].path);
            state->watches[index] = state->watches[state->watch_count - 1U];
            state->watch_count--;
            return;
        }
    }
}

static void add_watch(struct state *state, const char *path)
{
    static const uint32_t mask = IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE |
                                 IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF |
                                 IN_MOVE_SELF;
    struct watch *watches;
    int descriptor;

    if (state->inotify_fd < 0 || is_watched(state, path)) return;
    descriptor = inotify_add_watch(state->inotify_fd, path, mask);
    if (descriptor < 0) return;
    if (state->watch_count == state->watch_capacity) {
        size_t capacity = state->watch_capacity == 0U
                              ? 8U : state->watch_capacity * 2U;

        watches = (struct watch *)realloc(state->watches, capacity * sizeof(*watches));
        if (watches == NULL) {
            (void)inotify_rm_watch(state->inotify_fd, descriptor);
            return;
        }
        state->watches = watches;
        state->watch_capacity = capacity;
    }
    state->watches[state->watch_count].descriptor = descriptor;
    state->watches[state->watch_count].path = strdup(path);
    if (state->watches[state->watch_count].path == NULL) {
        (void)inotify_rm_watch(state->inotify_fd, descriptor);
        return;
    }
    state->watch_count++;
}

/* Add each directory watch before scanning it, closing the create-before-watch
 * race at startup and when a new date directory appears. */
static void scan_tree(struct state *state, const char *path, bool add_watches)
{
    DIR *directory;
    struct dirent *entry;

    if (add_watches) add_watch(state, path);
    directory = opendir(path);
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat attributes;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
                (int)sizeof(child) ||
            lstat(child, &attributes) != 0) {
            continue;
        }
        if (S_ISDIR(attributes.st_mode)) {
            scan_tree(state, child, add_watches);
        } else if (S_ISREG(attributes.st_mode)) {
            consider_file(state, child);
        }
    }
    closedir(directory);
}

static void process_inotify(struct state *state)
{
    char buffer[16U * 1024U]
        __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t length;

    while ((length = read(state->inotify_fd, buffer, sizeof(buffer))) > 0) {
        char *position = buffer;

        while (position < buffer + length) {
            const struct inotify_event *event =
                (const struct inotify_event *)position;
            const char *directory = watch_path(state, event->wd);
            char path[PATH_MAX];
            bool have_path = false;

            if (directory != NULL && event->len != 0U &&
                snprintf(path, sizeof(path), "%s/%s", directory,
                         event->name) < (int)sizeof(path)) {
                have_path = true;
            }
            if (event->mask & IN_Q_OVERFLOW) {
                scan_tree(state, state->root, true);
            } else if (have_path && (event->mask & IN_ISDIR) &&
                       (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                scan_tree(state, path, true);
            } else if (have_path && !(event->mask & IN_ISDIR) &&
                       (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) {
                consider_file(state, path);
            }
            if (event->mask & IN_IGNORED) remove_watch(state, event->wd);
            position += sizeof(*event) + event->len;
        }
    }
    if (length < 0 && errno != EAGAIN && errno != EINTR) {
        perror("inotify read");
    }
}

static bool write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;

    while (length != 0U) {
        ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);

        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static double capture_timestamp(const int key[7])
{
    struct tm local {};
    local.tm_year = key[0] - 1900;
    local.tm_mon = key[1] - 1;
    local.tm_mday = key[2];
    local.tm_hour = key[3];
    local.tm_min = key[4];
    local.tm_sec = key[5];
    local.tm_isdst = -1;
    time_t seconds = mktime(&local);

    if (seconds == (time_t)-1) return 0.0;
    return (double)seconds + (double)key[6] / 1000.0;
}

static void serve_frame(int fd, const struct state *state)
{
    struct raw_header header = {{0}, 0.0};
    uint8_t buffer[SEND_CHUNK_BYTES];
    size_t remaining = FRAME_BYTES;
    int input;

    input = open(state->latest, O_RDONLY | O_CLOEXEC);
    if (input < 0) return;
    memcpy(header.filename, state->latest,
           strnlen(state->latest, sizeof(header.filename) - 1U));
    header.timestamp = capture_timestamp(state->latest_key);
    if (!write_all(fd, &header, sizeof(header))) goto done;
    while (remaining != 0U) {
        size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
        ssize_t count = read(input, buffer, wanted);
        useconds_t delay;

        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || !write_all(fd, buffer, (size_t)count)) break;
        remaining -= (size_t)count;
        delay = (useconds_t)((uint64_t)TARGET_SEND_TIME_US *
                             (size_t)count / FRAME_BYTES);
        if (delay != 0U) (void)usleep(delay);
    }
done:
    close(input);
}

static int open_listener(unsigned port)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
                             sizeof(one)) < 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 20) < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static void run_server(const char *root, unsigned port)
{
    struct state state = {.root = root, .inotify_fd = -1};
    int listen_fd;

    state.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    scan_tree(&state, root, state.inotify_fd >= 0);
    listen_fd = open_listener(port);
    if (listen_fd < 0) {
        perror("thermal_socket listen");
        exit(1);
    }
    printf("Raw thermal service listening on TCP %u (uncompressed)\n", port);
    for (;;) {
        struct pollfd poll_fds[2] = {
            {.fd = listen_fd, .events = POLLIN},
            {.fd = state.inotify_fd, .events = POLLIN},
        };
        nfds_t count = state.inotify_fd >= 0 ? 2U : 1U;
        int result = poll(poll_fds, count, 1000);

        if (result < 0 && errno == EINTR) continue;
        if (result < 0) {
            perror("thermal_socket poll");
            exit(1);
        }
        if (state.inotify_fd >= 0 && (poll_fds[1].revents & POLLIN)) {
            process_inotify(&state);
        }
        if (state.inotify_fd >= 0 && !is_watched(&state, root)) {
            scan_tree(&state, root, true);
        }
        if (poll_fds[0].revents & POLLIN) {
            int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);

            if (fd < 0) continue;
            if (state.inotify_fd < 0) scan_tree(&state, root, false);
            if (state.have_latest) serve_frame(fd, &state);
            close(fd);
        }
    }
}

int main(int argc, char **argv)
{
    const char *root = THERMAL_DIR;
    unsigned port = LISTEN_PORT;

    if (argc > 3) {
        fprintf(stderr, "Usage: %s [capture-directory [port]]\n", argv[0]);
        return 2;
    }
    if (argc >= 2) root = argv[1];
    if (argc == 3) {
        char *end = NULL;
        unsigned long value = strtoul(argv[2], &end, 10);

        if (argv[2][0] == '\0' || *end != '\0' || value == 0U ||
            value > 65535U) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 2;
        }
        port = (unsigned)value;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    run_server(root, port);
    return 0;
}
