/* The cloud upload supervisor against a fake rclone (tests/fake_rclone.sh):
 * no network or account is needed. Time is passed in, so waiting periods are
 * stepped over by changing "now"; only the fake's own run time is real. */
#include "camera_app/uploader.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

static char g_dir[PATH_MAX], g_script[PATH_MAX], g_log[PATH_MAX + 16], g_pid[PATH_MAX + 16];

/* Bounded copy, so a long path cannot overflow or draw a truncation warning. */
static void put(char *to, size_t size, const char *from)
{
    size_t length = strlen(from);
    if (length >= size) length = size - 1;
    memcpy(to, from, length);
    to[length] = '\0';
}

static void under(char *to, size_t size, const char *directory, const char *name)
{
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    put(to, size, path);
}

static void make_directory(const char *name)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

static void remove_directory(const char *name)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    assert(rmdir(path) == 0);
}

static struct ca_upload_settings good_settings(void)
{
    struct ca_upload_settings s = {};
    s.enable = true;
    put(s.executable, sizeof(s.executable), g_script);
    under(s.config_path, sizeof(s.config_path), g_dir, "rclone.conf");
    put(s.remote, sizeof(s.remote), "fake:cam");
    under(s.capture_root, sizeof(s.capture_root), g_dir, "capture");
    under(s.record_root, sizeof(s.record_root), g_dir, "record");
    s.what = CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS;
    s.after_upload = CA_UPLOAD_KEEP;
    s.low_space_percent = 20;
    s.scan_minutes = 5;
    return s;
}

static std::vector<std::string> runs(void)
{
    std::vector<std::string> lines;
    std::ifstream file(g_log);
    for (std::string line; std::getline(file, line);) lines.push_back(line);
    return lines;
}

/* The fake writes its line a moment after it is forked. */
static bool wait_for_runs(size_t count)
{
    for (int i = 0; i < 1000; i++) {
        if (runs().size() >= count) return runs().size() == count;
        usleep(5000);
    }
    return false;
}

static void reset_log(void)
{
    assert(truncate(g_log, 0) == 0 || errno == ENOENT);
    unlink(g_pid);
}

static bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

static struct ca_uploader *start(const struct ca_upload_settings &settings, const char *mode)
{
    setenv("FAKE_RCLONE_MODE", mode, 1);
    reset_log();
    struct ca_uploader *u = ca_uploader_create();
    assert(u);
    ca_uploader_configure(u, &settings);
    return u;
}

/* Calls periodic until the state is reached; the fake takes a little real time to run. */
static void wait_for(struct ca_uploader *u, enum ca_upload_state state, uint64_t now,
                     bool recording = false, bool armed = false)
{
    for (int i = 0; i < 1000; i++) {
        ca_uploader_periodic(u, now, recording, armed);
        if (ca_uploader_status(u)->state == state) return;
        usleep(5000);
    }
    fprintf(stderr, "stuck in state %d, wanted %d\n", ca_uploader_status(u)->state, state);
    assert(false);
}

/* The fake has printed its progress, so it is past its signal setup. */
static void wait_for_progress(struct ca_uploader *u, uint64_t now)
{
    for (int i = 0; i < 1000; i++) {
        ca_uploader_periodic(u, now, false, false);
        if (ca_uploader_status(u)->bytes_total > 0) return;
        usleep(5000);
    }
    assert(false);
}

static pid_t fake_pid(void)
{
    std::ifstream file(g_pid);
    long pid = 0;
    file >> pid;
    assert(pid > 0);
    return (pid_t)pid;
}

static bool process_gone(pid_t pid)
{
    return kill(pid, 0) != 0 && errno == ESRCH;
}

static void unusable_settings(void)
{
    struct ca_upload_settings off = good_settings();
    off.enable = false;
    struct ca_upload_settings missing = good_settings();
    under(missing.executable, sizeof(missing.executable), g_dir, "nonexistent");
    struct ca_upload_settings colon = good_settings(), dash = good_settings(), empty = good_settings();
    struct ca_upload_settings nothing = good_settings(), config = good_settings();
    put(colon.remote, sizeof(colon.remote), "nocolon");
    put(dash.remote, sizeof(dash.remote), "-x:y");
    empty.remote[0] = '\0';
    nothing.what = 0;
    config.config_path[0] = '\0';
    const struct { const struct ca_upload_settings *settings; const char *reason; } cases[] = {
        {&off, "Disabled"}, {&missing, "not found"}, {&colon, "name:folder"}, {&dash, "name:folder"},
        {&empty, "name:folder"}, {&nothing, "Nothing selected"}, {&config, "credentials"},
    };
    for (const auto &c : cases) {
        struct ca_uploader *u = start(*c.settings, "ok");
        ca_uploader_periodic(u, 1000, false, false);
        usleep(50000);
        ca_uploader_periodic(u, 1000, false, false);
        const struct ca_upload_status *status = ca_uploader_status(u);
        assert(status->state == CA_UPLOAD_DISABLED);
        assert(has(status->reason, c.reason));
        assert(runs().empty());
        ca_uploader_destroy(u);
    }
}

