#pragma once
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// Only application payloads live in slots. Settings, authentication, recovery
// SSH and platform boot scripts stay in the stable directory.
namespace apcam_zt30 {
constexpr unsigned launcher_version = 1;
inline bool slot_id_valid(const char *id)
{
    if (!id || strlen(id) != 64) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}
inline bool fail(std::string &error, const std::string &message)
{
    error = message; return false;
}
inline bool directory(const std::string &path)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 0022);
}
inline bool regular(const std::string &path, bool executable = false)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        st.st_uid == geteuid() && !(st.st_mode & 0022) && (st.st_mode & 0400) &&
        (!executable || (st.st_mode & 0100));
}
inline bool canonical_root(const std::string &root, std::string &error)
{
    char resolved[4096];
    if (!realpath(root.c_str(), resolved) || root != resolved || !directory(root))
        return fail(error, "Application root must be a canonical, owner-writable directory with no group/world writes");
    return true;
}
inline bool assets_valid(const std::string &path, unsigned &files, std::string &error, unsigned depth = 0)
{
    if (depth > 4 || !directory(path)) return fail(error, "Invalid web asset directory: " + path);
    DIR *d = opendir(path.c_str()); if (!d) return fail(error, "Cannot open web assets");
    bool ok = true;
    while (auto *entry = readdir(d)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string child = path + "/" + entry->d_name;
        struct stat st{};
        if (lstat(child.c_str(), &st) < 0) { ok = fail(error, "Cannot inspect asset"); break; }
        if (++files > 256) { ok = fail(error, "Too many web asset entries"); break; }
        if (S_ISDIR(st.st_mode)) ok = assets_valid(child, files, error, depth + 1);
        else ok = regular(child);
        if (!ok) { if (error.empty()) error = "Invalid web asset: " + child; break; }
    }
    closedir(d); return ok;
}
inline bool payload_valid(const std::string &path, std::string &error, bool flat = false)
{
    if (!directory(path) || !regular(path + "/camera-app", true) ||
        !regular(path + "/zt30-web", true) || !regular(path + "/encoder-sdk.so"))
        return fail(error, "Incomplete or unsafe application payload: " + path);
    if (!flat) {
        DIR *d = opendir(path.c_str()); if (!d) return fail(error, "Cannot inspect slot");
        bool ok = true;
        while (auto *entry = readdir(d)) {
            const char *name = entry->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "camera-app") ||
                !strcmp(name, "zt30-web") || !strcmp(name, "encoder-sdk.so") || !strcmp(name, "webroot")) continue;
            if (strcmp(name, "manifest.json") || !regular(path + "/manifest.json")) { ok = false; break; }
        }
        closedir(d); if (!ok) return fail(error, "Unexpected slot payload entry");
    }
    unsigned files = 0;
    if (!assets_valid(path + "/webroot", files, error)) return false;
    for (const char *name : {"live.js", "status-actions.html", "parameters-form.html"})
        if (!regular(path + "/webroot/" + name)) return fail(error, "Required web asset missing");
    return true;
}
inline bool slot_directory(const std::string &root, const char *id, std::string &path, std::string &error)
{
    if (!slot_id_valid(id) || !canonical_root(root, error)) return fail(error, "Invalid slot ID or application root");
    path = root + "/slots/" + id;
    if (!directory(root + "/slots")) return fail(error, "Invalid slots directory");
    if (!directory(path)) return fail(error, "Invalid slot directory");
    return true;
}
inline bool slot_path(const std::string &root, const char *id, std::string &path, std::string &error)
{
    return slot_directory(root, id, path, error) && payload_valid(path, error);
}
inline bool resolve(const std::string &root, std::string &path, std::string &error)
{
    if (!canonical_root(root, error)) return false;
    char target[128];
    ssize_t n = readlink((root + "/active").c_str(), target, sizeof(target) - 1);
    if (n < 0) {
        if (errno != ENOENT) return fail(error, "Active slot must be a symlink");
        // Initial AP-only images may still use the qualified flat layout.
        // Once active exists, a damaged slot never silently selects it.
        path = root; return payload_valid(path, error, true);
    }
    target[n] = 0;
    if (strncmp(target, "slots/", 6) || !slot_id_valid(target + 6))
        return fail(error, "Invalid active slot link");
    return slot_path(root, target + 6, path, error);
}
inline bool sync_directory(const std::string &path, std::string &error)
{
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return fail(error, "Cannot open directory for sync");
    int result = fsync(fd); int e = errno; close(fd); errno = e;
    return result == 0 || fail(error, "Directory sync failed: " + path);
}
inline bool sync_payload(const std::string &path, std::string &error)
{
    DIR *d = opendir(path.c_str()); if (!d) return fail(error, "Cannot sync payload directory");
    bool ok = true;
    while (auto *entry = readdir(d)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        std::string child = path + "/" + entry->d_name; struct stat st{};
        if (lstat(child.c_str(), &st) < 0) { ok = fail(error, "Cannot inspect payload for sync"); break; }
        if (S_ISDIR(st.st_mode)) ok = sync_payload(child, error);
        else if (S_ISREG(st.st_mode)) {
            int fd = open(child.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) ok = false;
            else { ok = fsync(fd) == 0; close(fd); }
        } else ok = false;
        if (!ok) { if (error.empty()) error = "Payload sync failed"; break; }
    }
    closedir(d); return ok && sync_directory(path, error);
}
inline bool replace_link(const std::string &root, const char *name, const std::string &target, std::string &error)
{
    std::string temporary = root + "/.slot-link.XXXXXX";
    int fd = mkstemp(&temporary[0]); if (fd < 0) return fail(error, "Cannot reserve slot link");
    close(fd);
    if (unlink(temporary.c_str()) < 0 || symlink(target.c_str(), temporary.c_str()) < 0) {
        unlink(temporary.c_str()); return fail(error, "Cannot create slot link");
    }
    if (rename(temporary.c_str(), (root + "/" + name).c_str()) < 0) {
        unlink(temporary.c_str()); return fail(error, "Cannot publish slot link");
    }
    // Failure here means publication happened but durability is unconfirmed;
    // callers must inspect the active link, rather than assume rollback.
    return sync_directory(root, error);
}
inline bool activate(const std::string &root, const char *id, std::string &error)
{
    std::string path;
    if (!slot_path(root, id, path, error) || !sync_payload(path, error) ||
        !sync_directory(root + "/slots", error)) return false;
    return replace_link(root, "active", std::string("slots/") + id, error);
}
inline bool equal_files(const std::string &a, const std::string &b)
{
    int fa = open(a.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    int fb = open(b.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fa < 0 || fb < 0) { if (fa >= 0) close(fa); if (fb >= 0) close(fb); return false; }
    bool equal = true; char x[4096], y[4096];
    while (true) {
        ssize_t nx, ny;
        do { nx = read(fa, x, sizeof(x)); } while (nx < 0 && errno == EINTR);
        do { ny = read(fb, y, sizeof(y)); } while (ny < 0 && errno == EINTR);
        if (nx < 0 || ny < 0 || nx != ny || (nx && memcmp(x, y, (size_t)nx))) { equal = false; break; }
        if (!nx) break;
    }
    close(fa); close(fb); return equal;
}
inline bool migrate(const std::string &root, const char *id, std::string &error)
{
    std::string path;
    if (!slot_path(root, id, path, error)) return false;
    struct stat st{};
    if (lstat((root + "/active").c_str(), &st) == 0) {
        std::string current;
        if (!resolve(root, current, error) || current != path) return fail(error, "Migration must retain the current slot");
    } else if (errno != ENOENT) return fail(error, "Cannot inspect active slot");
    // Alias publication is idempotent. Validate every old file before touching
    // anything; a crash between aliases still boots the complete selected slot.
    for (const char *name : {"camera-app", "zt30-web", "encoder-sdk.so"}) {
        char target[128];
        ssize_t n = readlink((root + "/" + name).c_str(), target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = 0;
            if (std::string(target) != std::string("active/") + name) return fail(error, "Unexpected legacy alias");
        } else if (!regular(root + "/" + name) || !equal_files(root + "/" + name, path + "/" + name))
            return fail(error, "Migration payload must match the installed AP files");
    }
    if (!activate(root, id, error)) return false;
    for (const char *name : {"camera-app", "zt30-web", "encoder-sdk.so"})
        if (!replace_link(root, name, std::string("active/") + name, error)) return false;
    return true;
}
} // namespace apcam_zt30
