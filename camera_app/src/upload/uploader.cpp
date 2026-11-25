/* Runs a native HTTPS worker as a child process to copy photos and recordings to a cloud
 * remote. A small state machine driven from the main loop: it never blocks,
 * and every failure ends in DISABLED or BACKOFF so video is unaffected. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/uploader.h"
#include "camera_app/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

#define STOP_GRACE_MS 2000U
#define BACKOFF_FIRST_MS 30000U
#define BACKOFF_MAX_MS 120000U

enum job_kind { JOB_PHOTOS, JOB_RECORDINGS };

struct ca_uploader {
    struct ca_upload_settings settings;
    struct ca_upload_status status;
    pid_t pid;
    int fd;
    enum job_kind jobs[2];
    unsigned job_count, job_index;
    uint64_t next_scan, retry_at, kill_at;
    unsigned backoff_ms;
    bool killed, blocked;
    char line[4096];
    size_t line_length;
    bool line_overflow;
    /* Progress of finished jobs in this scan, plus the running one. */
    uint64_t base_bytes, base_total, job_bytes, job_total;
    unsigned base_files, base_files_total, job_files, job_files_total;
    char job_error[160];
};

static void update_progress(struct ca_uploader *u)
{
    u->status.bytes_done = u->base_bytes + u->job_bytes;
    u->status.bytes_total = u->base_total + u->job_total;
    u->status.files_done = u->base_files + u->job_files;
    u->status.files_total = u->base_files_total + u->job_files_total;
}

static void set_text(char *to, size_t size, const char *from)
{
    size_t length = strlen(from);
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = '\0';
}

/* ---- parsing rclone's --use-json-log output ---- */

static bool json_number(const char *text, const char *key, double *out)
{
    const char *p = strstr(text, key);
    char *end;
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ') p++;
    double value = strtod(p, &end);
    if (end == p) return false;
    *out = value;
    return true;
}

static bool json_string(const char *text, const char *key, char *out, size_t size)
{
    const char *p = strstr(text, key);
    size_t used = 0;
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    for (p++; *p && *p != '"' && used + 1 < size; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            if (*p == 'u') { /* not decoded */
                for (int i = 0; i < 4 && p[1]; i++) p++;
                c = '?';
            } else {
                c = (*p == 'n' || *p == 't' || *p == 'r') ? ' ' : *p;
            }
        } else if ((unsigned char)c < ' ') {
            c = ' ';
        }
        out[used++] = c;
    }
    out[used] = '\0';
    return true;
}

static void parse_line(struct ca_uploader *u, const char *line)
{
    char level[16];
    if (json_string(line, "\"level\":", level, sizeof(level)) &&
        (!strcmp(level, "error") || !strcmp(level, "critical") || !strcmp(level, "fatal"))) {
        char message[sizeof(u->job_error)];
        if (json_string(line, "\"msg\":", message, sizeof(message)) && message[0])
            set_text(u->job_error, sizeof(u->job_error), message);
    }
    const char *stats = strstr(line, "\"stats\":{");
    if (!stats) return;
    double value;
    /* "bytes" and "transfers" are matched with their opening quote so
     * "totalBytes" and "totalTransfers" are not mistaken for them. */
    if (json_number(stats, "\"bytes\":", &value)) u->job_bytes = value > 0 ? (uint64_t)value : 0;
    if (json_number(stats, "\"totalBytes\":", &value)) u->job_total = value > 0 ? (uint64_t)value : 0;
    if (json_number(stats, "\"transfers\":", &value)) u->job_files = value > 0 ? (unsigned)value : 0;
    if (json_number(stats, "\"totalTransfers\":", &value)) u->job_files_total = value > 0 ? (unsigned)value : 0;
    char last[sizeof(u->job_error)];
    if (json_string(stats, "\"lastError\":", last, sizeof(last)) && last[0])
        set_text(u->job_error, sizeof(u->job_error), last);
    update_progress(u);
}

static void drain(struct ca_uploader *u)
{
    if (u->fd < 0) return;
    for (;;) {
        char buffer[1024];
        ssize_t n = read(u->fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] == '\n') {
                if (!u->line_overflow) {
                    u->line[u->line_length] = '\0';
                    parse_line(u, u->line);
                }
                u->line_length = 0;
                u->line_overflow = false;
            } else if (u->line_length + 1 < sizeof(u->line)) {
                u->line[u->line_length++] = buffer[i];
            } else {
                u->line_overflow = true;
            }
        }
    }
}

/* ---- settings ---- */

