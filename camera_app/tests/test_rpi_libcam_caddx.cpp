/* rpi_libcam_caddx image control mapping and CADDX gimbal; needs no libcamera. */
#include "../src/backends/rpi_libcam/pipeline.h"
#include "../src/backends/caddx/caddx.h"
#include "camera_app/backend.h"
#include "apcam/network.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.0001f); }

static bool recording_get(void *opaque) { return *static_cast<bool *>(opaque); }

static const float DEG = 3.14159265358979323846f / 180.0f;

/* [network] video1_uri: what apcam_video_uri_valid() accepts and rejects. */
static void video_uri_validation(void)
{
    const char *valid[] = {
        "rtsp://10.0.0.5:8554/video1", "rtsps://pi.local/stream", "rtsp://pi-1.example.com",
        "rtsp://192.168.1.2:1/a/b.264", "rtsp://host:65535/",
    };
    const char *invalid[] = {
        "", "10.0.0.5", "http://10.0.0.5/video1", "RTSP://10.0.0.5/video1", "rtsp://",
        "rtsp:///video1", "rtsp://:8554/video1", "rtsp://host:/video1", "rtsp://host:0/video1",
        "rtsp://host:65536/video1", "rtsp://host:123456/video1", "rtsp://host:80x/video1",
        "rtsp://user:pass@host/video1", "rtsp://host?x=1", "rtsp://host/has space",
        "rtsp://host/\"quoted\"", "rtsp://host/caf\xc3\xa9", "rtsp://[::1]:8554/video1",
    };
    for (const char *uri : valid) assert(apcam_video_uri_valid(uri));
    for (const char *uri : invalid) assert(!apcam_video_uri_valid(uri));
    char longest[APCAM_VIDEO_URI_SIZE + 1];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest, "rtsp://h/", 9);
    longest[APCAM_VIDEO_URI_SIZE - 1] = '\0';
    assert(apcam_video_uri_valid(longest));
    longest[APCAM_VIDEO_URI_SIZE - 1] = 'a';
    longest[APCAM_VIDEO_URI_SIZE] = '\0';
    assert(!apcam_video_uri_valid(longest));
}

