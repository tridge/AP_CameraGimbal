#pragma once
#include "zt30_slots.h"
#include "zt30_bundle_elf.h"
#include "sha256.h"
#include "json_reader.h"
#include <set>
#include <sys/statvfs.h>
#include <vector>
#include <algorithm>

namespace apcam_zt30 {
constexpr uint64_t bundle_limit = 32*1024*1024;
constexpr uint64_t bundle_reserve = 8*1024*1024;
constexpr unsigned manifest_limit = 65536;
struct BundleFile { std::string path, hash; unsigned mode = 0; uint64_t size = 0; };
struct BundleRuntime { std::string name, path, hash; uint64_t size = 0; };
struct BundleManifest {
    std::string version, text, id;
    std::vector<BundleFile> files;
    std::vector<BundleRuntime> runtime;
    uint64_t size = 0;
};
inline bool token(const std::string &s, size_t maximum = 128)
{
    if (s.empty() || s.size() > maximum || s[0] == '.' || s.find("..") != std::string::npos) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '+')) return false;
    return true;
}
inline bool bundle_asset(const std::string &path)
{
    if (path.compare(0, 8, "webroot/")) return false;
    size_t at = 8; unsigned components = 0;
    while (true) {
        size_t end = path.find('/', at);
        if (++components > 4 || !token(path.substr(at, end == std::string::npos ? end : end-at), 80)) return false;
        if (end == std::string::npos) break;
        at = end+1;
    }
    size_t dot = path.rfind('.');
    return dot != std::string::npos && (path.substr(dot) == ".html" || path.substr(dot) == ".js" ||
        path.substr(dot) == ".css" || path.substr(dot) == ".txt");
}
inline bool bundle_runtime_path(const std::string &path)
{
    for (const char *prefix : {"/lib/", "/usr/lib/", "/config/lib/"}) {
        size_t n = strlen(prefix);
        if (!path.compare(0, n, prefix) && token(path.substr(n))) return true;
    }
    return false;
}
// Restricted canonical JSON: exact field order, ASCII strings without escapes,
// no whitespace/unknown fields, bounded integers and sorted unique entries.
// It is the same manifest on the wire and in the installed slot.
class BundleJSON {
    const std::string &text;
    size_t at = 0;
public:
    explicit BundleJSON(const std::string &t) : text(t) {}
    bool take(const char *s) {
        size_t n = strlen(s);
        if (text.compare(at, n, s)) return false;
        at += n; return true;
    }
    bool string(std::string &s) {
        if (!take("\"")) return false;
        size_t start = at;
        while (at < text.size() && text[at] != '"') {
            unsigned char c = text[at++]; if (c < 32 || c > 126 || c == '\\' || at-start > 255) return false;
        }
        s = text.substr(start, at-start); return take("\"");
    }
    bool number(uint64_t &n, uint64_t maximum) {
        n = 0; size_t start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            unsigned d = unsigned(text[at++]-'0');
            if (n > maximum/10 || (n == maximum/10 && d > maximum%10)) return false;
            n = n*10+d;
        }
        return at > start && (at-start == 1 || text[start] != '0');
    }
    bool done() const { return at == text.size(); }
};
inline bool parse_bundle_manifest(BundleManifest &m, std::string &error)
{
    BundleJSON j(m.text); uint64_t launcher = 0;
    if (!j.take("{\"schema\":\"apcg.zt30.app.v1\",\"board\":\"SIYI_ZT30\",\"launcher\":") ||
        !j.number(launcher, launcher_version) || launcher != launcher_version || !j.take(",\"version\":") ||
        !j.string(m.version) || !token(m.version, 80) || !j.take(",\"runtime\":[")) return fail(error, "Invalid bundle identity or launcher version");
    do {
        BundleRuntime r;
        if (m.runtime.size() >= 64 || !j.take("{\"name\":") || !j.string(r.name) || !token(r.name) ||
            !j.take(",\"path\":") || !j.string(r.path) || !bundle_runtime_path(r.path) ||
            !j.take(",\"size\":") || !j.number(r.size, bundle_limit) || !r.size ||
            !j.take(",\"sha256\":") || !j.string(r.hash) || !slot_id_valid(r.hash.c_str()) || !j.take("}") ||
            (!m.runtime.empty() && m.runtime.back().name >= r.name)) return fail(error, "Invalid or duplicate runtime dependency");
        m.runtime.push_back(r);
    } while (j.take(","));
    if (!j.take("],\"files\":[")) return fail(error, "Missing bundle files");
    do {
        BundleFile f; uint64_t mode = 0;
        if (m.files.size() >= 128 || !j.take("{\"path\":") || !j.string(f.path) ||
            !j.take(",\"mode\":") || !j.number(mode, 0755) || !j.take(",\"size\":") ||
            !j.number(f.size, bundle_limit) || !f.size || !j.take(",\"sha256\":") ||
            !j.string(f.hash) || !slot_id_valid(f.hash.c_str()) || !j.take("}") ||
            (!m.files.empty() && m.files.back().path >= f.path)) return fail(error, "Invalid or duplicate bundle file");
        bool binary = f.path == "camera-app" || f.path == "zt30-web" || f.path == "encoder-sdk.so";
        f.mode = unsigned(mode);
        if ((!binary && !bundle_asset(f.path)) || f.mode != unsigned(binary ? 0755 : 0644) ||
            f.size > (binary ? 16*1024*1024U : 2*1024*1024U) || f.size > bundle_limit-m.size)
            return fail(error, "Disallowed bundle path, mode or size");
        m.size += f.size; m.files.push_back(f);
    } while (j.take(","));
    if (!j.take("]}") || !j.done()) return fail(error, "Non-canonical or trailing manifest data");
    for (const char *required : {"camera-app", "zt30-web", "encoder-sdk.so", "webroot/live.js",
            "webroot/status-actions.html", "webroot/parameters-form.html"}) {
        bool found = false; for (const auto &f : m.files) if (f.path == required) found = true;
        if (!found) return fail(error, "Required bundle file missing");
    }
    std::set<std::string> directories;
    for (const auto &f : m.files) {
        size_t at = 0;
        while ((at = f.path.find('/', at)) != std::string::npos) directories.insert(f.path.substr(0, at++));
    }
    if (directories.size()+m.files.size()+1 > 256) return fail(error, "Too many bundle tree entries");
    m.id = apcam::sha256(m.text); return true;
}
inline bool read_exact(int fd, void *p, size_t size)
{
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(fd, static_cast<char *>(p)+used, size-used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += size_t(n);
    }
    return true;
}
inline bool write_exact(int fd, const void *p, size_t size)
{
    size_t used = 0;
    while (used < size) {
        ssize_t n = write(fd, static_cast<const char *>(p)+used, size-used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        used += size_t(n);
    }
    return true;
}
inline bool file_digest(const std::string &path, uint64_t expected_size, const std::string &expected_hash,
                        bool follow, std::string &error)
{
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | (follow ? 0 : O_NOFOLLOW));
    struct stat st{}; apcam::SHA256 h; char buffer[65536]; uint64_t remaining = expected_size;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && uint64_t(st.st_size) == expected_size;
    while (ok && remaining) {
        size_t n = size_t(std::min<uint64_t>(remaining, sizeof(buffer)));
        ok = read_exact(fd, buffer, n); if (ok) {h.update(buffer, n); remaining -= n;}
    }
    if (fd >= 0) close(fd);
    return (ok && h.finish() == expected_hash) || fail(error, "Size/checksum mismatch: " + path);
}
// Only installer-owned directories are removed. No symlinks are traversed and
// recursion/entry counts are bounded even for an interrupted or damaged stage.
inline bool remove_stage_tree(const std::string &path, unsigned &entries, unsigned depth = 0)
{
    if (depth > 6 || !directory(path)) return false;
    DIR *d = opendir(path.c_str()); if (!d) return false;
    bool ok = true;
    while (true) {
        errno = 0; auto *e = readdir(d);
        if (!e) {if (errno) ok = false; break;}
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (++entries > 512) {ok = false; break;}
        std::string child = path+"/"+e->d_name; struct stat st{};
        if (lstat(child.c_str(), &st) < 0) {ok = false; break;}
        if (S_ISDIR(st.st_mode)) ok = remove_stage_tree(child, entries, depth+1);
        else ok = (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) && unlink(child.c_str()) == 0;
        if (!ok) break;
    }
    closedir(d); return ok && rmdir(path.c_str()) == 0;
}
inline bool clean_orphan_stages(const std::string &slots, std::string &error)
{
    DIR *d = opendir(slots.c_str()); if (!d) return fail(error, "Cannot inspect slot stages");
    bool ok = true; unsigned count = 0;
    while (true) {
        errno = 0; auto *e = readdir(d);
        if (!e) {if (errno) ok = false; break;}
        if (strncmp(e->d_name, ".stage.", 7)) continue;
        std::string suffix = e->d_name+7;
        if (suffix.size() != 6 || !token(suffix, 6) || ++count > 16) {ok = false; break;}
        unsigned entries = 0;
        if (!remove_stage_tree(slots+"/"+e->d_name, entries)) {ok = false; break;}
    }
    closedir(d); return ok || fail(error, "Unsafe or excessive orphan stages; manual inspection required");
}
inline bool same_tree(const std::string &a, const std::string &b, unsigned &entries, unsigned depth = 0)
{
    if (depth > 6 || !directory(a) || !directory(b)) return false;
    auto names = [](const std::string &path, std::vector<std::string> &out) {
        DIR *d = opendir(path.c_str()); if (!d) return false; bool ok = true;
        while (true) {
            errno = 0; auto *e = readdir(d); if (!e) {if (errno) ok = false; break;}
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) out.emplace_back(e->d_name);
            if (out.size() > 256) {ok = false; break;}
        }
        closedir(d); std::sort(out.begin(), out.end()); return ok;
    };
    std::vector<std::string> x, y;
    if (!names(a, x) || !names(b, y) || x != y) return false;
    for (const auto &name : x) {
        if (++entries > 512) return false;
        std::string pa = a+"/"+name, pb = b+"/"+name; struct stat sa{}, sb{};
        if (lstat(pa.c_str(), &sa) || lstat(pb.c_str(), &sb) || (sa.st_mode & 07777) != (sb.st_mode & 07777)) return false;
        if (S_ISDIR(sa.st_mode)) {if (!same_tree(pa, pb, entries, depth+1)) return false;}
        else if (!S_ISREG(sa.st_mode) || !regular(pa) || !regular(pb) || !equal_files(pa, pb)) return false;
    }
    return true;
}
inline bool stage_bundle(const std::string &root, const std::string &bundle, const std::string &runtime_root,
                         std::string &selected, std::string &error)
{
    // Caller holds .slot-update.lock for the entire validation/publication.
    if (!canonical_root(root, error) || !directory(root+"/slots")) return fail(error, "A safe managed slots directory is required");
    int fd = open(bundle.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC); struct stat st{}; unsigned char header[12];
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 12 &&
        uint64_t(st.st_size) <= bundle_limit && read_exact(fd, header, sizeof(header)) && !memcmp(header, "APCZT301", 8);
    uint32_t length = ok ? uint32_t(header[8]) | (uint32_t(header[9])<<8) | (uint32_t(header[10])<<16) | (uint32_t(header[11])<<24) : 0;
    BundleManifest m;
    if (ok && length && length <= manifest_limit && uint64_t(length)+12 <= uint64_t(st.st_size)) {
        m.text.resize(length); ok = read_exact(fd, &m.text[0], length) && parse_bundle_manifest(m, error) &&
            m.size+length+12 == uint64_t(st.st_size);
    } else ok = false;
    if (!ok) {if (fd >= 0) close(fd); return fail(error, error.empty() ? "Invalid or truncated application bundle" : error);}
    for (const auto &r : m.runtime) {
        std::string path = (runtime_root == "/" ? "" : runtime_root)+r.path;
        if (!file_digest(path, r.size, r.hash, true, error)) {close(fd); return false;}
    }
    // Check stable bootstrap dependencies before touching a payload. They are
    // updated separately via SSH, never replaced by an application bundle.
    for (const char *name : {"zt30-slot", "app-init.sh", "app-supervisor.sh", "web-supervisor.sh", "start-dropbear.sh", "dropbear"})
        if (!regular(root+"/"+name, true)) {close(fd); return fail(error, "Missing or unsafe stable launcher/recovery dependency");}
    if (!clean_orphan_stages(root+"/slots", error)) {close(fd); return false;}
    struct statvfs space{};
    if (statvfs((root+"/slots").c_str(), &space) || !space.f_frsize ||
        uint64_t(space.f_bavail) < (m.size+manifest_limit+bundle_reserve+space.f_frsize-1)/space.f_frsize ||
        (space.f_files && space.f_favail < m.files.size()+16)) {close(fd); return fail(error, "Insufficient application-slot space (8 MiB reserve required)");}
    std::string stage = root+"/slots/.stage.XXXXXX";
    if (!mkdtemp(&stage[0])) {close(fd); return fail(error, "Cannot create private application stage");}
    for (const auto &f : m.files) {
        size_t at = stage.size()+1; std::string path = stage+"/"+f.path;
        while ((at = path.find('/', at)) != std::string::npos) {
            std::string dir = path.substr(0, at++);
            if (mkdir(dir.c_str(), 0755) && errno != EEXIST) {ok = false; break;}
            if (!directory(dir) || chmod(dir.c_str(), 0755)) {ok = false; break;}
        }
        if (!ok) break;
        int out = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        apcam::SHA256 h; char buffer[65536]; uint64_t left = f.size;
        ok = out >= 0;
        while (ok && left) {
            size_t n = size_t(std::min<uint64_t>(left, sizeof(buffer)));
            ok = read_exact(fd, buffer, n) && write_exact(out, buffer, n);
            if (ok) {h.update(buffer, n); left -= n;}
        }
        ok = ok && h.finish() == f.hash && fchmod(out, f.mode) == 0 && fsync(out) == 0;
        if (out >= 0) close(out);
        if (!ok) {fail(error, "Payload checksum or staged write failed: "+f.path); break;}
    }
    close(fd);
    if (ok) {
        int out = open((stage+"/manifest.json").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        ok = out >= 0 && write_exact(out, m.text.data(), m.text.size()) && fchmod(out, 0644) == 0 && fsync(out) == 0;
        if (out >= 0) close(out);
        if (!ok) fail(error, "Cannot store canonical slot manifest");
    }
    if (ok) ok = basic_elf(stage+"/camera-app", false, error) && basic_elf(stage+"/zt30-web", false, error) &&
        basic_elf(stage+"/encoder-sdk.so", true, error) && payload_valid(stage, error) &&
        chmod(stage.c_str(), 0755) == 0 && sync_payload(stage, error);
    std::string destination = root+"/slots/"+m.id; struct stat existing{};
    if (ok && lstat(destination.c_str(), &existing) == 0) {
        unsigned entries = 0;
        ok = same_tree(stage, destination, entries);
        if (!ok) fail(error, "Existing slot is damaged or differs from the bundle");
        else selected = m.id;
    } else if (ok) {
        if (errno != ENOENT || rename(stage.c_str(), destination.c_str())) ok = fail(error, "Cannot publish complete application slot");
        else {selected = m.id; stage.clear();}
    }
    if (!stage.empty()) {unsigned entries = 0; if (!remove_stage_tree(stage, entries) && ok) ok = fail(error, "Cannot remove private stage");}
    if (ok) ok = sync_directory(root+"/slots", error);
    if (ok) selected = m.id;
    // Failure after publication may leave a complete retained slot, but never
    // changes active. Activation is an explicit, separately locked operation.
    return ok;
}

inline bool manifest_identity(const std::string &text, std::string &schema)
{
    gcu_json j{text.data(), text.data()+text.size()}; bool seen = false, board = false;
    if (!gcu_json_take(&j, '{')) return false;
    do {
        char key[128];
        if (!gcu_json_string(&j, key, sizeof(key)) || !gcu_json_take(&j, ':')) return false;
        if (!strcmp(key, "schema")) {
            char value[128]; if (seen || !gcu_json_string(&j, value, sizeof(value))) return false;
            schema = value; seen = true;
        } else if (!strcmp(key, "board")) {
            char value[64]; if (board || !gcu_json_string(&j, value, sizeof(value)) || strcmp(value, "SIYI_ZT30")) return false;
            board = true;
        } else if (!gcu_json_value(&j, 1)) return false;
        if (gcu_json_take(&j, '}')) {gcu_json_space(&j); return seen && board && j.p == j.end;}
    } while (gcu_json_take(&j, ','));
    return false;
}
inline bool slot_entries(const std::string &root, const std::string &relative,
                         std::set<std::string> &files, std::set<std::string> &dirs, unsigned depth = 0)
{
    if (depth > 5 || !directory(root+relative)) return false;
    DIR *d = opendir((root+relative).c_str()); if (!d) return false;
    bool ok = true;
    while (true) {
        errno = 0; auto *e = readdir(d); if (!e) {if (errno) ok = false; break;}
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (files.size()+dirs.size() >= 256) {ok = false; break;}
        std::string name = relative+"/"+e->d_name; struct stat st{};
        if (lstat((root+name).c_str(), &st)) {ok = false; break;}
        if (S_ISDIR(st.st_mode)) {dirs.insert(name.substr(1)); ok = slot_entries(root, name, files, dirs, depth+1);}
        else {ok = regular(root+name); if (ok) files.insert(name.substr(1));}
        if (!ok) break;
    }
    closedir(d); return ok;
}
inline bool verify_bundle_slot(const std::string &root, const char *id, const std::string &runtime_root, std::string &error)
{
    std::string path; if (!slot_path(root, id, path, error)) return false;
    int fd = open((path+"/manifest.json").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st{}; std::string text;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && uint64_t(st.st_size) <= manifest_limit;
    if (ok) {text.resize(size_t(st.st_size)); ok = read_exact(fd, &text[0], text.size());}
    if (fd >= 0) close(fd);
    std::string schema;
    if (!ok || apcam::sha256(text) != id || !manifest_identity(text, schema)) return fail(error, "Invalid slot manifest or slot ID");
    // Explicitly support the trusted Stage2a migration manifests. These are not
    // release bundles and their payload was checked during migration.
    if (schema == "apcg.zt30.slot-development.v1") return true;
    if (schema != "apcg.zt30.app.v1") return fail(error, "Unsupported slot manifest schema");
    BundleManifest m; m.text = text; if (!parse_bundle_manifest(m, error)) return false;
    std::set<std::string> files, dirs, expected_files{"manifest.json"}, expected_dirs;
    for (const auto &f : m.files) {
        expected_files.insert(f.path);
        size_t at = 0; while ((at = f.path.find('/', at)) != std::string::npos) expected_dirs.insert(f.path.substr(0, at++));
        struct stat metadata{};
        if (lstat((path+"/"+f.path).c_str(), &metadata) || (metadata.st_mode & 07777) != f.mode ||
            !file_digest(path+"/"+f.path, f.size, f.hash, false, error)) return fail(error, "Slot payload changed: "+f.path);
    }
    if (!slot_entries(path, "", files, dirs) || files != expected_files || dirs != expected_dirs)
        return fail(error, "Slot tree differs from the canonical manifest");
    for (const auto &dep : m.runtime) {
        std::string runtime = (runtime_root == "/" ? "" : runtime_root)+dep.path;
        if (!file_digest(runtime, dep.size, dep.hash, true, error)) return false;
    }
    return basic_elf(path+"/camera-app", false, error) && basic_elf(path+"/zt30-web", false, error) &&
        basic_elf(path+"/encoder-sdk.so", true, error);
}
inline bool activate_bundle(const std::string &root, const char *id, const std::string &runtime_root, std::string &error)
{
    return verify_bundle_slot(root, id, runtime_root, error) && activate(root, id, error);
}
} // namespace apcam_zt30
