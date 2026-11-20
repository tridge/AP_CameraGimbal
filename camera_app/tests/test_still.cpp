#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/metadata.h"
#include "camera_app/still.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint16_t read_u16(const unsigned char *data)
{
    return (uint16_t)(data[0] | (data[1] << 8));
}

static uint32_t read_u32(const unsigned char *data)
{
    return (uint32_t)(read_u16(data) | ((uint32_t)read_u16(data + 2) << 16));
}

/* returns the entry for tag in the IFD at ifd_offset, or NULL */
static const unsigned char *find_tag(const unsigned char *tiff,
                                     uint32_t ifd_offset, uint16_t tag)
{
    uint16_t count = read_u16(tiff + ifd_offset);
    for (uint16_t i = 0; i < count; i++) {
        const unsigned char *entry = tiff + ifd_offset + 2U + i * 12U;
        if (read_u16(entry) == tag) return entry;
    }
    return NULL;
}

static const unsigned char *entry_data(const unsigned char *tiff,
                                       const unsigned char *entry)
{
    static const size_t sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8};
    uint16_t type = read_u16(entry + 2);
    size_t size = sizes[type] * read_u32(entry + 4);
    return size <= 4U ? entry + 8 : tiff + read_u32(entry + 8);
}

static double rational(const unsigned char *data)
{
    return (double)read_u32(data) / (double)read_u32(data + 4);
}

static void check_segments(const struct ca_metadata *metadata,
                           const struct timespec *captured_at, bool position)
{
    unsigned char output[CA_METADATA_SEGMENTS_MAX];
    size_t length = ca_metadata_jpeg_segments(metadata, captured_at, output,
                                              sizeof(output));
    const unsigned char *tiff = output + 4 + 6;
    const unsigned char *entry;
    size_t exif_length;
    uint32_t exif_ifd, gps_ifd;
    char xmp[2048];
    const char *packet;

    assert(length > 0U);
    assert(output[0] == 0xff && output[1] == 0xe1);
    exif_length = (size_t)((output[2] << 8) | output[3]) + 2U;
    assert(memcmp(output + 4, "Exif\0\0II\x2a\0\x08\0\0\0", 14) == 0);

    entry = find_tag(tiff, 8, 0x0110);
    assert(entry != NULL);
    assert(strcmp((const char *)entry_data(tiff, entry), "A8 mini") == 0);
    entry = find_tag(tiff, 8, 0x8769);
    assert(entry != NULL);
    exif_ifd = read_u32(entry + 8);
    entry = find_tag(tiff, exif_ifd, 0x9003);
    assert(entry != NULL);
    assert(strlen((const char *)entry_data(tiff, entry)) == 19U);
    entry = find_tag(tiff, exif_ifd, 0x9291);
    assert(entry != NULL);
    assert(strcmp((const char *)entry_data(tiff, entry), "123") == 0);
    entry = find_tag(tiff, exif_ifd, 0xa404);
    assert(entry != NULL);
    assert(fabs(rational(entry_data(tiff, entry)) - 2.5) < 1e-6);

    entry = find_tag(tiff, 8, 0x8825);
    if (!position) {
        assert(entry == NULL);
    } else {
        const unsigned char *coordinate;
        gps_ifd = read_u32(entry + 8);
        entry = find_tag(tiff, gps_ifd, 0x0001);
        assert(entry != NULL && entry_data(tiff, entry)[0] == 'S');
        entry = find_tag(tiff, gps_ifd, 0x0002);
        assert(entry != NULL);
        coordinate = entry_data(tiff, entry);
        assert(fabs(rational(coordinate) + rational(coordinate + 8) / 60.0 +
                    rational(coordinate + 16) / 3600.0 - 35.3627521) < 1e-7);
        entry = find_tag(tiff, gps_ifd, 0x0003);
        assert(entry != NULL && entry_data(tiff, entry)[0] == 'E');
        entry = find_tag(tiff, gps_ifd, 0x0004);
        assert(entry != NULL);
        coordinate = entry_data(tiff, entry);
        assert(fabs(rational(coordinate) + rational(coordinate + 8) / 60.0 +
                    rational(coordinate + 16) / 3600.0 - 149.1652374) < 1e-7);
        entry = find_tag(tiff, gps_ifd, 0x0006);
        assert(entry != NULL);
        assert(fabs(rational(entry_data(tiff, entry)) - 612.5) < 1e-6);
        entry = find_tag(tiff, gps_ifd, 0x0011);
        assert(entry != NULL);
        /* vehicle yaw 90 + gimbal yaw -30 */
        assert(fabs(rational(entry_data(tiff, entry)) - 60.0) < 0.02);
        entry = find_tag(tiff, gps_ifd, 0x001d);
        assert(entry != NULL);
        assert(strcmp((const char *)entry_data(tiff, entry), "2026:08:30") == 0);
    }

    assert(output[exif_length] == 0xff && output[exif_length + 1] == 0xe1);
    assert(length - exif_length - 4U < sizeof(xmp));
    memcpy(xmp, output + exif_length + 4, length - exif_length - 4U);
    xmp[length - exif_length - 4U] = '\0';
    assert(strncmp(xmp, "http://ns.adobe.com/xap/1.0/", 28) == 0);
    /* the packet follows the NUL-terminated namespace */
    packet = xmp + 29;
    assert(strstr(packet, "<?xpacket end=\"w\"?>") != NULL);
    assert(strstr(packet, "apcg:CaptureUnixTime=\"1788069600.123\"") != NULL);
    assert(strstr(packet, "apcg:Zoom=\"2.5\"") != NULL);
    assert(strstr(packet, "drone-dji:GimbalPitchDegree=\"-45.00\"") != NULL);
    assert(strstr(packet, "drone-dji:FlightYawDegree=\"+90.00\"") != NULL);
    assert(strstr(packet, "drone-dji:GimbalYawDegree=\"+60.00\"") != NULL);
    assert((strstr(packet, "drone-dji:GpsLatitude=\"-35.3627521\"") != NULL) ==
           position);
    assert((strstr(packet, "drone-dji:RelativeAltitude=\"+40.20\"") != NULL) ==
           position);
}

