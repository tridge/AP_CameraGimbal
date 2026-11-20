#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/camera_ftp.h"
#include "camera_app/camera_definition.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* MAVLink FTP opcodes and error codes */
enum { OP_TERMINATE = 1, OP_RESET = 2, OP_LIST = 3, OP_OPEN_RO = 4, OP_READ = 5,
       OP_BURST = 15, OP_LIST_TIME = 16, OP_ACK = 128, OP_NACK = 129 };
enum { ERR_FAIL = 1, ERR_SIZE = 3, ERR_SESSION = 4, ERR_NO_SESSIONS = 5, ERR_EOF = 6,
       ERR_UNKNOWN = 7, ERR_NOT_FOUND = 10 };
#define PAYLOAD_MAX 239
#define DEFINITION_NAME (CA_CAMERA_DEFINITION_PATH + 1)

static const char *const root_names[CA_CAMERA_FTP_ROOTS] = {"record", "capture", "logs"};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

void ca_camera_ftp_init(struct ca_camera_ftp *ftp, const char *record_root,
                        const char *capture_root, const char *log_root)
{
    const char *roots[CA_CAMERA_FTP_ROOTS] = {record_root, capture_root, log_root};
    memset(ftp, 0, sizeof(*ftp));
    for (unsigned i = 0; i < CA_CAMERA_FTP_ROOTS; i++) {
        if (roots[i] && roots[i][0] == '/' && strlen(roots[i]) < CA_CAMERA_FTP_ROOT_MAX)
            strcpy(ftp->roots[i], roots[i]);
    }
}

static void close_session(struct ca_camera_ftp_session *session)
{
    if (session->active && session->fd >= 0) close(session->fd);
    session->active = false;
    session->fd = -1;
    session->burst_remaining = 0;
}

void ca_camera_ftp_close(struct ca_camera_ftp *ftp)
{
    for (unsigned i = 0; i < 4; i++) close_session(&ftp->sessions[i]);
}

/* Cygwin also interprets Windows separators and drive prefixes. Use the
 * same plain component names on all platforms, including directory replies. */
static bool valid_component(const char *name, size_t length)
{
    if (!length || (length == 1 && name[0] == '.') ||
        (length == 2 && !memcmp(name, "..", 2))) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || c == '\\' || c == ':') return false;
    }
    return true;
}

/* Validate a request path and return its path relative to an export root.
 * Returns the root index, -1 for the virtual root, -2 for the camera
 * definition or -3 when invalid. */
static int resolve(const struct ca_camera_ftp *ftp, const char *path, char relative[PATH_MAX])
{
    while (*path == '/') path++;
    if (!strncmp(path, "./", 2)) path += 2;
    if (!*path || !strcmp(path, ".")) return -1;
    if (!strcmp(path, DEFINITION_NAME)) return -2;
    size_t name = strcspn(path, "/");
    int root = -3;
    for (unsigned i = 0; i < CA_CAMERA_FTP_ROOTS; i++) {
        if (ftp->roots[i][0] && strlen(root_names[i]) == name &&
            !memcmp(path, root_names[i], name)) root = (int)i;
    }
    if (root < 0) return -3;
    /* Every further component must be a plain name: no empty, "." or ".."
     * entries, so the path cannot leave the root. */
    const char *rest = path + name;
    for (const char *p = rest; *p;) {
        if (*p != '/') return -3;
        p++;
        if (!*p) break; /* trailing slash */
        size_t length = strcspn(p, "/");
        if (!valid_component(p, length)) return -3;
        p += length;
    }
    if (*rest == '/') rest++;
    if (snprintf(relative, PATH_MAX, "%s", rest) >= PATH_MAX) return -3;
    return root;
}

/* The configured root is trusted. Resolve every requested component relative
 * to an open directory, so replacing a name with a symlink cannot escape it.
 * Nonblocking opens let the caller reject FIFOs before waiting for a writer. */
