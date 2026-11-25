#pragma once
// Cloud upload on the Parameters page, included by mt11-web.cpp where the file,
// form and translation helpers it uses are already defined.
#include "APC_StringBuffer.h"
#include "../include/apcam/upload_paths.h"

#if APCAM_HAVE_UPLOAD
/* Cloud upload: the camera app supervises a native HTTPS worker. This page
 * shows its status and stores the user's own Google Drive credentials in the
 * rclone config file, which only the camera software can read. */
#define UPLOAD_REMOTE_NAME "gdrive"
// Vendor libcs do not all implement explicit_bzero. Volatile stores preserve
// the credential wipe without introducing a libc version dependency.
static void upload_zero(void *memory,size_t length)
{
    volatile unsigned char *p=static_cast<volatile unsigned char *>(memory);
    while(length--) *p++=0;
}

static const char *upload_rclone_config_path(void)
{
    const char *path = getenv("CAMERA_APP_RCLONE_CONFIG");
    if(path && *path) return path;
    if(APCAM_RCLONE_CONFIG[0]) return APCAM_RCLONE_CONFIG;
    static char native_path[4096];
    if(!apcam_upload_sibling(native_path,sizeof(native_path),REPLACEMENT_CONFIG_PATH,"upload.conf")) return "";
    return native_path;
}

static bool upload_status_value(const char *text, const char *key, char *out, size_t size)
{
    const size_t key_length = strlen(key);
    for (const char *line = text; line != NULL && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, key_length) == 0 && line[key_length] == '=') {
            size_t length = strcspn(line + key_length + 1, "\n");
            if (length >= size) length = size - 1;
            memcpy(out, line + key_length + 1, length);
            out[length] = '\0';
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

static bool camera_app_running(void)
{
    size_t size;
    char *ready = read_file(CAMERA_READY_PATH, 512, &size);
    const char *pid_line = ready != NULL ? strstr(ready, "\npid=") : NULL;
    long pid = 0;
    const bool running = pid_line != NULL && sscanf(pid_line, "\npid=%ld", &pid) == 1 &&
                         pid > 1 && kill((pid_t)pid, 0) == 0;
    free(ready);
    return running;
}

static void append_upload_status(APC_StringBuffer *page)
{
    char path[sizeof(CAMERA_READY_PATH) + 8], state[16], reason[128], success[24], error[192];
    char done[16], total[16];
    size_t size;
    snprintf(path, sizeof(path), "%s.upload", CAMERA_READY_PATH);
    char *text = camera_app_running() ? read_file(path, 2048, &size) : NULL;
    page->appendf("<h3>%s</h3>", T(S_UPLOAD_STATUS));
    if (text == NULL || !upload_status_value(text, "state", state, sizeof(state))) {
        page->append("<p class=help id=upload-status>");
        page->append_html(T(S_UPLOAD_NO_STATUS));
        page->append("</p>");
        free(text);
        return;
    }
    upload_status_value(text, "reason", reason, sizeof(reason));
    upload_status_value(text, "last_success", success, sizeof(success));
    upload_status_value(text, "last_error", error, sizeof(error));
    upload_status_value(text, "files_done", done, sizeof(done));
    upload_status_value(text, "files_total", total, sizeof(total));
    const struct { const char *name; enum string_id label; } states[] = {
        {"disabled", S_UPLOAD_STATE_DISABLED}, {"waiting", S_UPLOAD_STATE_WAITING},
        {"running", S_UPLOAD_STATE_RUNNING}, {"stopping", S_UPLOAD_STATE_STOPPING},
        {"backoff", S_UPLOAD_STATE_BACKOFF},
    };
    page->append("<p class=notice id=upload-status>");
    page->appendf("%s: ", T(S_UPLOAD_STATE));
    for (const auto &entry : states) {
        if (!strcmp(state, entry.name)) page->append_html(T(entry.label));
    }
    if (!strcmp(state, "disabled") && reason[0] && strcmp(reason, "Disabled") != 0) {
        page->append(" (");
        page->append_html(reason);
        page->append(")");
    }
    time_t when = (time_t)atoll(success);
    char when_text[40] = "";
    struct tm local;
    if (when > 0 && localtime_r(&when, &local) != NULL) strftime(when_text, sizeof(when_text), "%Y-%m-%d %H:%M:%S", &local);
    page->appendf("<br>%s: ", T(S_UPLOAD_LAST_SUCCESS));
    page->append_html(when_text[0] ? when_text : T(S_UPLOAD_NEVER));
    page->appendf("<br>%s: %s / %s", T(S_UPLOAD_FILES), done, total);
    if (error[0]) {
        page->appendf("<br>%s: ", T(S_UPLOAD_LAST_ERROR));
        page->append_html(error);
    }
    page->append("</p>");
    free(text);
}

static bool upload_credentials_saved(void)
{
    size_t size;
    char *config = read_file(upload_rclone_config_path(), 65536, &size);
    const bool saved = config != NULL &&
        (strncmp(config, "[" UPLOAD_REMOTE_NAME "]", sizeof("[" UPLOAD_REMOTE_NAME "]") - 1) == 0 ||
         strstr(config, "\n[" UPLOAD_REMOTE_NAME "]") != NULL);
    free(config);
    return saved;
}

/* The inputs belong to the separate upload-credentials form, which sits
 * before the parameters form because forms cannot be nested. */
static void append_upload_credentials(APC_StringBuffer *page)
{
    page->appendf("<h3>%s</h3><p class=help>", T(S_UPLOAD_CRED_HEADING));
    page->append_html(T(S_UPLOAD_CRED_HELP));
    page->append("</p><p class=notice id=upload-credentials-status>");
    page->append_html(T(upload_credentials_saved() ? S_UPLOAD_CRED_SAVED_FOR : S_UPLOAD_CRED_NONE));
    page->appendf("</p><p><label>%s<br><input form=upload-credentials name=client_id size=60 maxlength=200 "
                  "autocomplete=off spellcheck=false></label></p>", T(S_UPLOAD_CRED_ID));
    page->appendf("<p><label>%s<br><input form=upload-credentials name=client_secret type=password size=60 "
                  "maxlength=200 autocomplete=off></label></p>", T(S_UPLOAD_CRED_SECRET));
    page->appendf("<p><label>%s<br><textarea form=upload-credentials name=token rows=4 cols=60 maxlength=4096 "
                  "autocomplete=off spellcheck=false></textarea></label></p>", T(S_UPLOAD_CRED_TOKEN));
    page->appendf("<p><button type=submit form=upload-credentials name=action value=save>%s</button> "
                  "<button type=submit form=upload-credentials name=action value=remove formnovalidate>%s</button></p>",
                  T(S_UPLOAD_CRED_SAVE), T(S_UPLOAD_CRED_REMOVE));
}

static bool upload_credential_valid(const char *text, size_t length, bool token)
{
    if (token) {
        if (length < 2 || length > 4096 || text[0] != '{' || text[length - 1] != '}' ||
            strstr(text, "\"access_token\"") == NULL) return false;
        for (size_t i = 0; i < length; i++) {
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return false;
        }
        return true;
    }
    return length >= 1 && length <= 200 &&
           strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._~-") == length;
}

/* Replace (or with a NULL section remove) one section of the rclone config,
 * keeping any others. The file is written 0600 and swapped in atomically. */
static bool upload_replace_section(const char *path, const char *name, const char *section,
                                   char *error, size_t error_size)
{
    size_t size = 0;
    char *old = read_file(path, 65536, &size);
    if (old == NULL && errno != ENOENT) {
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(errno));
        return false;
    }
    if (old == NULL) old = strdup("");
    char header[64], temporary[PATH_MAX];
    snprintf(header, sizeof(header), "[%s]", name);
    APC_StringBuffer out;
    out.reset();
    bool skipping = false;
    for (const char *line = old; *line;) {
        const size_t length = strcspn(line, "\n");
        if (line[0] == '[') skipping = strncmp(line, header, strlen(header)) == 0;
        if (!skipping) {
            out.append_n(line, length);
            out.append("\n");
        }
        line += length;
        if (*line == '\n') line++;
    }
    if (section != NULL) out.append(section);
    if (!out.valid()) {
        free(old);
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(ENOMEM));
        return false;
    }
    bool ok = false;
    int fd = -1;
    if (snprintf(temporary, sizeof(temporary), "%s.web.new.%ld", path, (long)getpid()) < (int)sizeof(temporary)) {
        (void)unlink(temporary);
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        ok = fd >= 0 && write_all(fd, out.data(), out.size()) && fsync(fd) == 0 && close(fd) == 0 &&
             rename(temporary, path) == 0;
        if (!ok) {
            snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(errno));
            if (fd >= 0) close(fd);
            (void)unlink(temporary);
        }
    } else {
        snprintf(error, error_size, T(S_UPLOAD_CRED_FAILED), strerror(ENAMETOOLONG));
    }
    free(old);
    return ok;
}