int main(void)
{
    static const unsigned char jpeg[] = {0xff, 0xd8, 1, 2, 3, 0xff, 0xd9};
    struct timespec captured_at = {.tv_sec = 1788069600, .tv_nsec = 123000000};
    struct ca_metadata metadata;
    char root[] = "/tmp/camera-app-still-XXXXXX";
    char path[512];
    char directory[512];
    char bad_path[32] = "unchanged";
    struct stat status;
    unsigned char actual[CA_METADATA_SEGMENTS_MAX + sizeof(jpeg)];
    ssize_t actual_length;
    size_t segments_length;
    int fd;

    setenv("TZ", "UTC", 1);
    tzset();

    ca_metadata_set_model("A8 mini");
    ca_metadata_set_zoom(2.5f);
    ca_metadata_set_vehicle_attitude(0.1f, -0.05f, (float)(M_PI / 2.0));
    ca_metadata_set_gimbal_attitude(0.0f, (float)(-M_PI / 4.0),
                                    (float)(-M_PI / 6.0));
    ca_metadata_snapshot(&metadata);
    assert(!metadata.have_position);
    assert(metadata.have_vehicle_attitude && metadata.have_gimbal_attitude);
    check_segments(&metadata, &captured_at, false);

    ca_metadata_set_position(-353627521, 1491652374, 612.5f, 40.2f, 1.0f);
    ca_metadata_snapshot(&metadata);
    assert(metadata.have_position);
    check_segments(&metadata, &captured_at, true);

    assert(mkdtemp(root) != NULL);
    assert(ca_still_write_jpeg(root, 'C', jpeg, sizeof(jpeg), &captured_at,
                               path, sizeof(path)) == 0);
    assert(strstr(path, "_123_C.jpg") != NULL);
    assert(stat(path, &status) == 0);
    assert(status.st_mtim.tv_sec == captured_at.tv_sec);
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    actual_length = read(fd, actual, sizeof(actual));
    assert(actual_length > (ssize_t)sizeof(jpeg));
    assert(close(fd) == 0);
    assert(status.st_size == actual_length);
    /* SOI, EXIF APP1, XMP APP1, then the original body */
    assert(actual[0] == 0xff && actual[1] == 0xd8);
    assert(actual[2] == 0xff && actual[3] == 0xe1);
    segments_length = (size_t)actual_length - sizeof(jpeg);
    assert(memcmp(actual + 2 + segments_length, jpeg + 2, sizeof(jpeg) - 2U) == 0);

    errno = 0;
    assert(ca_still_write_jpeg(root, 'Z', (const unsigned char *)"bad", 3,
                               &captured_at, bad_path, sizeof(bad_path)) < 0);
    assert(errno == EINVAL);

    assert(strlen(path) < sizeof(directory));
    strcpy(directory, path);
    *strrchr(directory, '/') = '\0';
    assert(unlink(path) == 0);
    assert(rmdir(directory) == 0);
    assert(rmdir(root) == 0);
    puts("still JPEG tests passed");
    return 0;
}
