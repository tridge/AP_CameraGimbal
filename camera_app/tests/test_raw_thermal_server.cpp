#define _POSIX_C_SOURCE 200809L
#include "camera_app/raw_thermal_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WIDTH 16U
#define HEIGHT 8U

static void receive_all(int fd, uint8_t *data, size_t length)
{
    while (length != 0U) {
        ssize_t received = recv(fd, data, length, 0);
        if (received < 0 && errno == EINTR) continue;
        assert(received > 0);
        data += (size_t)received;
        length -= (size_t)received;
    }
}

int main(void)
{
    struct ca_raw_thermal_server *server = NULL;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    const struct timespec captured_at = {
        .tv_sec = 1700000000,
        .tv_nsec = 123456789,
    };
    uint16_t pixels[WIDTH * HEIGHT];
    const size_t frame_bytes = sizeof(pixels);
    const size_t response_bytes = CA_RAW_THERMAL_HEADER_BYTES + frame_bytes;
    uint8_t *response = (uint8_t*)(malloc(response_bytes));
    double timestamp = 0.0;
    int fd;

    assert(setenv("TZ", "UTC", 1) == 0);
    tzset();
    assert(response != NULL);
    for (size_t i = 0; i < WIDTH * HEIGHT; i++) {
        pixels[i] = (uint16_t)(18000U + i);
    }
    assert(ca_raw_thermal_server_open(&server, "/mnt/DCIM/capture", 0U,
                                      WIDTH, HEIGHT) == 0);
    assert(ca_raw_thermal_server_port(server) != 0U);
    assert(ca_raw_thermal_server_publish(server, pixels, &captured_at) == 0);
    address.sin_port = htons((uint16_t)ca_raw_thermal_server_port(server));
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    assert(connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0);
    receive_all(fd, response, response_bytes);
    assert(recv(fd, response, 1U, 0) == 0);
    close(fd);

    assert(strcmp((const char *)response,
                  "/mnt/DCIM/capture/2023-11-14/"
                  "2023-11-14_22-13-20_123_I.bin") == 0);
    memcpy(&timestamp, response + CA_RAW_THERMAL_FILENAME_BYTES,
           sizeof(timestamp));
    assert(timestamp > 1700000000.123456 && timestamp < 1700000000.123457);
    assert(memcmp(response + CA_RAW_THERMAL_HEADER_BYTES, pixels,
                  frame_bytes) == 0);
    ca_raw_thermal_server_source_timeout(server,1);
    usleep(10000); // expiring input must not serve a cached sensor frame
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    assert(connect(fd,(const sockaddr*)&address,sizeof(address))==0);
    assert(recv(fd,response,1,0)==0);close(fd);
    // Reusing the sender snapshot must pick up a newly published frame, even
    // after an expired request sent no data.
    for(size_t i=0;i<WIDTH*HEIGHT;i++)pixels[i]^=0xffff;
    ca_raw_thermal_server_source_timeout(server,0);
    assert(ca_raw_thermal_server_publish(server,pixels,&captured_at)==0);
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    assert(connect(fd,(const sockaddr*)&address,sizeof(address))==0);
    receive_all(fd,response,response_bytes);
    assert(recv(fd,response,1,0)==0);close(fd);
    assert(memcmp(response+CA_RAW_THERMAL_HEADER_BYTES,pixels,frame_bytes)==0);
    ca_raw_thermal_server_close(server);
    free(response);
    puts("raw thermal TCP server tests passed");
    return 0;
}