static void full_scan(void)
{
    struct ca_upload_settings s = good_settings();
    s.bandwidth_kib_s = 100;
    struct ca_uploader *u = start(s, "ok");
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now);

    std::vector<std::string> lines = runs();
    assert(lines.size() == 2);
    assert(lines[0].compare(0, 5, "copy ") == 0);
    assert(has(lines[0], "/capture fake:cam/photos "));
    assert(has(lines[1], "/record fake:cam/recordings "));
    for (const std::string &line : lines) {
        assert(has(line, "--config "));
        assert(has(line, "--transfers 1"));
        assert(has(line, "--use-json-log"));
        assert(has(line, "--exclude *.part --exclude *.tmp"));
        assert(has(line, "--bwlimit 100k"));
    }
    const struct ca_upload_status *status = ca_uploader_status(u);
    assert(status->last_success != 0 && status->last_error[0] == '\0');
    /* Two runs of 1 file / 2048 bytes done out of 2 files / 4096 bytes. */
    assert(status->files_done == 2 && status->files_total == 4);
    assert(status->bytes_done == 4096 && status->bytes_total == 8192);

    /* Nothing more until the next scan is due, five minutes later. */
    ca_uploader_periodic(u, now + 299999, false, false);
    assert(runs().size() == 2 && ca_uploader_status(u)->state == CA_UPLOAD_WAITING);
    now += 300000;
    ca_uploader_periodic(u, now, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(runs().size() == 4);
    ca_uploader_destroy(u);
}

static void gate(void)
{
    struct ca_uploader *u = start(good_settings(), "ok");
    uint64_t now = 1000;
    /* Recording, or armed, or both: nothing runs. */
    for (int i = 0; i < 3; i++) {
        ca_uploader_periodic(u, now, i != 1, i != 0);
        usleep(20000);
        ca_uploader_periodic(u, now, i != 1, i != 0);
        assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && runs().empty());
    }
    wait_for(u, CA_UPLOAD_WAITING, now); /* gate open: the scan runs and ends */
    assert(runs().size() == 2);

    /* The scan is not due for five minutes, but recording ending looks again. */
    ca_uploader_periodic(u, now + 1000, true, false);
    ca_uploader_periodic(u, now + 2000, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now + 2000);
    assert(runs().size() == 4);
    /* So does disarming. */
    ca_uploader_periodic(u, now + 3000, false, true);
    ca_uploader_periodic(u, now + 4000, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    wait_for(u, CA_UPLOAD_WAITING, now + 4000);
    assert(runs().size() == 6);
    ca_uploader_destroy(u);
}

static void stops_when_recording_starts(void)
{
    struct ca_uploader *u = start(good_settings(), "slow");
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false, false);
    wait_for_progress(u, now);
    const pid_t pid = fake_pid();
    assert(!process_gone(pid));

    ca_uploader_periodic(u, now, true, false); /* recording starts */
    const enum ca_upload_state state = ca_uploader_status(u)->state;
    assert(state == CA_UPLOAD_STOPPING || state == CA_UPLOAD_WAITING);
    wait_for(u, CA_UPLOAD_WAITING, now, true, false);
    assert(process_gone(pid));
    /* Stopping on request is not a failure, and nothing restarts while recording. */
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success == 0);
    ca_uploader_periodic(u, now + 600000, true, false);
    assert(runs().size() == 1);

    /* Recording ends: the interrupted work starts again. */
    ca_uploader_periodic(u, now + 600001, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING && wait_for_runs(2));
    ca_uploader_destroy(u);
}

static void kills_a_child_that_ignores_sigterm(void)
{
    struct ca_uploader *u = start(good_settings(), "hang");
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false, false);
    wait_for_progress(u, now);
    const pid_t pid = fake_pid();

    ca_uploader_periodic(u, now, false, true); /* armed */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING);
    usleep(100000);
    ca_uploader_periodic(u, now + 1999, false, true); /* grace period not over */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING && !process_gone(pid));
    now += 2000;
    wait_for(u, CA_UPLOAD_WAITING, now, false, true);
    assert(process_gone(pid));
    ca_uploader_destroy(u);
}