static bool upload_credentials_update(const APC_HTTPRequest *request, bool *removed,
                                      char *error, size_t error_size)
{
    size_t action_length = 0, id_length = 0, secret_length = 0, token_length = 0;
    char *action = form_value(request, "action", &action_length);
    char *id = form_value(request, "client_id", &id_length);
    char *secret = form_value(request, "client_secret", &secret_length);
    char *token = form_value(request, "token", &token_length);
    bool ok = false;
    *removed = action != NULL && action_length == strlen("remove") && memcmp(action, "remove", action_length) == 0;
    if (*removed) {
        ok = upload_replace_section(upload_rclone_config_path(), UPLOAD_REMOTE_NAME, NULL, error, error_size);
    } else if (id == NULL || secret == NULL || token == NULL ||
               !upload_credential_valid(id, id_length, false) ||
               !upload_credential_valid(secret, secret_length, false) ||
               !upload_credential_valid(token, token_length, true)) {
        snprintf(error, error_size, "%s", T(S_UPLOAD_CRED_INVALID));
    } else {
        char section[4096 + 512];
        snprintf(section, sizeof(section), "[" UPLOAD_REMOTE_NAME "]\ntype = drive\nclient_id = %s\n"
                 "client_secret = %s\ntoken = %s\n\n", id, secret, token);
        ok = upload_replace_section(upload_rclone_config_path(), UPLOAD_REMOTE_NAME, section, error, error_size);
        upload_zero(section, sizeof(section));
    }
    if (secret != NULL) upload_zero(secret, secret_length);
    if (token != NULL) upload_zero(token, token_length);
    free(action);
    free(id);
    free(secret);
    free(token);
    return ok;
}

/* The credentials inputs sit in the Cloud Upload tab but belong to this form,
 * which comes before the parameters form because forms cannot be nested. */
static void append_upload_credentials_form(APC_StringBuffer *page)
{
    page->appendf("<form id=upload-credentials method=post action=/upload-credentials>"
                  "<input type=hidden name=csrf value=\"%s\"></form>", csrf_token);
}

/* The credentials come first in the panel, across the whole width of the fields grid. */
static void append_upload_panel_top(APC_StringBuffer *page)
{
    page->append("<div style=\"grid-column:1/-1\">");
    append_upload_credentials(page);
    page->append("</div>");
}

static void append_upload_panel(APC_StringBuffer *page)
{
    append_upload_status(page);
}
#endif
