#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/live_video_server.h"
#include "camera_app/video_metadata.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int contains(const uint8_t *data, size_t length, const char text[4])
{
    for (size_t i = 0; i + 4U <= length; i++) {
        if (memcmp(data + i, text, 4U) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct ca_live_video_server *server = NULL;
    struct sockaddr_in address = {.sin_family = AF_INET};
    struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    struct stat st;
    uint8_t *h264 = NULL;
    uint8_t received[1024 * 1024];
    size_t used = 0;
    uint8_t selection = 1;
    int fd = -1;
    int input = -1;
    int result = 1;

    if (argc != 2 || stat(argv[1], &st) < 0 || st.st_size <= 0) return 2;
    {
    h264 = (uint8_t*)(malloc((size_t)st.st_size));
    input = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (h264 == NULL || input < 0 ||
        read(input, h264, (size_t)st.st_size) != st.st_size) goto done;
    close(input);
    input = -1;
    size_t h264_length = (size_t)st.st_size;
    size_t scan = 0U, nal, nal_length;
    while (ca_annexb_next(h264, h264_length, &scan, &nal, &nal_length)) {
        if (nal_length != 0U && (h264[nal] & 31U) == 9U && nal > 4U) {
            h264_length = nal - (h264[nal - 4U] == 0U ? 4U : 3U);
            break;
        }
    }
    if (ca_live_video_server_open(&server, 0U) < 0 ||
        ca_live_video_server_configure(server, 1U, 320U, 240U, 10U,
                                       true) < 0) goto done;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    address.sin_port = htons((uint16_t)ca_live_video_server_port(server));
    address.sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)};
    if (fd < 0 || connect(fd, (const struct sockaddr *)&address,
                          sizeof(address)) < 0 ||
        send(fd, &selection, 1U, 0) != 1) goto done;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (recv(fd, &selection, 1U, MSG_WAITALL) != 1 || selection != 0U) goto done;
    for (unsigned attempt = 0; attempt < 5U; attempt++) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
        (void)nanosleep(&delay, NULL);
        if (ca_live_video_server_publish(server, 1U, h264,
                                         h264_length, 0U, true, 88.0f) < 0) {
            goto done;
        }
        ssize_t bytes = recv(fd, received + used, sizeof(received) - used, 0);
        if (bytes > 0) used += (size_t)bytes;
        if (contains(received, used, "ftyp") &&
            contains(received, used, "moov") &&
            contains(received, used, "moof") &&
            contains(received, used, "mdat") &&
            memmem(received, used, "\"hfov_deg\":88.0000",
                   strlen("\"hfov_deg\":88.0000")) != NULL) {
            puts("PASS native fragmented-MP4 live server stream selection");
            result = 0;
            break;
        }
    }
    }
done:
    if (fd >= 0) close(fd);
    if (input >= 0) close(input);
    ca_live_video_server_close(server);
    free(h264);
    return result;
}
