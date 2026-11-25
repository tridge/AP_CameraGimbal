#ifndef CAMERA_APP_UPLOADER_H
#define CAMERA_APP_UPLOADER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Copies photos and recordings to a cloud remote with a small native HTTPS worker as a child
 * process. It never blocks: call ca_uploader_periodic() from the main loop.
 * Uploads run only while the camera is not recording and the vehicle is not
 * armed, so every file present while the worker runs is finished. */

enum ca_upload_state {
    CA_UPLOAD_DISABLED, /* off, or the settings are not usable (see reason) */
    CA_UPLOAD_WAITING,  /* idle: recording, armed, or waiting for the next scan */
    CA_UPLOAD_RUNNING,
    CA_UPLOAD_STOPPING, /* asked the worker to stop; waiting for it to exit */
    CA_UPLOAD_BACKOFF   /* the last run failed; waiting before retrying */
};

enum ca_upload_after { CA_UPLOAD_KEEP, CA_UPLOAD_DELETE, CA_UPLOAD_DELETE_WHEN_LOW };

#define CA_UPLOAD_PHOTOS 1U
#define CA_UPLOAD_RECORDINGS 2U

struct ca_upload_settings {
    bool enable;
    char executable[256];   /* native worker; compatible external worker may be supplied */
    char config_path[256];  /* Drive credentials in the established INI format */
    char remote[160];       /* "name:folder"; must contain a colon */
    char capture_root[256]; /* photos, sent to <remote>/photos */
    char record_root[256];  /* recordings, sent to <remote>/recordings */
    unsigned what;          /* CA_UPLOAD_PHOTOS | CA_UPLOAD_RECORDINGS */
    enum ca_upload_after after_upload;
    unsigned low_space_percent; /* DELETE_WHEN_LOW: a scan that starts below this free space deletes what it uploads */
    unsigned bandwidth_kib_s;   /* KiB/s; 0 = unlimited */
    unsigned scan_minutes;      /* idle time between scans, at least 1 */
};

struct ca_upload_status {
    enum ca_upload_state state;
    char reason[96];     /* why DISABLED */
    time_t last_success; /* wall clock; 0 = none yet */
    uint64_t bytes_done, bytes_total;
    unsigned files_done, files_total;
    char last_error[160];
};

struct ca_uploader;

struct ca_uploader *ca_uploader_create(void);
/* Stops a running transfer (briefly blocking) and frees everything. */
void ca_uploader_destroy(struct ca_uploader *uploader);
/* Takes effect the next time the worker starts, except that disabling stops it. */
void ca_uploader_configure(struct ca_uploader *uploader, const struct ca_upload_settings *settings);
/* now_ms is any monotonic millisecond clock. */
void ca_uploader_periodic(struct ca_uploader *uploader, uint64_t now_ms, bool recording, bool armed);
const struct ca_upload_status *ca_uploader_status(const struct ca_uploader *uploader);
/* The status as key=value lines for the web page, for example
 * "state=running\nfiles_done=1\n". It never contains credentials. Returns the
 * length, or 0 if size is too small. */
size_t ca_uploader_format_status(const struct ca_upload_status *status, char *out, size_t size);
#endif