/* The gate reopening while rclone is being stopped must not cancel the SIGKILL. */
static void kills_after_the_gate_reopens(void)
{
    struct ca_uploader *u = start(good_settings(), "hang");
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false, false);
    wait_for_progress(u, now);
    const pid_t pid = fake_pid();

    ca_uploader_periodic(u, now, true, false); /* recording starts: asked to stop */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING);
    usleep(100000);
    ca_uploader_periodic(u, now + 100, false, false); /* recording ends again */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_STOPPING && !process_gone(pid));
    now += 2000;
    for (int i = 0; i < 1000 && !process_gone(pid); i++) {
        ca_uploader_periodic(u, now, false, false);
        usleep(5000);
    }
    assert(process_gone(pid)); /* killed, not left stuck in STOPPING */
    ca_uploader_destroy(u);
}

static void backoff(void)
{
    struct ca_uploader *u = start(good_settings(), "fail");
    uint64_t now = 1000;
    ca_uploader_periodic(u, now, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    assert(runs().size() == 1);
    assert(has(ca_uploader_status(u)->last_error, "Failed to copy: quota \"exceeded\""));
    assert(ca_uploader_status(u)->last_success == 0);

    /* 30 s, then 60 s, then 120 s, which is the longest wait. */
    ca_uploader_periodic(u, now + 29999, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_BACKOFF && runs().size() == 1);
    now += 30000;
    ca_uploader_periodic(u, now, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING && wait_for_runs(2));
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 59999, false, false);
    assert(runs().size() == 2);
    now += 60000;
    ca_uploader_periodic(u, now, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 119999, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_BACKOFF && runs().size() == 3);
    now += 120000;
    ca_uploader_periodic(u, now, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 119999, false, false);
    assert(runs().size() == 4); /* still 2 minutes, not longer */
    now += 120000;

    /* A success clears the error and the wait. */
    setenv("FAKE_RCLONE_MODE", "ok", 1);
    ca_uploader_periodic(u, now, false, false);
    wait_for(u, CA_UPLOAD_WAITING, now);
    assert(ca_uploader_status(u)->last_error[0] == '\0' && ca_uploader_status(u)->last_success != 0);
    const size_t before = runs().size();
    setenv("FAKE_RCLONE_MODE", "fail", 1);
    now += 300000;
    ca_uploader_periodic(u, now, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, now);
    ca_uploader_periodic(u, now + 30000, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING && wait_for_runs(before + 2));
    ca_uploader_destroy(u);
}

static void status_text(void)
{
    struct ca_upload_status status = {};
    status.state = CA_UPLOAD_RUNNING;
    status.last_success = 1730000000;
    status.files_done = 1;
    status.files_total = 4;
    status.bytes_done = 2048;
    status.bytes_total = 8192;
    snprintf(status.last_error, sizeof(status.last_error), "quota exceeded");
    char text[512];
    assert(ca_uploader_format_status(&status, text, sizeof(text)) > 0);
    assert(!strcmp(text, "state=running\nreason=\nlast_success=1730000000\nfiles_done=1\nfiles_total=4\n"
                         "bytes_done=2048\nbytes_total=8192\nlast_error=quota exceeded\n"));
    assert(ca_uploader_format_status(&status, text, 20) == 0); /* too small */
}

static void unknown_remote(void)
{
    /* rclone reports a bad remote as a critical line with exit code 1. */
    struct ca_uploader *u = start(good_settings(), "critical");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, 1000);
    assert(has(ca_uploader_status(u)->last_error, "didn't find section in config file"));
    ca_uploader_destroy(u);
}

static void source_folders(void)
{
    struct ca_upload_settings s = good_settings();
    make_directory("capture");
    make_directory("record");

    /* A folder that does not exist is skipped, not an error. */
    remove_directory("record");
    struct ca_uploader *u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(runs().size() == 1 && has(runs()[0], "fake:cam/photos"));
    assert(ca_uploader_status(u)->last_error[0] == '\0');
    ca_uploader_destroy(u);

    remove_directory("capture");
    u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_WAITING && ca_uploader_status(u)->last_success != 0);
    assert(runs().empty());
    ca_uploader_destroy(u);
    make_directory("capture");
    make_directory("record");

    /* The choice of what to upload. */
    s.what = CA_UPLOAD_RECORDINGS;
    u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(runs().size() == 1 && has(runs()[0], "fake:cam/recordings"));
    ca_uploader_destroy(u);

    /* A remote that names only the drive. */
    s = good_settings();
    put(s.remote, sizeof(s.remote), "fake:");
    u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(has(runs()[0], " fake:photos ") && has(runs()[1], " fake:recordings "));
    ca_uploader_destroy(u);
}

/* Settings saved while a scan runs: a queued job whose kind is no longer wanted
 * must not start (with delete_after it would move that whole folder). */