static bool is_directory(const char *path)
{
    struct stat info;
    return path[0] && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool settings_usable(const struct ca_upload_settings *s, char *reason, size_t size)
{
    if (!s->enable) { set_text(reason, size, "Disabled"); return false; }
    if (!s->executable[0] || access(s->executable, X_OK) != 0) {
        snprintf(reason, size, "Upload worker not found at %.64s", s->executable);
        return false;
    }
    if (!s->config_path[0]) { set_text(reason, size, "No upload credentials file set"); return false; }
    /* A remote needs its colon: without one rclone would copy into a folder
     * on this device. A leading '-' would be read as an option. */
    if (!s->remote[0] || s->remote[0] == '-' || !strchr(s->remote, ':')) {
        set_text(reason, size, "Remote must look like name:folder");
        return false;
    }
    if (!s->what || (s->what & ~(CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS))) {
        set_text(reason, size, "Nothing selected to upload");
        return false;
    }
    return true;
}

static bool space_is_low(const char *path, unsigned percent)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0 || v.f_blocks == 0) return false;
    return (uint64_t)v.f_bavail * 100U < (uint64_t)percent * v.f_blocks;
}

/* ---- the child process ---- */

static void set_disabled(struct ca_uploader *u, const char *reason)
{
    if (u->status.state != CA_UPLOAD_DISABLED || strcmp(u->status.reason, reason) != 0) {
        if (strcmp(reason, "Disabled") != 0) ca_log("upload: %s", reason);
        set_text(u->status.reason, sizeof(u->status.reason), reason);
    }
    u->status.state = CA_UPLOAD_DISABLED;
}

static void fail_run(struct ca_uploader *u, uint64_t now, const char *error)
{
    set_text(u->status.last_error, sizeof(u->status.last_error), error);
    u->backoff_ms = u->backoff_ms ? (u->backoff_ms * 2U > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : u->backoff_ms * 2U)
                                  : BACKOFF_FIRST_MS;
    u->retry_at = now + u->backoff_ms;
    u->base_bytes = u->base_total = u->job_bytes = u->job_total = 0;
    u->base_files = u->base_files_total = u->job_files = u->job_files_total = 0;
    u->status.state = CA_UPLOAD_BACKOFF;
    ca_log("upload: %s; retrying in %u s", error, u->backoff_ms / 1000U);
}

static bool start_job(struct ca_uploader *u, uint64_t now)
{
    const struct ca_upload_settings *s = &u->settings;
    const bool photos = u->jobs[u->job_index] == JOB_PHOTOS;
    const char *source = photos ? s->capture_root : s->record_root;
    const bool move = s->after_upload == CA_UPLOAD_DELETE ||
                      (s->after_upload == CA_UPLOAD_DELETE_WHEN_LOW && space_is_low(source, s->low_space_percent));
    const size_t remote_length = strlen(s->remote);
    char destination[sizeof(s->remote) + 16], bandwidth[24];
    snprintf(destination, sizeof(destination), "%s%s%s", s->remote,
             s->remote[remote_length - 1] == ':' || s->remote[remote_length - 1] == '/' ? "" : "/",
             photos ? "photos" : "recordings");
    snprintf(bandwidth, sizeof(bandwidth), "%uk", s->bandwidth_kib_s);

    const char *argv[32];
    size_t n = 0;
    argv[n++] = s->executable;
    argv[n++] = move ? "move" : "copy";
    argv[n++] = source;
    argv[n++] = destination;
    argv[n++] = "--config";
    argv[n++] = s->config_path;
    argv[n++] = "--transfers";
    argv[n++] = "1";
    argv[n++] = "--checkers";
    argv[n++] = "2";
    argv[n++] = "--stats";
    argv[n++] = "5s";
    argv[n++] = "--use-json-log";
    argv[n++] = "--stats-log-level";
    argv[n++] = "NOTICE";
    argv[n++] = "--exclude";
    argv[n++] = "*.part";
    argv[n++] = "--exclude";
    argv[n++] = "*.tmp";
    if (s->bandwidth_kib_s) {
        argv[n++] = "--bwlimit";
        argv[n++] = bandwidth;
    }
    argv[n] = NULL;

    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) {
        fail_run(u, now, "cannot create a pipe for upload worker");
        return false;
    }
    /* The child keeps the write end as stderr, so keep it clear of 0-2. */
    if (pipes[1] < 3) {
        int moved = fcntl(pipes[1], F_DUPFD_CLOEXEC, 3);
        close(pipes[1]);
        pipes[1] = moved;
    }
    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max < 0 || open_max > 65536) open_max = 1024;
    const pid_t parent = getpid();
    if (pipes[1] < 0 || fcntl(pipes[0], F_SETFL, O_NONBLOCK) != 0) {
        close(pipes[0]);
        if (pipes[1] >= 0) close(pipes[1]);
        fail_run(u, now, "cannot set up the pipe for upload worker");
        return false;
    }
    const pid_t child = fork();
    if (child == 0) {
        /* Only async-signal-safe calls between fork and exec in this
         * threaded application. */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGTERM, SIG_DFL); /* a vendor library may have ignored it */
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        if (getppid() != parent) _exit(127);
        setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
        syscall(SYS_ioprio_set, 1 /* process */, 0, 3 << 13 /* idle class */);
