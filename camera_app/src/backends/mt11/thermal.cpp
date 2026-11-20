#include <new>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "thermal.h"

#include "camera_app/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <pthread.h>
#include <stdbool.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MT11_USB_VENDOR 0x0bdaU
#define MT11_USB_PRODUCT 0x5830U
#define MT11_UVC_CONTROL_INTERFACE 0
#define MT11_UVC_INTERFACE 1
#define MT11_UVC_ENDPOINT 0x81U
#define MT11_UVC_URB_COUNT 12U
#define MT11_UVC_URB_BYTES 1312256U
#define MT11_XU_VALUE 0x0078U
#define MT11_XU_COMMAND_FIRST 0x9d00U
#define MT11_XU_COMMAND_SECOND 0x1d08U
#define MT11_XU_STATUS 0x0200U
#define MT11_XU_RESULT 0x1d10U
#define MT11_XU_SHORT_COMMAND 0x1d00U
#define MT11_XU_SHORT_RESULT 0x1d08U
#define MT11_XU_TIMEOUT_MS 5000U

struct thermal_urb {
    struct usbdevfs_urb *urb;
    uint8_t *buffer;
    bool submitted;
};

struct ca_mt11_thermal {
    int fd;
    bool interface_claimed[MT11_UVC_INTERFACE + 1];
    bool kernel_driver_detached[MT11_UVC_INTERFACE + 1];
    pthread_t thread;
    bool thread_started;
    atomic_bool stop;
    pthread_mutex_t control_lock;
    bool control_lock_initialized;
    struct ca_mt11_uvc_assembler assembler;
    struct thermal_urb urbs[MT11_UVC_URB_COUNT];
};

static int claim_uvc_interface(struct ca_mt11_thermal *thermal,
                               unsigned interface)
{
    int interface_number = (int)interface;

    if (ioctl(thermal->fd, USBDEVFS_CLAIMINTERFACE, &interface_number) == 0) {
        thermal->interface_claimed[interface] = true;
        return 0;
    }
    if (errno != EBUSY) return -1;

    /* uvcvideo normally binds the thermal module during boot.  Claim each
     * interface atomically while detaching only that known driver; never
     * evict an unexpected owner. */
    struct usbdevfs_disconnect_claim claim = {
        .interface = interface,
        .flags = USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER,
    };
    snprintf(claim.driver, sizeof(claim.driver), "%s", "uvcvideo");
    if (ioctl(thermal->fd, USBDEVFS_DISCONNECT_CLAIM, &claim) < 0) {
        return -1;
    }
    thermal->interface_claimed[interface] = true;
    thermal->kernel_driver_detached[interface] = true;
    ca_log("thermal USB detached uvcvideo from interface %u",
           interface);
    return 0;
}

static void reconnect_uvc_interface(struct ca_mt11_thermal *thermal,
                                    unsigned interface)
{
    if (!thermal->kernel_driver_detached[interface] || thermal->fd < 0) return;

    struct usbdevfs_ioctl command = {
        .ifno = (int)interface,
        .ioctl_code = USBDEVFS_CONNECT,
        .data = NULL,
    };
    if (ioctl(thermal->fd, USBDEVFS_IOCTL, &command) < 0) {
        ca_log("thermal USB could not restore uvcvideo on interface %u: %s",
               interface, strerror(errno));
        return;
    }
    thermal->kernel_driver_detached[interface] = false;
    ca_log("thermal USB restored uvcvideo on interface %u",
           interface);
}

void ca_mt11_thermal_gain_get_command(uint8_t command[16])
{
    static const uint8_t get_gain[16] = {
        0x14, 0x85, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    };
    memcpy(command, get_gain, sizeof(get_gain));
}

