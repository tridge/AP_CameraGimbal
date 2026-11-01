#pragma once

#include "APC_StringBuffer.h"
#include "build/webroot.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <initializer_list>

// Values are already escaped according to their HTML context by page code.
// Numeric format strings are compiled code, never supplied by a template.
class APC_TemplateValue {
public:
    APC_TemplateValue(const char *text) : _text(text ? text : "") {}
    template<typename T> APC_TemplateValue(const char *format, T value)
    {
        const int length = snprintf(_number, sizeof(_number), format, value);
        _valid = length >= 0 && size_t(length) < sizeof(_number);
    }
    const char *text() const { return _text ? _text : _number; }
    bool valid() const { return _valid; }
private:
    const char *_text = nullptr;
    char _number[128] {};
    bool _valid = true;
};

// Only built-in manifest names can be read. Assets are flat regular files;
// descriptor-relative opens refuse symlinks and cannot escape the webroot.
// Templates are private and never served directly through an HTTP route.
class APC_WebRoot {
public:
    static bool known_asset(const char *name)
    {
        for (const char *entry : apcam_web_assets) {
            if (strcmp(name, entry) == 0) return true;
        }
        return false;
    }
    static uint64_t asset_limit(const char *name)
    {
        return strcmp(name, "mpegts.js") == 0 ? 512U * 1024U : 256U * 1024U;
    }
    bool available() const
    {
        if (!_path) return false;
        int directory = open(_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0) return false;
        bool ok = true;
        for (const char *name : apcam_web_assets) {
            int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            struct stat st {};
            ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size > 0 && uint64_t(st.st_size) <= asset_limit(name);
            if (fd >= 0) close(fd);
            if (!ok) break;
        }
        close(directory);
        return ok;
    }
    void set_path(const char *path) { _path = path; }
    bool append(APC_StringBuffer &output, const char *name) const
    {
        if (!known_asset(name) || !_path) { output.invalidate(); return false; }
        int directory = open(_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0) { output.invalidate(); return false; }
        int fd = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        close(directory);
        if (fd < 0) { output.invalidate(); return false; }
        struct stat st {};
        bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
                  uint64_t(st.st_size) <= asset_limit(name);
        char buffer[4096];
        off_t remaining = ok ? st.st_size : 0;
        while (ok && remaining > 0) {
            ssize_t count = read(fd, buffer, remaining < off_t(sizeof(buffer)) ? size_t(remaining) : sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            ok = output.append_n(buffer, size_t(count));
            remaining -= count;
        }
        close(fd);
        if (!ok) output.invalidate();
        return ok;
    }
    bool render(APC_StringBuffer &output, const char *name,
                std::initializer_list<APC_TemplateValue> values) const
    {
        APC_StringBuffer source;
        if (!append(source, name)) { output.invalidate(); return false; }
        for (const auto &value : values) {
            if (!value.valid()) { output.invalidate(); return false; }
        }
        const char *cursor = source.data();
        while (const char *start = strstr(cursor, "{{")) {
            if (!output.append_n(cursor, size_t(start - cursor))) return false;
            char *end = nullptr;
            errno = 0;
            unsigned long index = strtoul(start + 2, &end, 10);
            if (errno || end == start + 2 || strncmp(end, "}}", 2) || index >= values.size()) {
                output.invalidate();
                return false;
            }
            if (!output.append((values.begin() + index)->text())) return false;
            cursor = end + 2;
        }
        return output.append(cursor);
    }
private:
    const char *_path = nullptr;
};
