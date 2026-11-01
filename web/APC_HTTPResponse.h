#pragma once
#include "APC_StringBuffer.h"
#include <sys/socket.h>
#include <cerrno>
// Response framing and browser security policy share one owner/boundary.
// The connection remains owned by the server or its streaming child.
class APC_HTTPResponse {
public:
    APC_HTTPResponse(int fd, const char *server_name) : _fd(fd), _server_name(server_name) {}
    void send(int status, const char *reason, const char *content_type,
              const char *body, size_t body_len, const char *extra_headers) const;
private:
    static bool send_all(int fd, const void *buffer, size_t length)
    {
        const char *cursor = static_cast<const char *>(buffer);
        while (length) {
            const ssize_t sent = ::send(fd, cursor, length, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            cursor += sent;
            length -= size_t(sent);
        }
        return true;
    }
    int _fd;
    const char *_server_name;
};
inline void APC_HTTPResponse::send(int status, const char *reason,
                          const char *content_type, const char *body,
                          size_t body_len, const char *extra_headers) const
{
    APC_StringBuffer header;

    header.reset();
    if (header.appendf("HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' https://firmware.ardupilot.org; media-src 'self' blob:; "
        "script-src 'self'; connect-src 'self'; form-action 'self'; "
        "frame-ancestors 'none'\r\n"
        "%s\r\n",
        status, reason, _server_name, content_type, body_len,
        extra_headers ? extra_headers : "") &&
        send_all(_fd, header.data(), header.size()) && body_len > 0) {
        (void)send_all(_fd, body, body_len);
    }
    header.reset();
}