static int open_export(const struct ca_camera_ftp *ftp, int root, const char *relative,
                       bool directory)
{
    int fd = open(ftp->roots[root], O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return -1;
    while (*relative) {
        size_t length = strcspn(relative, "/");
        char component[PATH_MAX];
        memcpy(component, relative, length);
        component[length] = '\0';
        relative += length;
        bool more = *relative == '/';
        if (more) relative++;
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
        if (more || directory) flags |= O_DIRECTORY;
        int next = openat(fd, component, flags);
        int saved_errno = errno;
        close(fd);
        if (next < 0) {
            errno = saved_errno;
            return -1;
        }
        fd = next;
    }
    return fd;
}

static int compare_names(const void *a, const void *b)
{
    return strcoll(*(const char *const *)a, *(const char *const *)b);
}

/* Like scandir, but keep the directory descriptor for subsequent metadata
 * lookups instead of resolving a pathname again. */
static bool directory_names(DIR *dir, char ***result, size_t *count)
{
    char **names = NULL;
    size_t used = 0, capacity = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) goto fail;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (used == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            if (next < capacity || next > SIZE_MAX / sizeof(*names)) goto fail;
            char **grown = (char**)(realloc(names, next * sizeof(*names)));
            if (!grown) goto fail;
            names = grown;
            capacity = next;
        }
        names[used] = strdup(entry->d_name);
        if (!names[used]) goto fail;
        used++;
    }
    if (used > 1) qsort(names, used, sizeof(*names), compare_names);
    *result = names;
    *count = used;
    return true;
fail:
    for (size_t i = 0; i < used; i++) free(names[i]);
    free(names);
    return false;
}

/* One "F<name>\t<size>[\t<mtime>]" or "D<name>[\t<size>\t<mtime>]" entry;
 * -1 when the entry type is not exported. */
static int format_entry(char *out, size_t space, bool with_time, const char *name,
                        const struct stat *st)
{
    if (!valid_component(name, strlen(name))) return -1;
    if (S_ISREG(st->st_mode)) {
        return with_time ?
            snprintf(out, space, "F%s\t%llu\t%lld", name, (unsigned long long)st->st_size,
                     (long long)st->st_mtime) :
            snprintf(out, space, "F%s\t%llu", name, (unsigned long long)st->st_size);
    }
    if (S_ISDIR(st->st_mode)) {
        return with_time ? snprintf(out, space, "D%s\t0\t%lld", name, (long long)st->st_mtime) :
                           snprintf(out, space, "D%s", name);
    }
    return -1;
}

/* Directory offsets count exported entries, matching the client's count.
 * Entries that cannot fit a packet do not consume an offset. */