void ca_mt11_thermal_gain_set_command(uint8_t command[16], uint8_t gain)
{
    static const uint8_t set_gain[16] = {
        0x14, 0xc5, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(command, set_gain, sizeof(set_gain));
    command[7] = gain;
}

void ca_mt11_thermal_palette_get_command(uint8_t command[16])
{
    static const uint8_t get_palette[16] = {
        0x09, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(command, get_palette, sizeof(get_palette));
}

void ca_mt11_thermal_palette_set_command(uint8_t command[16], uint8_t palette)
{
    static const uint8_t set_palette[16] = {
        0x09, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    memcpy(command, set_palette, sizeof(set_palette));
    /* The module palette numbering is one-based; SIYI is zero-based. */
    command[8] = (uint8_t)(palette + 1U);
}

static int xu_control(int fd, uint8_t request_type, uint8_t request,
                      uint16_t index, void *data, uint16_t length)
{
    struct usbdevfs_ctrltransfer transfer = {
        .bRequestType = request_type,
        .bRequest = request,
        .wValue = MT11_XU_VALUE,
        .wIndex = index,
        .wLength = length,
        .timeout = MT11_XU_TIMEOUT_MS,
        .data = data,
    };
    int result = ioctl(fd, USBDEVFS_CONTROL, &transfer);

    if (result < 0) return -1;
    if (result != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int stage_xu_command(int fd, const uint8_t command[16])
{
    if (xu_control(fd, 0x41U, 0x45U, MT11_XU_COMMAND_FIRST,
                   (void *)command, 8U) < 0 ||
        xu_control(fd, 0x41U, 0x45U, MT11_XU_COMMAND_SECOND,
                   (void *)(command + 8U), 8U) < 0) {
        return -1;
    }
    return 0;
}

static int wait_for_xu_command(int fd)
{
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 5000000L};

    for (unsigned elapsed = 0; elapsed < MT11_XU_TIMEOUT_MS; elapsed += 5U) {
        uint8_t status = 0xffU;

        if (xu_control(fd, 0xc1U, 0x44U, MT11_XU_STATUS, &status, 1U) < 0) {
            return -1;
        }
        if (status == 0U) return 0;
        if (status != 1U) {
            errno = EPROTO;
            return -1;
        }
        while (nanosleep(&interval, NULL) < 0) {
            if (errno != EINTR) return -1;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int lock_controls(struct ca_mt11_thermal *thermal)
{
    int result = pthread_mutex_lock(&thermal->control_lock);
    if (result == 0) return 0;
    errno = result;
    return -1;
}

static int unlock_controls(struct ca_mt11_thermal *thermal, int result)
{
    int saved_errno = errno;
    int unlock_result = pthread_mutex_unlock(&thermal->control_lock);

    if (result < 0) errno = saved_errno;
    else if (unlock_result != 0) {
        errno = unlock_result;
        result = -1;
    }
    return result;
}

static int32_t raw_to_centi_c(uint16_t raw)
{
    int32_t centi_c = (int32_t)(((uint32_t)raw * 100U + 32U) / 64U) - 27315;
    return centi_c;
}

bool ca_mt11_thermal_range_from_y16(const uint16_t *pixels, uint32_t width,
                                    uint32_t height,
                                    struct ca_thermal_range *range)
{
    size_t count;
    size_t minimum_index = 0;
    size_t maximum_index = 0;

    if (pixels == NULL || range == NULL || width == 0U || height == 0U ||
        width > UINT16_MAX || height > UINT16_MAX ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        errno = EINVAL;
        return false;
    }
    count = (size_t)width * height;
    for (size_t i = 1; i < count; i++) {
        if (pixels[i] < pixels[minimum_index]) minimum_index = i;
        if (pixels[i] > pixels[maximum_index]) maximum_index = i;
    }
    range->maximum_centi_c = raw_to_centi_c(pixels[maximum_index]);
    range->minimum_centi_c = raw_to_centi_c(pixels[minimum_index]);
    range->maximum_x = (uint16_t)(maximum_index % width);
    range->maximum_y = (uint16_t)(maximum_index / width);
    range->minimum_x = (uint16_t)(minimum_index % width);
    range->minimum_y = (uint16_t)(minimum_index / width);
    return true;
}

static int read_number(const char *directory, const char *name,
                       unsigned base, unsigned *value)
{
    char path[512];
    char text[32];
    char *end;
    int fd;
    ssize_t length;

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    length = read(fd, text, sizeof(text) - 1U);
    close(fd);
    if (length <= 0) return -1;
    text[length] = '\0';
    errno = 0;
    unsigned long parsed = strtoul(text, &end, base);
    if (errno != 0 || end == text || parsed > 0xffffffffUL) {
        errno = EINVAL;
        return -1;
    }
    *value = (unsigned)parsed;
    return 0;
}

static int find_usb_device(char *path, size_t path_size)
{
    const char *root = "/sys/bus/usb/devices";
    DIR *directory = opendir(root);
    struct dirent *entry;

    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char sysfs[512];
        unsigned vendor, product, bus, device;
        if (entry->d_name[0] == '.') continue;
        if (snprintf(sysfs, sizeof(sysfs), "%s/%s", root, entry->d_name) >=
            (int)sizeof(sysfs)) continue;
        if (read_number(sysfs, "idVendor", 16, &vendor) < 0 ||
            read_number(sysfs, "idProduct", 16, &product) < 0 ||
            vendor != MT11_USB_VENDOR || product != MT11_USB_PRODUCT ||
            read_number(sysfs, "busnum", 10, &bus) < 0 ||
            read_number(sysfs, "devnum", 10, &device) < 0) continue;
        if (snprintf(path, path_size, "/dev/bus/usb/%03u/%03u", bus,
                     device) >= (int)path_size) {
            closedir(directory);
            errno = ENAMETOOLONG;
            return -1;
        }
        closedir(directory);
        return 0;
    }
    closedir(directory);
    errno = ENODEV;
    return -1;
}

int ca_mt11_uvc_assembler_init(struct ca_mt11_uvc_assembler *assembler,
                               ca_mt11_thermal_frame_cb callback,
                               void *opaque)
{
    if (assembler == NULL || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(assembler, 0, sizeof(*assembler));
    assembler->frame = (uint8_t*)(malloc(CA_MT11_THERMAL_FRAME_BYTES));
    if (assembler->frame == NULL) return -1;
    assembler->fid = -1;
    assembler->callback = callback;
    assembler->opaque = opaque;
    return 0;
}

void ca_mt11_uvc_assembler_destroy(struct ca_mt11_uvc_assembler *assembler)
{
    if (assembler == NULL) return;
    free(assembler->frame);
    memset(assembler, 0, sizeof(*assembler));
    assembler->fid = -1;
}

static void deliver_frame(struct ca_mt11_uvc_assembler *assembler)
{
    if (assembler->used == CA_MT11_THERMAL_FRAME_BYTES &&
        assembler->have_received_at) {
        assembler->callback(
            assembler->frame,
            (const uint16_t *)(assembler->frame +
                CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT * 2U),
            &assembler->last_received_at,
            assembler->opaque);
    }
    assembler->used = 0;
}

int ca_mt11_uvc_assembler_consume(struct ca_mt11_uvc_assembler *assembler,
                                  const uint8_t *payload, size_t length,
                                  const struct timespec *received_at)
{
    uint8_t header_length;
    uint8_t flags;
    int fid;
    size_t data_length;

    if (assembler == NULL || assembler->frame == NULL || payload == NULL ||
        received_at == NULL || length < 2U) {
        errno = EINVAL;
        return -1;
    }
    header_length = payload[0];
    flags = payload[1];
    if (header_length < 2U || header_length > length) {
        errno = EPROTO;
        return -1;
    }
    if ((flags & 0x40U) != 0U) {
        assembler->used = 0;
        errno = EIO;
        return -1;
    }
    fid = flags & 1U;
    if (assembler->fid != -1 && fid != assembler->fid &&
        assembler->used != 0U) {
        deliver_frame(assembler);
    }
    assembler->fid = fid;
    assembler->last_received_at = *received_at;
    assembler->have_received_at = true;
    data_length = length - header_length;
    if (data_length > CA_MT11_THERMAL_FRAME_BYTES - assembler->used) {
        assembler->used = 0;
        errno = EMSGSIZE;
        return -1;
    }
    memcpy(assembler->frame + assembler->used, payload + header_length,
           data_length);
    assembler->used += data_length;
    if ((flags & 0x02U) != 0U) deliver_frame(assembler);
    return 0;
}

// Rows are assembled in a cached buffer and copied out whole, because the
// destination VB mapping is uncached and per-byte stores are very slow.
#define YUYV_CHUNK 1024U

static void yuyv_to_nv12(const uint8_t *source, uint32_t width,
                         uint32_t height, uint8_t *y, size_t y_stride,
                         uint8_t *uv, size_t uv_stride, bool rotate)
{
    uint8_t line[YUYV_CHUNK];

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *input = source + (size_t)row * width * 2U;
        uint32_t out_row = rotate ? height - 1U - row : row;
        uint8_t *output = y + (size_t)out_row * y_stride;
        for (uint32_t c0 = 0; c0 < width; c0 += YUYV_CHUNK) {
            uint32_t n = width - c0 < YUYV_CHUNK ? width - c0 : YUYV_CHUNK;
            for (uint32_t k = 0; k < n; k++) {
                line[rotate ? n - 1U - k : k] = input[(c0 + k) * 2U];
            }
            memcpy(output + (rotate ? width - c0 - n : c0), line, n);
        }
        if ((row & 1U) != 0U) continue;
        output = uv + (size_t)(out_row / 2U) * uv_stride;
        for (uint32_t c0 = 0; c0 < width; c0 += YUYV_CHUNK) {
            uint32_t n = width - c0 < YUYV_CHUNK ? width - c0 : YUYV_CHUNK;
            for (uint32_t k = 0; k < n; k += 2U) {
                uint32_t d = rotate ? n - 2U - k : k;
                line[d] = input[(c0 + k) * 2U + 1U];
                line[d + 1U] = input[(c0 + k) * 2U + 3U];
            }
            memcpy(output + (rotate ? width - c0 - n : c0), line, n);
        }
    }
}

void ca_mt11_yuyv_to_nv12(const uint8_t *source, uint32_t width,
                          uint32_t height, uint8_t *y, size_t y_stride,
                          uint8_t *uv, size_t uv_stride)
{
    yuyv_to_nv12(source, width, height, y, y_stride, uv, uv_stride, false);
}

void ca_mt11_yuyv_to_nv12_rotated_180(const uint8_t *source, uint32_t width,
                                      uint32_t height, uint8_t *y,
                                      size_t y_stride, uint8_t *uv,
                                      size_t uv_stride)
{
    yuyv_to_nv12(source, width, height, y, y_stride, uv, uv_stride, true);
}

static int uvc_control(int fd, uint8_t request_type, uint8_t request,
                       uint16_t value, uint8_t data[26])
{
    struct usbdevfs_ctrltransfer control = {
        .bRequestType = request_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = MT11_UVC_INTERFACE,
        .wLength = 26,
        .timeout = 1000,
        .data = data,
    };
    int result = ioctl(fd, USBDEVFS_CONTROL, &control);
    return result == 26 ? 0 : -1;
}

static int configure_uvc(int fd)
{
    static const uint8_t temperature_mode[26] = {
        0x01, 0x00,             /* bmHint: frame interval fixed */
        0x01, 0x02,             /* format 1, 640x1024 frame 2 */
        0x80, 0x1a, 0x06, 0x00, /* 400000 x 100 ns = 25 fps */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x20, 0x00,
        0x00, 0x00, 0x14, 0x00, /* 1,310,720-byte video frame */
        0x00, 0x06, 0x14, 0x00, /* 1,312,256-byte USB payload */
    };
    uint8_t defaults[sizeof(temperature_mode)];
    uint8_t negotiated[sizeof(temperature_mode)];

    if (uvc_control(fd, 0xa1, 0x83, 0x0100, defaults) < 0) return -1;
    if (defaults[2] != temperature_mode[2] ||
        defaults[3] != temperature_mode[3]) {
        ca_log("thermal USB default format=%u frame=%u; selecting temperature mode format=1 frame=2",
               defaults[2], defaults[3]);
    }
    memcpy(negotiated, temperature_mode, sizeof(negotiated));
    if (uvc_control(fd, 0x21, 0x01, 0x0100, negotiated) < 0 ||
        uvc_control(fd, 0xa1, 0x81, 0x0100, negotiated) < 0) {
        return -1;
    }
    if (negotiated[2] != 1U || negotiated[3] != 2U) {
        errno = EPROTO;
        return -1;
    }
    if (uvc_control(fd, 0x21, 0x01, 0x0200, negotiated) < 0) return -1;
    return 0;
}

static int submit_urb(struct ca_mt11_thermal *thermal,
                      struct thermal_urb *entry)
{
    memset(entry->urb, 0, sizeof(*entry->urb));
    entry->urb->type = USBDEVFS_URB_TYPE_BULK;
    entry->urb->endpoint = MT11_UVC_ENDPOINT;
    entry->urb->buffer = entry->buffer;
    entry->urb->buffer_length = MT11_UVC_URB_BYTES;
    entry->urb->usercontext = entry;
    if (ioctl(thermal->fd, USBDEVFS_SUBMITURB, entry->urb) < 0) return -1;
    entry->submitted = true;
    return 0;
}

static void *thermal_thread(void *opaque)
{
    struct ca_mt11_thermal *thermal = (struct ca_mt11_thermal*)(opaque);
    unsigned failures = 0;
    while (!atomic_load(&thermal->stop)) {
        struct usbdevfs_urb *completed = NULL;
        struct thermal_urb *entry;
        struct timespec received_at;
        if (ioctl(thermal->fd, USBDEVFS_REAPURB, &completed) < 0) {
            if (!atomic_load(&thermal->stop) && errno != EINTR) {
                // a disconnected core fails every call; don't spin or flood the log
                if (failures++ % 100U == 0U) {
                    ca_log("thermal USB reap failed (%u): %s", failures,
                           strerror(errno));
                }
                (void)usleep(100000);
            }
            continue;
        }
        failures = 0;
        if (clock_gettime(CLOCK_REALTIME, &received_at) < 0) {
            ca_log("thermal USB timestamp failed: %s", strerror(errno));
            received_at.tv_sec = time(NULL);
            received_at.tv_nsec = 0;
        }
        entry = (thermal_urb*)(completed != NULL ? completed->usercontext : NULL);
        if (entry == NULL) continue;
        entry->submitted = false;
        if (completed->status == 0 && completed->actual_length > 0) {
            if (ca_mt11_uvc_assembler_consume(
                    &thermal->assembler, entry->buffer,
                    (size_t)completed->actual_length, &received_at) < 0 &&
                errno != EIO) {
                ca_log("thermal UVC payload rejected: %s", strerror(errno));
            }
        } else if (!atomic_load(&thermal->stop) && completed->status != 0) {
            ca_log("thermal USB transfer failed: status=%d",
                   completed->status);
        }
        if (!atomic_load(&thermal->stop) && submit_urb(thermal, entry) < 0) {
            ca_log("thermal USB resubmit failed: %s", strerror(errno));
            atomic_store(&thermal->stop, true);
        }
    }
    return NULL;
}

int ca_mt11_thermal_open(struct ca_mt11_thermal **result,
                         ca_mt11_thermal_frame_cb callback, void *opaque)
{
    struct ca_mt11_thermal *thermal;
    char path[64];

    if (result == NULL || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    thermal = new (std::nothrow) ca_mt11_thermal{};
    if (thermal == NULL) return -1;
    thermal->fd = -1;
    int mutex_result = pthread_mutex_init(&thermal->control_lock, NULL);
    if (mutex_result != 0) {
        errno = mutex_result;
        goto fail;
    }
    thermal->control_lock_initialized = true;
    if (ca_mt11_uvc_assembler_init(&thermal->assembler, callback, opaque) < 0 ||
        find_usb_device(path, sizeof(path)) < 0) goto fail;
    thermal->fd = open(path, O_RDWR | O_CLOEXEC);
    if (thermal->fd < 0 ||
        claim_uvc_interface(thermal, MT11_UVC_CONTROL_INTERFACE) < 0 ||
        claim_uvc_interface(thermal, MT11_UVC_INTERFACE) < 0 ||
        configure_uvc(thermal->fd) < 0) goto fail;
    for (unsigned i = 0; i < MT11_UVC_URB_COUNT; i++) {
        thermal->urbs[i].urb = (struct usbdevfs_urb *)calloc(1, sizeof(struct usbdevfs_urb));
        if (!thermal->urbs[i].urb) goto fail;
        thermal->urbs[i].buffer = (uint8_t*)(malloc(MT11_UVC_URB_BYTES));
        if (thermal->urbs[i].buffer == NULL ||
            submit_urb(thermal, &thermal->urbs[i]) < 0) goto fail;
    }
    if (pthread_create(&thermal->thread, NULL, thermal_thread, thermal) != 0) {
        goto fail;
    }
    thermal->thread_started = true;
    ca_log("thermal USB ready: %s 640x512 YUYV + 640x512 radiometric Y16 at 25 fps",
           path);
    *result = thermal;
    return 0;
fail:
    ca_log("thermal USB startup failed: %s", strerror(errno));
    ca_mt11_thermal_close(thermal);
    return -1;
}

int ca_mt11_thermal_get_gain(struct ca_mt11_thermal *thermal, uint8_t *gain)
{
    uint8_t command[16];
    uint8_t result[2] = {};
    int status = -1;

    if (thermal == NULL || gain == NULL || thermal->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (lock_controls(thermal) < 0) return -1;
    ca_mt11_thermal_gain_get_command(command);
    if (stage_xu_command(thermal->fd, command) == 0 &&
        wait_for_xu_command(thermal->fd) == 0 &&
        xu_control(thermal->fd, 0xc1U, 0x44U, MT11_XU_RESULT,
                   result, sizeof(result)) == 0) {
        unsigned value = ((unsigned)result[0] << 8U) | result[1];
        if (value <= 1U) {
            *gain = (uint8_t)value;
            status = 0;
        } else {
            errno = EPROTO;
        }
    }
    return unlock_controls(thermal, status);
}

int ca_mt11_thermal_set_gain(struct ca_mt11_thermal *thermal, uint8_t gain)
{
    uint8_t command[16];
    int status;

    if (thermal == NULL || thermal->fd < 0 || gain > 1U) {
        errno = EINVAL;
        return -1;
    }
    if (lock_controls(thermal) < 0) return -1;
    ca_mt11_thermal_gain_set_command(command, gain);
    /* Long writes do not publish the 0/1 completion status used by reads.
     * The SIYI facade deliberately leaves confirmation to its following
     * gain query, matching the stock app and MAVProxy. */
    status = stage_xu_command(thermal->fd, command);
    return unlock_controls(thermal, status);
}

int ca_mt11_thermal_get_palette(struct ca_mt11_thermal *thermal,
                               uint8_t *palette)
{
    uint8_t command[16];
    uint8_t result = 0;
    int status = -1;

    if (thermal == NULL || palette == NULL || thermal->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (lock_controls(thermal) < 0) return -1;
    ca_mt11_thermal_palette_get_command(command);
    if (xu_control(thermal->fd, 0x41U, 0x45U, MT11_XU_SHORT_COMMAND,
                   command, 8U) == 0 &&
        xu_control(thermal->fd, 0xc1U, 0x44U, MT11_XU_SHORT_RESULT,
                   &result, sizeof(result)) == 0) {
        if (result >= 1U && result <= 12U && result != 2U) {
            *palette = (uint8_t)(result - 1U);
            status = 0;
        } else {
            errno = EPROTO;
        }
    }
    return unlock_controls(thermal, status);
}

int ca_mt11_thermal_set_palette(struct ca_mt11_thermal *thermal,
                               uint8_t palette)
{
    uint8_t command[16];
    int status;

    if (thermal == NULL || thermal->fd < 0 || palette > 11U || palette == 1U) {
        errno = EINVAL;
        return -1;
    }
    if (lock_controls(thermal) < 0) return -1;
    ca_mt11_thermal_palette_set_command(command, palette);
    status = xu_control(thermal->fd, 0x41U, 0x45U, MT11_XU_COMMAND_FIRST,
                        command, 8U);
    if (status == 0) {
        status = xu_control(thermal->fd, 0x41U, 0x45U,
                            MT11_XU_COMMAND_SECOND, command + 8U, 1U);
    }
    return unlock_controls(thermal, status);
}

void ca_mt11_thermal_close(struct ca_mt11_thermal *thermal)
{
    if (thermal == NULL) return;
    atomic_store(&thermal->stop, true);
    if (thermal->fd >= 0) {
        for (unsigned i = 0; i < MT11_UVC_URB_COUNT; i++) {
            if (thermal->urbs[i].submitted) {
                (void)ioctl(thermal->fd, USBDEVFS_DISCARDURB,
                            thermal->urbs[i].urb);
            }
        }
    }
    if (thermal->thread_started) pthread_join(thermal->thread, NULL);
    if (thermal->fd >= 0) {
        for (int interface = MT11_UVC_INTERFACE;
             interface >= MT11_UVC_CONTROL_INTERFACE; interface--) {
            if (thermal->interface_claimed[interface]) {
                (void)ioctl(thermal->fd, USBDEVFS_RELEASEINTERFACE,
                            &interface);
                thermal->interface_claimed[interface] = false;
            }
        }
        for (unsigned interface = MT11_UVC_CONTROL_INTERFACE;
             interface <= MT11_UVC_INTERFACE; interface++) {
            reconnect_uvc_interface(thermal, interface);
        }
        close(thermal->fd);
    }
    for (unsigned i = 0; i < MT11_UVC_URB_COUNT; i++) {
        free(thermal->urbs[i].buffer);
        free(thermal->urbs[i].urb);
    }
    ca_mt11_uvc_assembler_destroy(&thermal->assembler);
    if (thermal->control_lock_initialized) {
        (void)pthread_mutex_destroy(&thermal->control_lock);
    }
    delete thermal;
}
