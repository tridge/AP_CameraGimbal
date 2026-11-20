#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <strings.h>
#include <sys/socket.h>
// The request owns its wire buffer; temporary header views never own it.
// Request limits and streamed firmware-body handling match the original parser.
class APC_HTTPRequest {
public:
    APC_HTTPRequest() = default;
    ~APC_HTTPRequest() { if (_owns_storage) free(storage); }
    APC_HTTPRequest(const APC_HTTPRequest &) = delete;
    APC_HTTPRequest &operator=(const APC_HTTPRequest &) = delete;
    int receive(int fd);
    const char *header_span(const char *name, size_t *length) const;
    const char *header(const char *name, char *value, size_t capacity) const;

    char method[12] {};
    char path[4096] {};
    char query[4096] {};
    char *storage = nullptr;
    size_t header_len = 0;
    char *body = nullptr;
    size_t body_len = 0;
    size_t content_length = 0;
    bool streaming_body = false;
private:
    static constexpr size_t MAX_HEADER = 16U * 1024U;
    static constexpr size_t MAX_BODY = 256U * 1024U;
    static constexpr int RECEIVE_INCOMPLETE = -1;
    bool _owns_storage = false;
    static bool parse_content_length(const char *, size_t, size_t *, bool *);
};

/* trimmed value of the first header called name, or NULL when absent */
inline const char *APC_HTTPRequest::header_span(const char *name, size_t *length) const
{
    if (!storage) return nullptr;
    const char *p = this->storage;
    const char *end = this->storage + this->header_len;
    size_t name_len = strlen(name);

    while (p < end) {
        const char *line_end = (const char*)(memchr(p, '\n', (size_t)(end - p)));
        if (line_end == NULL) line_end = end;
        if ((size_t)(line_end - p) > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *value_start = p + name_len + 1;
            const char *value_end;
            while (value_start < line_end && isspace((unsigned char)*value_start)) {
                value_start++;
            }
            value_end = line_end;
            while (value_end > value_start && isspace((unsigned char)value_end[-1])) {
                value_end--;
            }
            *length = (size_t)(value_end - value_start);
            return value_start;
        }
        p = line_end + (line_end < end ? 1 : 0);
    }
    return NULL;
}

inline const char *APC_HTTPRequest::header(const char *name, char *value, size_t value_size) const
{
    size_t length;
    const char *start = header_span(name, &length);

    if (start == NULL || length >= value_size) return NULL;
    memcpy(value, start, length);
    value[length] = '\0';
    return value;
}

inline bool APC_HTTPRequest::parse_content_length(const char *headers, size_t header_len,
                                 size_t *content_length, bool *present)
{
    APC_HTTPRequest temporary {};
    temporary.storage = (char *)headers;
    temporary.header_len = header_len;
    char value[64];
    char *end;
    unsigned long long parsed;

    *content_length = 0;
    *present = false;
    if (temporary.header("Content-Length", value, sizeof(value)) == NULL) {
        return true;
    }
    *present = true;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || *value == '\0' || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }
    *content_length = (size_t)parsed;
    return true;
}

inline int APC_HTTPRequest::receive(int fd)
{
    size_t capacity = MAX_HEADER + 1;
    size_t used = 0;
    size_t header_len = 0;
    size_t content_length = 0;
    bool content_length_present = false;
    char *storage = (char*)(calloc(1, capacity));

    if (storage == NULL) return 500;
    while (used < MAX_HEADER) {
        ssize_t got = recv(fd, storage + used, capacity - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(storage);
            return RECEIVE_INCOMPLETE;
        }
        if (got == 0) break;
        used += (size_t)got;
        storage[used] = '\0';
        char *boundary = strstr(storage, "\r\n\r\n");
        if (boundary != NULL) {
            header_len = (size_t)(boundary + 4 - storage);
            if (header_len > MAX_HEADER) {
                free(storage);
                return 431;
            }
            break;
        }
    }
    if (header_len == 0) {
        free(storage);
        return used >= MAX_HEADER ? 431 : RECEIVE_INCOMPLETE;
    }
    if (sscanf(storage, "%11s %4095s", this->method, this->path) != 2) {
        free(storage);
        return 400;
    }
    char *query = strchr(this->path, '?');
    if (query != NULL) {
        snprintf(this->query, sizeof(this->query), "%s", query + 1);
        *query = '\0';
    }
    if (!parse_content_length(storage, header_len, &content_length,
                              &content_length_present)) {
        free(storage);
        return 400;
    }
    char transfer_encoding[64];
    APC_HTTPRequest headers {};
    headers.storage = storage;
    headers.header_len = header_len;
    if (headers.header("Transfer-Encoding", transfer_encoding,
                    sizeof(transfer_encoding)) != NULL) {
        free(storage);
        return 400;
    }
    this->streaming_body = strcmp(this->method, "POST") == 0 &&
                              (strcmp(this->path, "/upgrade") == 0 || strcmp(this->path, "/survey/terrain") == 0);
    this->content_length = content_length;
    this->storage = storage;
    this->header_len = header_len;
    this->body = storage + header_len;
    this->body_len = used - header_len;

    if (this->body_len > content_length ||
        ((strcmp(this->method, "POST") == 0 ||
          strcmp(this->method, "PUT") == 0) && !content_length_present)) {
        free(storage);
        this->storage = nullptr;
        return 400;
    }
    if (this->streaming_body) { _owns_storage = true; return 0; }
    if (content_length > MAX_BODY) {
        free(storage);
        this->storage = nullptr;
        return 413;
    }
    if (header_len + content_length + 1 > capacity) {
        char *expanded = (char*)(realloc(storage, header_len + content_length + 1));
        if (expanded == NULL) {
            free(storage);
            this->storage = nullptr;
            return 500;
        }
        storage = expanded;
        capacity = header_len + content_length + 1;
        this->storage = storage;
        this->body = storage + header_len;
    }
    while (used < header_len + content_length) {
        ssize_t got = recv(fd, storage + used, capacity - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(storage);
            this->storage = nullptr;
            return RECEIVE_INCOMPLETE;
        }
        if (got == 0) {
            free(storage);
            this->storage = nullptr;
            return RECEIVE_INCOMPLETE;
        }
        used += (size_t)got;
    }
    this->body_len = content_length;
    this->body[content_length] = '\0';
    _owns_storage = true;
    return 0;
}
