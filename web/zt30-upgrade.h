#pragma once
static bool zt30_upgrade_restart(void *, const std::string &, std::string &error)
{
    char text[512]{};
    bool ok = restart_camera_locked(CAMERA_REPLACEMENT, text, sizeof(text)) && restarted_config_ok(text, sizeof(text));
    if (!ok) error = text;
    return ok;
}
static bool zt30_upgrade_ready(void *, const std::string &id, bool &thermal, std::string &error)
{
    pid_t pid = ready_pid(); std::string path, expected = std::string(APP_DIR)+"/slots/"+id+"/camera-app";
    if (!apcam_zt30::proc_executable(pid, path) || path != expected ||
        !zt30_slot_app_pid(pid, path.c_str(), APP_DIR)) return apcam_zt30::fail(error, "New camera process is not running the selected bundle");
    if (!apcam::live_video_probe(live_video_port(), 0, 8000)) return apcam_zt30::fail(error, "New camera has no live RGB media fragments");
    thermal = apcam::live_video_probe(live_video_port(), 1, 12000);
    return true;
}
// Explicit dependencies permit isolated host failure tests. Production always
// initializes these with the real managed-process and live-stream checks.
static apcam_zt30::UpdateServices zt30_upgrade_services{zt30_upgrade_restart, zt30_upgrade_ready, nullptr};
#ifndef ZT30_UPLOAD_DURATION_MS
#define ZT30_UPLOAD_DURATION_MS 120000
#endif
#ifndef ZT30_RUNTIME_ROOT
#define ZT30_RUNTIME_ROOT "/"
#endif
static void send_zt30_upgrade_result(int fd)
{
    apcam_zt30::Trial t; std::string error;
    if (!apcam_zt30::read_trial(APP_DIR, t, error)) {send_text_error(fd, 500, "Internal Server Error", error.c_str(), nullptr); return;}
    std::string result = "{\"state\":\""+t.state+"\",\"candidate\":\""+t.candidate+"\",\"csrf\":\""+csrf_token+"\"}";
    send_response(fd, 200, "OK", "application/json", result.data(), result.size(), nullptr);
}
static bool clean_zt30_uploads()
{
    DIR *directory = opendir(RUNTIME_DIR); if (!directory) return false;
    bool ok = true; unsigned entries = 0;
    while (auto *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.compare(0, 16, "firmware-upload.") || name.size() != 22 || !apcam_zt30::token(name.substr(16), 6)) continue;
        if (++entries > 64) {ok = false; break;}
        struct stat st{}; const std::string path = std::string(RUNTIME_DIR)+"/"+name;
        if (lstat(path.c_str(), &st) || st.st_uid != geteuid() || (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) || unlink(path.c_str())) {ok = false; break;}
    }
    closedir(directory); return ok;
}
static void handle_zt30_upgrade(int fd, const APC_HTTPRequest *request, const char *peer, bool rollback = false)
{
    if (!rollback && !valid_csrf_header(request)) {send_text_errorf(fd, 403, "Forbidden", S_CSRF_RELOAD); return;}
    char filename[256] = {}, type[128] = {};
    if (!rollback) {
        if (!request->content_length || request->content_length > apcam_zt30::bundle_limit) {send_text_errorf(fd, 413, "Payload Too Large", S_E_FW_SIZE); return;}
        if (!find_header(request, "Content-Type", type, sizeof(type)) || strncasecmp(type, "application/octet-stream", 24) ||
            (type[24] && type[24] != ';')) {send_text_errorf(fd, 415, "Unsupported Media Type", S_E_FW_CONTENT_TYPE); return;}
        if (!find_header(request, "X-Firmware-Name", filename, sizeof(filename)) || !valid_firmware_name(filename)) {
            send_text_errorf(fd, 400, "Bad Request", S_E_FW_NAME, FIRMWARE_NAME_PATTERNS); return;
        }
    }
    int upload_lock = open(UPGRADE_LOCK_PATH, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (upload_lock < 0 || flock(upload_lock, LOCK_EX | LOCK_NB)) {
        if (upload_lock >= 0) close(upload_lock);
        send_text_error(fd, 409, "Conflict", "Another application update is in progress.\n", nullptr); return;
    }
    if (!clean_zt30_uploads()) {
        close(upload_lock); send_text_error(fd, 500, "Internal Server Error", "Cannot remove interrupted application uploads.\n", nullptr); return;
    }
    int output = -1, restart = -1, writer = -1;
    std::string package, error; apcam_zt30::UpdateResult result;
    auto clean = [&]() {if (output >= 0) close(output); if (!package.empty()) unlink(package.c_str());
        if (writer >= 0) close(writer);
        if (restart >= 0) close(restart);
        close(upload_lock);};
    if (!rollback) {
        package = std::string(RUNTIME_DIR)+"/firmware-upload.XXXXXX";
        output = mkostemp(&package[0], O_CLOEXEC);
        struct statvfs space{};
        if (output < 0 || fstatvfs(output, &space) || uint64_t(space.f_bavail)*space.f_frsize < request->content_length+1024*1024) {
            clean(); send_text_error(fd, 507, "Insufficient Storage", "Insufficient temporary space for the application bundle.\n", nullptr); return;
        }
        size_t received = 0;
        if (!receive_upload_body(fd, request, output, &received, ZT30_UPLOAD_DURATION_MS) || fsync(output)) {
            clean(); send_text_error(fd, 400, "Bad Request", "Application upload was interrupted.\n", nullptr); return;
        }
        close(output); output = -1;
    }
    char restart_error[512]{}; restart = lock_restart(restart_error, sizeof(restart_error), true);
    writer = restart < 0 ? -1 : open((std::string(APP_DIR)+"/.slot-update.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (restart < 0 || writer < 0 || !apcam_zt30::regular(std::string(APP_DIR)+"/.slot-update.lock") || flock(writer, LOCK_EX | LOCK_NB)) {
        clean(); send_text_error(fd, 409, "Conflict", "Camera restart or slot update is busy.\n", nullptr); return;
    }
    bool ok;
    if (rollback) {
        std::string previous;
        ok = apcam_zt30::link_slot(APP_DIR, "previous", previous, error) &&
            apcam_zt30::switch_application(APP_DIR, ZT30_RUNTIME_ROOT, previous, zt30_upgrade_services, result, error);
    } else ok = apcam_zt30::update_application(APP_DIR, package, ZT30_RUNTIME_ROOT, zt30_upgrade_services, result, error);
    if (!ok) {
        log_message("AP application update from %s failed: %s", peer, error.c_str());
        send_text_error(fd, result.activation_attempted ? 500 : 400, result.activation_attempted ? "Internal Server Error" : "Bad Request", error.c_str(), nullptr);
    } else {
        log_message("AP application %s started by %s; web trial confirmation pending, thermal=%s", result.selected.c_str(), peer, result.thermal ? "live" : "unavailable");
        {
            std::string body = "{\"slot\":\""+result.selected+"\",\"thermal\":"+(result.thermal ? "true" : "false")+"}";
            send_response(fd, 201, "Created", "application/json", body.data(), body.size(), nullptr);
        }
    }
    // Supervisor cannot reload the old master until response has been sent and
    // both locks are released. Its persistent trial survives worker/power loss.
    clean();
}
