#include "net/BufferWriter.h"
#include "net/SocketUtil.h"
#include <cassert>
#include <cstdio>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

int main()
{
    const char payload[] = "camera video packet";
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const timeval timeout{2, 0};
    assert(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    {
        xop::BufferWriter writer(2);
        assert(!writer.Append(payload, sizeof(payload), sizeof(payload)));
        assert(writer.Append(payload, sizeof(payload)));
        assert(writer.Append(payload, sizeof(payload), 1));
        assert(!writer.Append(payload, sizeof(payload)));
        assert(writer.Send(pair[0]) >= 0);
        assert(writer.IsEmpty());
        char received[2 * sizeof(payload)];
        assert(recv(pair[1], received, 2 * sizeof(payload) - 1, MSG_WAITALL) ==
               static_cast<int>(2 * sizeof(payload) - 1));
        assert(memcmp(received, payload, sizeof(payload)) == 0);
        assert(memcmp(received + sizeof(payload), payload + 1, sizeof(payload) - 1) == 0);
        // Also release an unsent copied buffer when the connection closes.
        assert(writer.Append(payload, sizeof(payload)));
    }
    close(pair[0]);
    close(pair[1]);
    const int tcp = socket(AF_INET, SOCK_STREAM, 0);
    assert(tcp >= 0);
    xop::SocketUtil::SetNoDelay(tcp);
    int enabled = 0;
    socklen_t length = sizeof(enabled);
    assert(getsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &enabled, &length) == 0);
    assert(enabled == 1);
    close(tcp);
    puts("PASS XOP copied-buffer ownership, queue draining and TCP_NODELAY");
}