static void settings_change_during_a_scan(void)
{
    struct ca_upload_settings s = good_settings();
    s.after_upload = CA_UPLOAD_DELETE;
    struct ca_uploader *u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false); /* the photos job starts */
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    s.what = CA_UPLOAD_PHOTOS; /* recordings no longer wanted */
    ca_uploader_configure(u, &s);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(runs().size() == 1 && has(runs()[0], "/capture ") && has(runs()[0], "move "));
    ca_uploader_destroy(u);

    /* The other way round: the queued recordings job is still wanted and runs. */
    s = good_settings();
    u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    s.what = CA_UPLOAD_RECORDINGS;
    ca_uploader_configure(u, &s);
    wait_for(u, CA_UPLOAD_WAITING, 1000);
    assert(runs().size() == 2 && has(runs()[1], "/record "));
    ca_uploader_destroy(u);
}

static void after_upload(void)
{
    const struct { enum ca_upload_after mode; unsigned percent; const char *command; } cases[] = {
        {CA_UPLOAD_KEEP, 20, "copy "},
        {CA_UPLOAD_DELETE, 20, "move "},
        {CA_UPLOAD_DELETE_WHEN_LOW, 100, "move "}, /* free space is below 100% */
        {CA_UPLOAD_DELETE_WHEN_LOW, 0, "copy "},   /* never below 0% */
    };
    for (const auto &c : cases) {
        struct ca_upload_settings s = good_settings();
        s.after_upload = c.mode;
        s.low_space_percent = c.percent;
        struct ca_uploader *u = start(s, "ok");
        ca_uploader_periodic(u, 1000, false, false);
        wait_for(u, CA_UPLOAD_WAITING, 1000);
        assert(runs().size() == 2);
        for (const std::string &line : runs()) assert(line.compare(0, 5, c.command) == 0);
        ca_uploader_destroy(u);
    }
}

static void destroy_and_disable_stop_the_child(void)
{
    struct ca_uploader *u = start(good_settings(), "slow");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for_progress(u, 1000);
    pid_t pid = fake_pid();
    ca_uploader_destroy(u);
    assert(process_gone(pid));

    u = start(good_settings(), "slow");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for_progress(u, 1000);
    pid = fake_pid();
    struct ca_upload_settings off = good_settings();
    off.enable = false;
    ca_uploader_configure(u, &off);
    wait_for(u, CA_UPLOAD_DISABLED, 1000);
    assert(process_gone(pid));
    /* Turned on again, it starts at once. */
    struct ca_upload_settings on = good_settings();
    ca_uploader_configure(u, &on);
    ca_uploader_periodic(u, 2000, false, false);
    assert(ca_uploader_status(u)->state == CA_UPLOAD_RUNNING);
    ca_uploader_destroy(u);
}

static void missing_executable_after_start(void)
{
    /* rclone not runnable at the moment it is needed: a failure, not a crash. */
    char fake[PATH_MAX + 32];
    snprintf(fake, sizeof(fake), "%s/not-a-program", g_dir);
    FILE *file = fopen(fake, "w");
    assert(file);
    fputs("this is not a program\n", file);
    fclose(file);
    assert(chmod(fake, 0755) == 0);
    struct ca_upload_settings s = good_settings();
    put(s.executable, sizeof(s.executable), fake);
    struct ca_uploader *u = start(s, "ok");
    ca_uploader_periodic(u, 1000, false, false);
    wait_for(u, CA_UPLOAD_BACKOFF, 1000);
    assert(has(ca_uploader_status(u)->last_error, "code 127"));
    ca_uploader_destroy(u);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s/uploader-test-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(mkdtemp(pattern));
    put(g_dir, sizeof(g_dir), pattern);
    char *script = realpath("tests/fake_rclone.sh", NULL);
    assert(script);
    snprintf(g_script, sizeof(g_script), "%s", script);
    free(script);
    snprintf(g_log, sizeof(g_log), "%s/runs.log", g_dir);
    snprintf(g_pid, sizeof(g_pid), "%s/fake.pid", g_dir);
    setenv("FAKE_RCLONE_LOG", g_log, 1);
    setenv("FAKE_RCLONE_PID", g_pid, 1);
    make_directory("capture");
    make_directory("record");

    unusable_settings();
    full_scan();
    gate();
    stops_when_recording_starts();
    kills_a_child_that_ignores_sigterm();
    kills_after_the_gate_reopens();
    backoff();
    status_text();
    unknown_remote();
    source_folders();
    settings_change_during_a_scan();
    after_upload();
    destroy_and_disable_stop_the_child();
    missing_executable_after_start();

    char command[PATH_MAX + 32];
    snprintf(command, sizeof(command), "rm -rf '%s'", g_dir);
    if (system(command) != 0) return 1;
    printf("PASS uploader: gate, stop and kill, backoff, source folders, copy/move and cleanup\n");
    return 0;
}