static uint8_t list_directory(const struct ca_camera_ftp *ftp, int root, const char *relative,
                              bool with_time, uint32_t offset, uint8_t *out, uint8_t *count)
{
    char entry[PAYLOAD_MAX + 1];
    struct stat st;
    size_t used = 0;
    uint32_t index = 0;
    *count = 0;
    if (root == -1) {
        int length = with_time ? snprintf(entry, sizeof(entry), "F%s\t%zu\t0", DEFINITION_NAME, ftp->xml_length)
                               : snprintf(entry, sizeof(entry), "F%s\t%zu", DEFINITION_NAME, ftp->xml_length);
        if (length < 0 || length >= (int)sizeof(entry)) return ERR_SIZE;
        if (index++ >= offset) { memcpy(out, entry, (size_t)length + 1); used += (size_t)length + 1; }
        for (unsigned i = 0; i < CA_CAMERA_FTP_ROOTS; i++) {
            if (!ftp->roots[i][0]) continue;
            if (with_time) {
                long long mtime = stat(ftp->roots[i], &st) == 0 ? (long long)st.st_mtime : 0;
                length = snprintf(entry, sizeof(entry), "D%s\t0\t%lld", root_names[i], mtime);
            } else {
                length = snprintf(entry, sizeof(entry), "D%s", root_names[i]);
            }
            if (index++ < offset || used + (size_t)length + 1 > PAYLOAD_MAX) continue;
            memcpy(out + used, entry, (size_t)length + 1);
            used += (size_t)length + 1;
        }
        *count = (uint8_t)used;
        return used ? 0 : ERR_EOF;
    }
    int fd = open_export(ftp, root, relative, true);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR || errno == ELOOP ? ERR_NOT_FOUND : ERR_FAIL;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return ERR_FAIL;
    }
    char **names;
    size_t total;
    if (!directory_names(dir, &names, &total)) {
        closedir(dir);
        return ERR_FAIL;
    }
    for (size_t i = 0; i < total; i++) {
        if (fstatat(fd, names[i], &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        int length = format_entry(entry, sizeof(entry), with_time, names[i], &st);
        if (length < 0 || (size_t)length + 1 > PAYLOAD_MAX) continue;
        if (index++ < offset) continue;
        if (used + (size_t)length + 1 > PAYLOAD_MAX) break;
        memcpy(out + used, entry, (size_t)length + 1);
        used += (size_t)length + 1;
    }
    for (size_t i = 0; i < total; i++) free(names[i]);
    free(names);
    closedir(dir);
    *count = (uint8_t)used;
    return used ? 0 : ERR_EOF; /* an empty packet would never advance the client */
}

static struct ca_camera_ftp_session *find_session(struct ca_camera_ftp *ftp, uint8_t system,
                                                  uint8_t component, uint8_t id)
{
    for (unsigned i = 0; i < 4; i++) {
        struct ca_camera_ftp_session *candidate = &ftp->sessions[i];
        if (candidate->active && candidate->system == system &&
            candidate->component == component && candidate->id == id) return candidate;
    }
    return NULL;
}

/* Read from the session's file or the definition; 0 at EOF, -1 on error. */
static ssize_t read_session(const struct ca_camera_ftp *ftp, const struct ca_camera_ftp_session *session,
                            uint64_t offset, uint8_t *out, size_t size)
{
    if (session->fd < 0) {
        if (offset >= ftp->xml_length) return 0;
        size_t count = ftp->xml_length - offset;
        if (count > size) count = size;
        memcpy(out, ftp->xml + offset, count);
        return (ssize_t)count;
    }
    ssize_t got;
    do got = pread(session->fd, out, size, (off_t)offset); while (got < 0 && errno == EINTR);
    return got;
}

static void set_error(uint8_t response[251], uint8_t error)
{
    response[3] = OP_NACK;
    response[4] = 1;
    response[12] = error;
}

void ca_camera_ftp_reply(struct ca_camera_ftp *ftp, const char *xml, size_t length,
                         uint8_t system, uint8_t component, uint64_t now_ms,
                         const uint8_t request[251], uint8_t response[251])
{
    uint16_t seq = (uint16_t)((unsigned)request[0] | ((unsigned)request[1] << 8));
    seq++;
    memset(response, 0, 251);
    response[0] = (uint8_t)seq;
    response[1] = (uint8_t)(seq >> 8);
    response[2] = request[2];
    response[3] = OP_ACK;
    response[5] = request[3];
    memcpy(response + 8, request + 8, 4);
    ftp->xml = xml;
    ftp->xml_length = length;
    uint8_t error = 0;
    uint8_t opcode = request[3], size = request[4];
    for (unsigned i = 0; i < 4; i++) {
        if (ftp->sessions[i].active && now_ms - ftp->sessions[i].last_ms > 60000)
            close_session(&ftp->sessions[i]);
    }
    struct ca_camera_ftp_session *session = find_session(ftp, system, component, request[2]);
    /* a new request from this client ends any burst still in progress */
    for (unsigned i = 0; i < 4; i++) {
        if (ftp->sessions[i].active && ftp->sessions[i].system == system &&
            ftp->sessions[i].component == component) ftp->sessions[i].burst_remaining = 0;
    }
    char path[240] = {};
    char relative[PATH_MAX];
    if (size > PAYLOAD_MAX) {
        error = ERR_SIZE;
    } else if (opcode == OP_RESET) { /* only for the requesting client */
        for (unsigned i = 0; i < 4; i++) {
            if (ftp->sessions[i].system == system && ftp->sessions[i].component == component)
                close_session(&ftp->sessions[i]);
        }
    } else if (opcode == OP_LIST || opcode == OP_LIST_TIME) {
        memcpy(path, request + 12, size);
        int root = resolve(ftp, path, relative);
        if (root == -2 || root == -3) error = ERR_NOT_FOUND;
        else error = list_directory(ftp, root, relative, opcode == OP_LIST_TIME,
                                    get32(request + 8), response + 12, &response[4]);
    } else if (opcode == OP_OPEN_RO) {
        memcpy(path, request + 12, size);
        int root = resolve(ftp, path, relative);
        int fd = -1;
        uint64_t file_size = length;
        if (root == -1 || root == -3) {
            error = ERR_NOT_FOUND;
        } else if (root >= 0) {
            struct stat st;
            fd = open_export(ftp, root, relative, false);
            if (fd < 0) {
                error = errno == ENOENT || errno == ENOTDIR || errno == ELOOP ? ERR_NOT_FOUND : ERR_FAIL;
            } else if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                close(fd);
                fd = -1;
                error = ERR_NOT_FOUND;
            } else {
                file_size = (uint64_t)st.st_size;
            }
        }
        if (!error) {
            /* Reopening reuses this client's session, so a retry of a lost
             * Open ACK is harmless without leaking slots. */
            if (session) {
                close_session(session);
            } else {
                for (unsigned i = 0; i < 4 && !session; i++)
                    if (!ftp->sessions[i].active) session = &ftp->sessions[i];
            }
            if (!session) {
                if (fd >= 0) close(fd);
                error = ERR_NO_SESSIONS;
            } else {
                *session = (struct ca_camera_ftp_session){
                    .system = system, .component = component, .id = request[2],
                    .active = true, .last_ms = now_ms, .fd = fd, .size = file_size};
                response[4] = 4;
                put32(response + 12, file_size > UINT32_MAX ? UINT32_MAX : (uint32_t)file_size);
            }
        }
    } else if (opcode == OP_TERMINATE) {
        if (session) close_session(session);
        else error = ERR_SESSION;
    } else if (opcode == OP_READ || opcode == OP_BURST) {
        uint32_t offset = get32(request + 8);
        if (!session) error = ERR_SESSION;
        else if (!size) error = ERR_SIZE;
        else if (offset >= session->size) error = ERR_EOF;
        else {
            ssize_t got = read_session(ftp, session, offset, response + 12, size);
            if (got < 0) error = ERR_FAIL;
            else if (got == 0) error = ERR_EOF;
            else {
                response[4] = (uint8_t)got;
                session->last_ms = now_ms;
                if (opcode == OP_BURST) {
                    /* A bounded burst avoids starving control traffic; the
                     * GCS requests the next burst at the next offset. */
                    unsigned packets = ftp->burst_packets ? ftp->burst_packets : 1;
                    bool more = packets > 1 && (size_t)got == size &&
                                offset + (uint64_t)got < session->size;
                    response[6] = more ? 0 : 1;
                    if (more) {
                        session->burst_offset = offset + (uint32_t)got;
                        session->burst_remaining = packets - 1;
                        session->burst_seq = (uint16_t)(seq + 1);
                        session->burst_size = size;
                    }
                }
            }
        }
    } else {
        error = ERR_UNKNOWN; /* no writes */
    }
    if (error) set_error(response, error);
}