/* [network] uart_device_override: what apcam_uart_device_override_valid() accepts and rejects, and the ini key. */
static void uart_device_override(void)
{
    const char *valid[] = {
        "", "/dev/ttyAMA0", "/dev/serial0", "/dev/serial/by-id/usb-x_y-if00",
        "udp://192.168.144.15:15002", "udp://localhost:1", "udp://10.0.0.1:65535",
        "tcp://192.168.144.15:15002", "tcp://localhost:5760",
    };
    const char *invalid[] = {
        "/dev/", "/dev/../etc/passwd", "/etc/passwd", "ttyAMA0", "/dev/tty AMA0", "udp://",
        "udp://192.168.144.15", "udp://192.168.144.15:", "udp://192.168.144.15:0",
        "udp://192.168.144.15:65536", "udp://192.168.144.15:123456", "udp://example.com:15002",
        "udp://300.1.1.1:5", "udp://192.168.144.15:80x", "tcp://", "tcp://192.168.144.15",
        "tcp://example.com:15002", "tcp://192.168.144.15:0", "http://192.168.144.15:80",
    };
    for (const char *link : valid) assert(apcam_uart_device_override_valid(link));
    for (const char *link : invalid) assert(!apcam_uart_device_override_valid(link));
    static const struct { const char *ini; bool loads; const char *value; } cases[] = {
        {"[network]\ncapture = false\n", true, ""},
        {"[network]\nuart_device_override = \"\"\n", true, ""},
        {"[network]\nuart_device_override = /dev/ttyAMA1\n", true, "/dev/ttyAMA1"},
        {"[network]\nuart_device_override = udp://192.168.144.15:15002\n", true, "udp://192.168.144.15:15002"},
        {"[network]\nuart_device_override = http://192.168.144.15\n", false, NULL},
        {"[network]\nuart_device_override = /dev/../etc/passwd\n", false, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[] = "/tmp/camera-uart-override-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        size_t length = strlen(cases[i].ini);
        assert(write(fd, cases[i].ini, length) == (ssize_t)length);
        close(fd);
        struct ca_config config;
        ca_config_defaults(&config);
        char error[256] = "";
        assert((ca_config_load(&config, path, error, sizeof(error)) == 0) == cases[i].loads);
        if (cases[i].loads) assert(strcmp(config.uart_device_override, cases[i].value) == 0);
        unlink(path);
    }
}

/* Reference packets from ArduPilot's AP_Mount_CADDX encoding (roll/pitch locked). */
static void caddx_packets(void)
{
    static const uint8_t neutral[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0x6e};
    static const uint8_t down45_right90[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x40, 0x72, 0xa5};
    static const uint8_t down90_left45[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0c, 0xe0, 0xa1, 0x2d};
    uint8_t packet[10];
    ca_caddx_pack(packet, 0, 0, 0);
    assert(memcmp(packet, neutral, sizeof(packet)) == 0);
    ca_caddx_pack(packet, 0, -45 * DEG, 90 * DEG);
    assert(memcmp(packet, down45_right90, sizeof(packet)) == 0);
    ca_caddx_pack(packet, 0, -90 * DEG, -45 * DEG);
    assert(memcmp(packet, down90_left45, sizeof(packet)) == 0);
    /* A full turn wraps to zero rather than overflowing 12 bits. */
    ca_caddx_pack(packet, 0, 0, 359.99f * DEG);
    assert(memcmp(packet, neutral, sizeof(packet)) == 0);
}

/* A closed passthrough port, as while the flight controller reboots, raises
 * ICMP errors; sending resumes once the port is listening again. */
static void caddx_udp_unreachable(void)
{
    int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    close(server);
    char device[64];
    snprintf(device, sizeof(device), "udp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_set_gimbal_angles(backend, 0, 0) == 0);
    for (unsigned i = 0; i < 10; i++) {
        ca_backend_periodic(backend);
        usleep(25000);
    }
    assert(ca_backend_fd(backend) < 0);

    server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    struct pollfd item = {.fd = server, .events = POLLIN};
    for (unsigned i = 0; i < 20 && poll(&item, 1, 0) == 0; i++) {
        ca_backend_periodic(backend);
        usleep(25000);
    }
    uint8_t packet[16];
    assert(poll(&item, 1, 0) == 1 && recv(server, packet, sizeof(packet), 0) == 10);
    assert(packet[0] == 0xa5 && packet[1] == 0x5a);
    ca_backend_close(backend);
    close(server);
}

/* The UDP transport used through a flight controller's serial passthrough. */
static void caddx_udp(void)
{
    int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && bind(server, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    char device[64];
    snprintf(device, sizeof(device), "udp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    /* The main loop must not poll a UDP link (see caddx_udp_unreachable). */
    assert(ca_backend_fd(backend) < 0);
    assert(ca_backend_has_gimbal(backend));

    /* Nothing is sent, and no attitude reported, before the first command. */
    struct ca_gimbal_attitude attitude;
    ca_backend_periodic(backend);
    struct pollfd item = {.fd = server, .events = POLLIN};
    assert(poll(&item, 1, 50) == 0);
    assert(!ca_backend_gimbal_attitude(backend, &attitude));

    assert(ca_backend_set_gimbal_angles(backend, -45 * DEG, 90 * DEG) == 0);
    ca_backend_periodic(backend);
    uint8_t packet[16];
    assert(poll(&item, 1, 1000) == 1);
    assert(recv(server, packet, sizeof(packet), 0) == 10);
    static const uint8_t expected[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x40, 0x72, 0xa5};
    assert(memcmp(packet, expected, sizeof(expected)) == 0);
    assert(ca_backend_gimbal_attitude(backend, &attitude));
    assert(fabsf(attitude.pitch_rad + 45 * DEG) < 1e-5f && fabsf(attitude.yaw_rad - 90 * DEG) < 1e-5f);

    /* Targets are limited to the gimbal's range. */
    assert(ca_backend_set_gimbal_angles(backend, -120 * DEG, 0) == 0);
    assert(ca_backend_gimbal_attitude(backend, &attitude));
    assert(fabsf(attitude.pitch_rad + 90 * DEG) < 1e-5f);
    errno = 0;
    assert(ca_backend_set_gimbal_angles(backend, NAN, 0) < 0 && errno == EINVAL);
    ca_backend_close(backend);
    close(server);
}

/* The TCP transport: connects when the first command is due, sends the same packet,
 * and reconnects after the peer closes the connection. */
static void caddx_tcp(void)
{
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(server >= 0 && setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    assert(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(server, 2) == 0);
    assert(getsockname(server, (struct sockaddr *)&address, &length) == 0);
    char device[64];
    snprintf(device, sizeof(device), "tcp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_fd(backend) < 0); /* serviced by periodic(), like UDP */
    assert(ca_backend_has_gimbal(backend));
    assert(ca_backend_set_gimbal_angles(backend, -45 * DEG, 90 * DEG) == 0);

    static const uint8_t expected[10] = {0xa5, 0x5a, 0x03, 0x00, 0x00, 0x00, 0x0e, 0x40, 0x72, 0xa5};
    for (int round = 0; round < 2; round++) {
        struct pollfd item = {.fd = server, .events = POLLIN};
        int client = -1;
        for (int i = 0; i < 300 && client < 0; i++) {
            ca_backend_periodic(backend);
            if (poll(&item, 1, 10) == 1) client = accept(server, NULL, NULL);
        }
        assert(client >= 0);
        uint8_t packet[16];
        ssize_t got = -1;
        struct pollfd data = {.fd = client, .events = POLLIN};
        for (int i = 0; i < 300 && got < 0; i++) {
            ca_backend_periodic(backend);
            if (poll(&data, 1, 10) == 1) got = recv(client, packet, sizeof(packet), 0);
        }
        assert(got >= 10 && memcmp(packet, expected, sizeof(expected)) == 0);
        close(client); /* the next round must reconnect */
    }
    ca_backend_close(backend);
    close(server);
}

/* A TCP peer that is not listening yet must not stop the backend opening: it connects
 * once the peer appears. */
static void caddx_tcp_late_server(void)
{
    int one = 1;
    struct sockaddr_in address = {};
    socklen_t length = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int probe = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(probe >= 0 && bind(probe, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(probe, (struct sockaddr *)&address, &length) == 0);
    close(probe); /* nothing listens on this port yet */
    char device[64];
    snprintf(device, sizeof(device), "tcp://127.0.0.1:%u", ntohs(address.sin_port));

    struct ca_backend_config config = {};
    config.uart_device = device;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_has_gimbal(backend));
    assert(ca_backend_set_gimbal_angles(backend, -45 * DEG, 90 * DEG) == 0);
    for (int i = 0; i < 5; i++) ca_backend_periodic(backend); /* refused; nothing to send to */

    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(server >= 0 && setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    assert(bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 && listen(server, 1) == 0);
    struct pollfd item = {.fd = server, .events = POLLIN};
    int client = -1;
    for (int i = 0; i < 400 && client < 0; i++) {
        ca_backend_periodic(backend);
        if (poll(&item, 1, 10) == 1) client = accept(server, NULL, NULL);
    }
    assert(client >= 0);
    uint8_t packet[16];
    ssize_t got = -1;
    struct pollfd data = {.fd = client, .events = POLLIN};
    for (int i = 0; i < 300 && got < 0; i++) {
        ca_backend_periodic(backend);
        if (poll(&data, 1, 10) == 1) got = recv(client, packet, sizeof(packet), 0);
    }
    assert(got >= 10 && packet[0] == 0xa5 && packet[1] == 0x5a);
    close(client);
    close(server);
    ca_backend_close(backend);

    /* An address that cannot be parsed is still a configuration error. */
    config.uart_device = "tcp://not-an-address:1";
    assert(ca_backend_open(&backend, &config) < 0);
}

/* With one stream the SupportProxy video2 port and name are not offered, so a stored
 * video2 port must not make a valid save (for example a video1 port equal to it)
 * unloadable. The video1 rules still apply. */
static void single_stream_support_proxy(void)
{
    static const struct { const char *ini; bool loads; } cases[] = {
        /* A video1 port equal to the stored video2 port, and an empty video2 name. */
        {"[support_proxy]\nenabled = true\nhost = localhost\nvideo1_port = 40002\n"
         "video1_name = video1\nvideo2_port = 40002\n", true},
        {"[support_proxy]\nenabled = true\nhost = localhost\nvideo1_port = 40001\n"
         "video1_name = video1\nvideo2_port = 40002\nvideo2_name =\n", true},
        /* The video1 name and the other SupportProxy rules still apply. */
        {"[support_proxy]\nenabled = true\nhost = localhost\nvideo1_port = 40001\nvideo1_name =\n", false},
        {"[support_proxy]\nenabled = true\nhost = localhost\nsigning = true\n", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[] = "/tmp/camera-support-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        size_t length = strlen(cases[i].ini);
        assert(write(fd, cases[i].ini, length) == (ssize_t)length);
        close(fd);
        struct ca_config config;
        ca_config_defaults(&config);
        char error[256] = "";
        assert((ca_config_load(&config, path, error, sizeof(error)) == 0) == cases[i].loads);
        unlink(path);
    }
}

/* [network] video1_uri: blank or an rtsp:// or rtsps:// URI. */
static void video1_uri(void)
{
    static const struct { const char *ini; bool loads; const char *value; } cases[] = {
        {"[network]\ncapture = false\n", true, ""},
        {"[network]\nvideo1_uri = \"\"\n", true, ""},
        {"[network]\nvideo1_uri = rtsp://10.0.2.100:8554/video1\n", true, "rtsp://10.0.2.100:8554/video1"},
        {"[network]\nvideo1_uri = rtsps://pi.local/camera\n", true, "rtsps://pi.local/camera"},
        {"[network]\nvideo1_uri = 10.0.2.100\n", false, NULL},
        {"[network]\nvideo1_uri = http://10.0.2.100/video1\n", false, NULL},
        {"[network]\nvideo1_uri = rtsp://user:pw@10.0.2.100/video1\n", false, NULL},
        {"[network]\nvideo1_uri = rtsp://10.0.2.100:99999/video1\n", false, NULL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[] = "/tmp/camera-video-uri-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        size_t length = strlen(cases[i].ini);
        assert(write(fd, cases[i].ini, length) == (ssize_t)length);
        close(fd);
        struct ca_config config;
        ca_config_defaults(&config);
        char error[256] = "";
        assert((ca_config_load(&config, path, error, sizeof(error)) == 0) == cases[i].loads);
        if (cases[i].loads) assert(strcmp(config.video1_uri, cases[i].value) == 0);
        unlink(path);
    }
}

/* VIDEO_MAIN_KBPS and VIDEO_MAIN_FPS: defaults, range checks and the ini keys. */
static void video_encoder_settings(void)
{
    struct ca_config c;
    ca_config_defaults(&c);
    assert(c.main_bitrate_kbps == 4096 && c.main_frame_rate == 30);
    int bitrate = ca_config_param_find("VIDEO_MAIN_KBPS");
    int rate = ca_config_param_find("VIDEO_MAIN_FPS");
    assert(bitrate >= 0 && rate >= 0);
    assert(ca_config_param_assign(&c, (size_t)bitrate, 8000) == 0 && c.main_bitrate_kbps == 8000);
    assert(ca_config_param_assign(&c, (size_t)bitrate, 999) < 0 && c.main_bitrate_kbps == 8000);
    assert(ca_config_param_assign(&c, (size_t)bitrate, 20001) < 0 && c.main_bitrate_kbps == 8000);
    assert(ca_config_param_assign(&c, (size_t)rate, 15) == 0 && c.main_frame_rate == 15);
    assert(ca_config_param_assign(&c, (size_t)rate, 4) < 0 && c.main_frame_rate == 15);
    assert(ca_config_param_assign(&c, (size_t)rate, 31) < 0 && c.main_frame_rate == 15);

    char path[] = "/tmp/camera-video-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "[stream.main]\nbitrate_kbps = 12000\nframe_rate = 25\n";
    assert(write(fd, ini, sizeof(ini) - 1) == (ssize_t)(sizeof(ini) - 1));
    close(fd);
    struct ca_config loaded;
    ca_config_defaults(&loaded);
    char error[256] = "";
    assert(ca_config_load(&loaded, path, error, sizeof(error)) == 0);
    assert(loaded.main_bitrate_kbps == 12000 && loaded.main_frame_rate == 25);
    unlink(path);
}

/* The main resolutions on offer: names, sizes and the ini key. */
static void main_resolutions(void)
{
    static const struct { enum ca_video_resolution id; const char *name; unsigned width, height; } sizes[] = {
        {CA_VIDEO_1080P, "1920x1080", 1920, 1080}, {CA_VIDEO_720P, "1280x720", 1280, 720},
        {CA_VIDEO_540P, "960x540", 960, 540}, {CA_VIDEO_480P, "854x480", 854, 480},
        {CA_VIDEO_360P, "640x360", 640, 360}, {CA_VIDEO_240P, "426x240", 426, 240},
        {CA_VIDEO_180P, "320x180", 320, 180},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned width = 0, height = 0;
        ca_video_resolution_size(sizes[i].id, &width, &height);
        assert(width == sizes[i].width && height == sizes[i].height);
        assert(strcmp(ca_video_resolution_name(sizes[i].id), sizes[i].name) == 0);
        char path[] = "/tmp/camera-res-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        char ini[64];
        int n = snprintf(ini, sizeof(ini), "[stream.main]\nresolution = %s\n", sizes[i].name);
        assert(write(fd, ini, (size_t)n) == n);
        close(fd);
        struct ca_config loaded;
        ca_config_defaults(&loaded);
        char error[256] = "";
        assert(ca_config_load(&loaded, path, error, sizeof(error)) == 0);
        assert(loaded.main_resolution == sizes[i].id);
        unlink(path);
    }
    /* The encoder stops at 1080p, so larger sizes are not accepted. */
    char path[] = "/tmp/camera-res-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char ini[] = "[stream.main]\nresolution = 3840x2160\n";
    assert(write(fd, ini, sizeof(ini) - 1) == (ssize_t)(sizeof(ini) - 1));
    close(fd);
    struct ca_config rejected;
    ca_config_defaults(&rejected);
    char error[256] = "";
    assert(ca_config_load(&rejected, path, error, sizeof(error)) < 0);
    unlink(path);
}

int main(void)
{
    main_resolutions();
    single_stream_support_proxy();
    video_encoder_settings();
    video1_uri();
    uart_device_override();
    struct ca_config settings;
    ca_config_defaults(&settings);
    struct ca_rpi_image_controls c;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, 0); near(c.contrast, 1); near(c.saturation, 1);
    near(c.exposure_value, 0);
    assert(c.analogue_gain == 0 && c.exposure_us == 0 && c.mode == CA_AE_AUTO);

    settings.brightness = 100; settings.contrast = 0; settings.saturation = 75;
    settings.exposure_compensation = -10;
    settings.iso = CA_ISO_400;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, 1); near(c.contrast, 0); near(c.saturation, 1.5f);
    near(c.exposure_value, -1);
    near(c.analogue_gain, 4);
    assert(c.exposure_us == 0 && c.mode == CA_AE_GAIN_PRIORITY);

    settings.brightness = 0; settings.iso = CA_ISO_100; settings.shutter = CA_SHUTTER_1_1000;
    ca_rpi_image_controls(&settings, &c);
    near(c.brightness, -1); near(c.analogue_gain, 1);
    assert(c.exposure_us == 1000 && c.mode == CA_AE_MANUAL);

    settings.iso = CA_ISO_AUTO; settings.shutter = CA_SHUTTER_1_30;
    ca_rpi_image_controls(&settings, &c);
    assert(c.analogue_gain == 0 && c.exposure_us == 33333 && c.mode == CA_AE_SHUTTER_PRIORITY);
    settings.iso = CA_ISO_3200; settings.shutter = CA_SHUTTER_1_2000;
    ca_rpi_image_controls(&settings, &c);
    near(c.analogue_gain, 32); assert(c.exposure_us == 500);

    bool recording = true;
    struct ca_backend_config config = {};
    config.recording_get = recording_get;
    config.recording_opaque = &recording;
    struct ca_backend *backend = NULL;
    assert(ca_backend_open(&backend, &config) == 0);
    assert(ca_backend_fd(backend) < 0);
    assert(!ca_backend_has_gimbal(backend));
    assert(ca_backend_handle_fd(backend) == 0);
    struct ca_gimbal_attitude attitude;
    assert(!ca_backend_gimbal_attitude(backend, &attitude));
    errno = 0;
    assert(ca_backend_set_gimbal_angles(backend, 0, 0) < 0 && errno == ENOTSUP);
    assert(ca_backend_set_gimbal_rates(backend, 0, 0) < 0 && errno == ENOTSUP);
    assert(ca_backend_recording(backend));
    recording = false;
    assert(!ca_backend_recording(backend));
    ca_backend_periodic(backend);
    ca_backend_close(backend);
    video_uri_validation();
    caddx_packets();
    caddx_udp();
    caddx_udp_unreachable();
    caddx_tcp();
    caddx_tcp_late_server();
    puts("rpi_libcam_caddx image controls and CADDX gimbal passed");
    return 0;
}