#endif
        int null = open("/dev/null", O_RDWR);
        if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(pipes[1], 2) < 0) _exit(127);
        for (int fd = 3; fd < open_max; fd++) close(fd);
        execv(argv[0], const_cast<char *const *>(argv));
        _exit(127);
    }
    close(pipes[1]);
    if (child < 0) {
        close(pipes[0]);
        fail_run(u, now, "cannot start upload worker");
        return false;
    }
    u->pid = child;
    u->fd = pipes[0];
    u->line_length = 0;
    u->line_overflow = false;
    u->job_bytes = u->job_total = 0;
    u->job_files = u->job_files_total = 0;
    u->job_error[0] = '\0';
    u->killed = false;
    u->status.state = CA_UPLOAD_RUNNING;
    update_progress(u);
    ca_log("upload: worker %s %s -> %s", move ? "move" : "copy", source, destination);
    return true;
}

static void complete_scan(struct ca_uploader *u, uint64_t now)
{
    unsigned minutes = u->settings.scan_minutes ? u->settings.scan_minutes : 1U;
    u->status.last_success = time(NULL);
    u->status.last_error[0] = '\0';
    u->backoff_ms = 0;
    u->next_scan = now + (uint64_t)minutes * 60000U;
    u->status.state = CA_UPLOAD_WAITING;
}

static void start_scan(struct ca_uploader *u, uint64_t now)
{
    const struct ca_upload_settings *s = &u->settings;
    u->job_count = u->job_index = 0;
    if ((s->what & CA_UPLOAD_PHOTOS) && is_directory(s->capture_root)) u->jobs[u->job_count++] = JOB_PHOTOS;
    if ((s->what & CA_UPLOAD_RECORDINGS) && is_directory(s->record_root)) u->jobs[u->job_count++] = JOB_RECORDINGS;
    u->base_bytes = u->base_total = 0;
    u->base_files = u->base_files_total = 0;
    u->job_bytes = u->job_total = 0;
    u->job_files = u->job_files_total = 0;
    update_progress(u);
    if (u->job_count == 0) complete_scan(u, now); /* nothing to look at */
    else (void)start_job(u, now);
}

/* The job list is made when a scan starts, so the settings may have changed
 * since: a job runs only if they still ask for its kind. */
static bool job_wanted(const struct ca_uploader *u)
{
    const unsigned wanted = u->settings.what;
    return (u->jobs[u->job_index] == JOB_PHOTOS ? (wanted & CA_UPLOAD_PHOTOS) : (wanted & CA_UPLOAD_RECORDINGS)) != 0;
}

/* The child has exited. */
static void finish_job(struct ca_uploader *u, uint64_t now, int wait_status, bool may_continue)
{
    drain(u);
    close(u->fd);
    u->fd = -1;
    u->pid = -1;
    if (u->status.state == CA_UPLOAD_STOPPING) {
        /* Asked to stop: not a failure. Look again when uploads may run. */
        u->base_bytes = u->base_total = u->job_bytes = u->job_total = 0;
        u->base_files = u->base_files_total = u->job_files = u->job_files_total = 0;
        update_progress(u);
        u->next_scan = now;
        u->status.state = CA_UPLOAD_WAITING;
        ca_log("upload: stopped");
        return;
    }
    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
        u->base_bytes += u->job_bytes;
        u->base_total += u->job_total;
        u->base_files += u->job_files;
        u->base_files_total += u->job_files_total;
        u->job_bytes = u->job_total = 0;
        u->job_files = u->job_files_total = 0;
        update_progress(u);
        do u->job_index++;
        while (u->job_index < u->job_count && !job_wanted(u));
        if (u->job_index < u->job_count) {
            if (may_continue) (void)start_job(u, now);
            else { u->next_scan = now; u->status.state = CA_UPLOAD_WAITING; }
            return;
        }
        complete_scan(u, now);
        ca_log("upload: finished, %u files", u->status.files_done);
        return;
    }
    char error[sizeof(u->status.last_error)];
    if (u->job_error[0]) set_text(error, sizeof(error), u->job_error);
    else if (WIFEXITED(wait_status)) snprintf(error, sizeof(error), "Upload worker exited with code %d", WEXITSTATUS(wait_status));
    else snprintf(error, sizeof(error), "Upload worker killed by signal %d", WTERMSIG(wait_status));
    fail_run(u, now, error);
}

