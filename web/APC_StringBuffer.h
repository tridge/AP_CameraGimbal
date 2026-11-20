#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cctype>

// Owns a response/configuration buffer. Allocation failure is reported to the
// caller; no exceptions or implicit copies on these memory-limited cameras.
class APC_StringBuffer {
public:
    APC_StringBuffer() = default;
    ~APC_StringBuffer() { reset(); }
    APC_StringBuffer(const APC_StringBuffer &) = delete;
    APC_StringBuffer &operator=(const APC_StringBuffer &) = delete;

    const char *data() const { return _data ? _data : ""; }
    void invalidate() { _failed = true; }
    bool valid() const { return !_failed; }
    size_t size() const { return _length; }
    void reset() { free(_data); _data = nullptr; _length = _capacity = 0; _failed = false; }
    char *release()
    {
        if (_failed) { reset(); return nullptr; }
        char *result = _data;
        _data = nullptr;
        _length = _capacity = 0;
        return result;
    }
    bool append_n(const char *text, size_t length);
    bool append(const char *text);
    bool vappendf(const char *format, va_list ap);
    bool appendf(const char *format, ...) __attribute__((format(printf, 2, 3)));
    bool append_html(const char *text);
    bool append_html_n(const char *text, size_t length);
    bool append_url(const char *text);
    bool append_log_html(const char *text, size_t length);

private:
    bool reserve(size_t extra);
    char *_data = nullptr;
    size_t _length = 0;
    size_t _capacity = 0;
    bool _failed = false;
};

inline bool APC_StringBuffer::reserve(size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *new_data;

    if (_failed) return false;
    if (extra > SIZE_MAX - _length - 1) {
        _failed = true;
        return false;
    }
    needed = _length + extra + 1;
    if (needed <= _capacity) {
        return true;
    }
    new_cap = _capacity ? _capacity : 4096;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    new_data = static_cast<char *>(realloc(_data, new_cap));
    if (new_data == NULL) {
        _failed = true;
        return false;
    }
    _data = new_data;
    _capacity = new_cap;
    return true;
}

inline bool APC_StringBuffer::append_n(const char *text, size_t len)
{
    if (!reserve(len)) {
        return false;
    }
    memcpy(_data + _length, text, len);
    _length += len;
    _data[_length] = '\0';
    return true;
}

inline bool APC_StringBuffer::append(const char *text)
{
    return append_n(text, strlen(text));
}

inline bool APC_StringBuffer::vappendf(const char *fmt, va_list ap)
{
    va_list copy;
    int length;

    va_copy(copy, ap);
    length = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (length < 0) { invalidate(); return false; }
    if (!reserve((size_t)length)) return false;
    if (vsnprintf(_data + _length, _capacity - _length, fmt, ap) != length) {
        invalidate();
        return false;
    }
    _length += (size_t)length;
    return true;
}

inline bool APC_StringBuffer::appendf(const char *fmt, ...)
{
    va_list ap;
    bool ok;

    va_start(ap, fmt);
    ok = vappendf(fmt, ap);
    va_end(ap);
    return ok;
}

inline bool APC_StringBuffer::append_html(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        switch (*p) {
        case '&':
            if (!append("&amp;")) return false;
            break;
        case '<':
            if (!append("&lt;")) return false;
            break;
        case '>':
            if (!append("&gt;")) return false;
            break;
        case '"':
            if (!append("&quot;")) return false;
            break;
        case '\'':
            if (!append("&#39;")) return false;
            break;
        default:
            if (*p >= 0x20 || *p == '\n' || *p == '\r' || *p == '\t') {
                if (!append_n((const char *)p, 1)) return false;
            }
            break;
        }
        p++;
    }
    return true;
}

inline bool APC_StringBuffer::append_html_n(const char *text,
                             size_t length)
{
    if (length == SIZE_MAX) { invalidate(); return false; }
    char *copy = static_cast<char *>(malloc(length + 1));
    bool ok;

    if (copy == NULL) { invalidate(); return false; }
    memcpy(copy, text, length);
    copy[length] = '\0';
    ok = append_html(copy);
    free(copy);
    return ok;
}

inline bool APC_StringBuffer::append_url(const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0') {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (!append_n((const char *)p, 1)) return false;
        } else {
            char encoded[3] = {'%', hex[*p >> 4], hex[*p & 15]};
            if (!append_n(encoded, sizeof(encoded))) return false;
        }
        p++;
    }
    return true;
}

inline bool APC_StringBuffer::append_log_html(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)text[i];
        if (value == 0x1b && i + 1 < length && text[i + 1] == '[') {
            i += 2;
            while (i < length) {
                value = (unsigned char)text[i];
                if (value >= 0x40 && value <= 0x7e) break;
                i++;
            }
            continue;
        }
        if (value == '\r') continue;
        if (value == '&') {
            if (!append("&amp;")) return false;
        } else if (value == '<') {
            if (!append("&lt;")) return false;
        } else if (value == '>') {
            if (!append("&gt;")) return false;
        } else if (value >= 0x20 || value == '\n' || value == '\t') {
            if (!append_n((const char *)&text[i], 1)) return false;
        }
    }
    return true;
}