bool ca_camera_ftp_burst_next(struct ca_camera_ftp *ftp, uint8_t system,
                              uint8_t component, uint8_t response[251], bool final)
{
    struct ca_camera_ftp_session *session = NULL;
    for (unsigned i = 0; i < 4 && !session; i++) {
        struct ca_camera_ftp_session *candidate = &ftp->sessions[i];
        if (candidate->active && candidate->burst_remaining && candidate->system == system &&
            candidate->component == component) session = candidate;
    }
    if (!session) return false;
    memset(response, 0, 251);
    response[0] = (uint8_t)session->burst_seq;
    response[1] = (uint8_t)(session->burst_seq >> 8);
    response[2] = session->id;
    response[3] = OP_ACK;
    response[5] = OP_BURST;
    put32(response + 8, session->burst_offset);
    session->burst_seq++;
    session->burst_remaining--;
    ssize_t got = session->burst_offset >= session->size ? 0 :
        read_session(ftp, session, session->burst_offset, response + 12, session->burst_size);
    if (got <= 0) {
        /* end the burst where the data stopped, as the autopilot does */
        set_error(response, got < 0 ? ERR_FAIL : ERR_EOF);
        session->burst_remaining = 0;
        return true;
    }
    response[4] = (uint8_t)got;
    session->burst_offset += (uint32_t)got;
    if (final || (size_t)got < session->burst_size || session->burst_offset >= session->size)
        session->burst_remaining = 0;
    response[6] = session->burst_remaining ? 0 : 1;
    return true;
}