static void service_child(struct ca_uploader *u, uint64_t now, bool want_stop, bool may_continue)
{
    drain(u);
    int wait_status = 0;
    const pid_t done = waitpid(u->pid, &wait_status, WNOHANG);
    if (done == u->pid) {
        finish_job(u, now, wait_status, may_continue);
        return;
    }
    if (done < 0 && errno != EINTR) { /* already gone */
        finish_job(u, now, 0, may_continue);
        return;
    }
    /* A stop already asked for runs its course, even if uploads may run again. */
    if (!want_stop && u->status.state != CA_UPLOAD_STOPPING) return;
    if (u->status.state != CA_UPLOAD_STOPPING) {
        kill(u->pid, SIGTERM);
        u->status.state = CA_UPLOAD_STOPPING;
        u->kill_at = now + STOP_GRACE_MS;
        u->killed = false;
    } else if (!u->killed && now >= u->kill_at) {
        kill(u->pid, SIGKILL);
        u->killed = true;
        ca_log("upload: worker did not stop; killed");
    }
}

/* ---- public interface ---- */

struct ca_uploader *ca_uploader_create(void)
{
    struct ca_uploader *u = (struct ca_uploader *)calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->pid = -1;
    u->fd = -1;
    u->status.state = CA_UPLOAD_DISABLED;
    set_text(u->status.reason, sizeof(u->status.reason), "Disabled");
    return u;
}

void ca_uploader_destroy(struct ca_uploader *u)
{
    if (!u) return;
    if (u->pid > 0) {
        kill(u->pid, SIGTERM);
        bool gone = false;
        for (int i = 0; i < 80 && !gone; i++) {
            int wait_status;
            if (waitpid(u->pid, &wait_status, WNOHANG) != 0) gone = true;
            else {
                if (i == 40) kill(u->pid, SIGKILL);
                usleep(25000);
            }
        }
    }
    if (u->fd >= 0) close(u->fd);
    free(u);
}

void ca_uploader_configure(struct ca_uploader *u, const struct ca_upload_settings *settings)
{
    if (u && settings) u->settings = *settings;
}

void ca_uploader_periodic(struct ca_uploader *u, uint64_t now, bool recording, bool armed)
{
    if (!u) return;
    const bool allowed = !recording && !armed;
    if (u->blocked && allowed) u->next_scan = now; /* recording ended or disarmed: look for new files */
    u->blocked = !allowed;

    char reason[sizeof(u->status.reason)];
    const bool usable = settings_usable(&u->settings, reason, sizeof(reason));
    if (u->pid > 0) {
        service_child(u, now, !usable || !allowed, usable && allowed);
        if (u->pid > 0) return;
    }
    if (!usable) {
        set_disabled(u, reason);
        return;
    }
    if (u->status.state == CA_UPLOAD_DISABLED) {
        u->status.reason[0] = '\0';
        u->status.state = CA_UPLOAD_WAITING;
        u->next_scan = now;
    }
    if (!allowed) return;
    if ((u->status.state == CA_UPLOAD_WAITING && now >= u->next_scan) ||
        (u->status.state == CA_UPLOAD_BACKOFF && now >= u->retry_at))
        start_scan(u, now);
}

const struct ca_upload_status *ca_uploader_status(const struct ca_uploader *u)
{
    return &u->status;
}

size_t ca_uploader_format_status(const struct ca_upload_status *status, char *out, size_t size)
{
    static const char *const names[] = {"disabled", "waiting", "running", "stopping", "backoff"};
    int n = snprintf(out, size,
                     "state=%s\nreason=%s\nlast_success=%lld\nfiles_done=%u\nfiles_total=%u\n"
                     "bytes_done=%llu\nbytes_total=%llu\nlast_error=%s\n",
                     names[status->state], status->reason, (long long)status->last_success,
                     status->files_done, status->files_total,
                     (unsigned long long)status->bytes_done, (unsigned long long)status->bytes_total,
                     status->last_error);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}
