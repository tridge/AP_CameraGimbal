#define _GNU_SOURCE
#include "camera_app/config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_config(int fd, const char *text)
{
    size_t length = strlen(text);
    assert(write(fd, text, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

int main(int argc, char **argv)
{
    char path[] = "/tmp/camera-app-config-XXXXXX";
    char error[160] = "";
    struct ca_config config;
    int fd;

    if (argc > 1) {
        ca_config_defaults(&config);
        assert(ca_config_load(&config, argv[1], error, sizeof(error)) == 0);
        assert(!config.log_disarmed);
    }
    ca_config_defaults(&config);
    assert(!config.log_disarmed);
    assert(strcmp(config.timezone, "GMT-10") == 0);
    assert(config.photo_scope == CA_PHOTO_SCOPE_ALL);
    assert(config.orientation == CA_MOUNT_AUTO);
    assert(config.uart_protocol == CA_UART_NONE);
    assert(config.main_resolution == CA_VIDEO_1080P);
    assert(config.sub_resolution == CA_VIDEO_720P);
    assert(config.recording_resolution == CA_VIDEO_1080P);
    assert(config.main_codec == CA_VIDEO_H264);
    assert(config.brightness == 50);
    assert(config.position_targeting);
    assert(config.support.video1_port == 0U);
    assert(config.support.video2_port == 0U);
    assert(config.autorecord == CA_AUTORECORD_DISABLED);
    assert(config.mavlink_system_id == 0U);
    assert(config.mavlink_camera_component_id == 100U);
    assert(config.mavlink_tcp_port == 14550U);
    assert(config.mavlink_udp_port == 14550U);

    fd = mkstemp(path);
    assert(fd >= 0);
    write_config(fd,
                 "# replacement config\n"
                 "[general]\n"
                 "timezone = Australia/Brisbane\n"
                 "unknown_future_option = 1\n"
                 "[capture]\n"
                 "photo_scope = all\n"
                 "[mount]\norientation = inverted\n"
                 "[uart]\nprotocol = mavlink\n"
                 "[mavlink]\nposition_targeting = false\n"
                 "system_id = 42\ntcp_port = 14600\nudp_port = 14601\n"
                 "[thermal]\npalette = ironbow\n"
                 "[recording]\nautorecord = true\nresolution = 3840x2160\n"
                 "[stream.main]\nresolution = 3840x2160\ncodec = h265\n"
                 "[stream.sub]\nresolution = 1920x1080\ncodec = h264\n"
                 "[image]\nbrightness = 61\nsaturation = 62\n"
                 "contrast = 63\nexposure_compensation = -5\n"
                 "iso = 800\nshutter = 1/250\nmetering = spot\n"
                 "white_balance = cloudy\n");
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(strcmp(config.timezone, "Australia/Brisbane") == 0);
    assert(config.photo_scope == CA_PHOTO_SCOPE_ALL);
    assert(strcmp(ca_photo_scope_name(config.photo_scope), "all") == 0);
    assert(config.orientation == CA_MOUNT_INVERTED);
    assert(config.uart_protocol == CA_UART_MAVLINK);
    assert(!config.position_targeting);
    assert(config.mavlink_system_id == 42U);
    assert(config.mavlink_tcp_port == 14600U);
    assert(config.mavlink_udp_port == 14601U);
    assert(config.thermal_palette == CA_PALETTE_IRONBOW);
    assert(config.autorecord == CA_AUTORECORD_ENABLED);
    assert(config.main_resolution == CA_VIDEO_2160P);
    assert(config.sub_resolution == CA_VIDEO_1080P);
    assert(config.recording_resolution == CA_VIDEO_2160P);
    assert(config.main_codec == CA_VIDEO_H265);
    assert(config.sub_codec == CA_VIDEO_H264);
    assert(config.brightness == 61 && config.saturation == 62 &&
           config.contrast == 63 && config.exposure_compensation == -5);
    assert(config.iso == CA_ISO_800);
    assert(config.shutter == CA_SHUTTER_1_250);
    assert(config.metering == CA_METERING_SPOT);
    assert(config.white_balance == CA_WB_CLOUDY);
    assert(strcmp(ca_mount_orientation_name(config.orientation), "inverted") == 0);
    assert(strcmp(ca_video_resolution_name(config.main_resolution),
                  "3840x2160") == 0);
    assert(strcmp(ca_video_codec_name(config.main_codec), "h265") == 0);
    assert(strcmp(ca_thermal_palette_name(config.thermal_palette),
                  "ironbow") == 0);

    assert(ca_config_param_count() == 34U);
    for (size_t i = 0; i < ca_config_param_count(); i++) {
        const char *name = ca_config_param_name(i);
        assert(strlen(name) > 0U && strlen(name) <= 16U);
        assert(ca_config_param_find(name) == (int)i);
        for (const char *c = name; *c != '\0'; c++) {
            assert(isupper((unsigned char)*c) || *c == '_' || isdigit((unsigned char)*c));
        }
        /* Every numeric kind must round-trip through its INI representation. */
        int value = ca_config_param_get(&config, i);
        assert(ca_config_param_save(&config, path, i, (float)value) == 0);
        assert(ca_config_param_get(&config, i) == value);
    }
    assert(ca_config_param_find("TIMEZONE") == -1);
    assert(!config.osd_cross && !config.osd_thermal_fov && !config.osd_recording);
    assert(ca_config_param_save(&config,path,(size_t)ca_config_param_find("OSD_RECORD"),1)==0);
    assert(config.osd_recording);
    assert(ca_config_param_save(&config,path,(size_t)ca_config_param_find("OSD_CROSS"),1)==0);
    assert(config.osd_cross);
    assert(ca_config_param_save(&config,path,(size_t)ca_config_param_find("OSD_THERMAL_FOV"),1)==0);
    assert(config.osd_thermal_fov);
    size_t component = (size_t)ca_config_param_find("MAV_CAM_COMP_ID");
    for (unsigned value = 100; value <= 105; value++) {
        assert(ca_config_param_save(&config, path, component, (float)value) == 0);
        assert(config.mavlink_camera_component_id == value);
    }
    assert(ca_config_param_save(&config, path, component, 99) < 0);
    assert(ca_config_param_save(&config, path, component, 106) < 0);
    assert(ca_config_param_save(&config, path, component, 100.5f) < 0);
    assert(config.mavlink_camera_component_id == 105U);
    int id = ca_config_param_find("MAV_SYSID");
    assert(ca_config_param_save(&config, path, (size_t)id, 255) == 0);
    assert(config.mavlink_system_id == 255U);
    assert(ca_config_param_save(&config, path, (size_t)id, 256) < 0);
    assert(ca_config_param_save(&config, path, (size_t)id, -1) < 0);
    assert(ca_config_param_save(&config, path, (size_t)id, 1.5f) < 0);
    assert(ca_config_param_save(&config, path, (size_t)id, NAN) < 0);
    assert(ca_config_param_save(&config, path, (size_t)id, INFINITY) < 0);
    assert(config.mavlink_system_id == 255U);
    assert(ca_config_param_save(&config, path, (size_t)id, 0) == 0);
    assert(config.mavlink_system_id == 0U);
    assert(ca_config_param_save(&config, path,
        (size_t)ca_config_param_find("THERMAL_PALETTE"), 1) < 0);
    size_t autorecord = (size_t)ca_config_param_find("REC_AUTOSTART");
    assert(ca_config_param_save(&config, path, autorecord, 2) == 0);
    assert(config.autorecord == CA_AUTORECORD_WHILE_ARMED);
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.autorecord == CA_AUTORECORD_WHILE_ARMED);
    assert(ca_config_param_get(&config, autorecord) == 2);
    assert(ca_config_param_save(&config, path, autorecord, 3) < 0);
    assert(ca_config_param_save(&config, path, autorecord, -1) < 0);
    assert(ca_config_param_save(&config, path, autorecord, 1.5f) < 0);
    FILE *saved = fopen(path, "r");
    assert(saved != NULL);
    char contents[4096] = {0};
    assert(fread(contents, 1U, sizeof(contents) - 1U, saved) > 0U);
    fclose(saved);
    assert(strstr(contents, "# replacement config\n") != NULL);
    assert(strstr(contents, "autorecord = while_armed\n") != NULL);
    assert(strstr(contents, "unknown_future_option = 1\n") != NULL);
    assert(strstr(contents, "timezone = Australia/Brisbane\n") != NULL);

    /* Existing installations can omit new keys or repeat a section with
     * different keys. Neither case should lose settings or create duplicates. */
    fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    write_config(fd, "[mavlink]\ntcp_port = 12345\n"
                     "[general]\ntimezone = UTC\n"
                     "[mavlink]\nsystem_id = 42");
    assert(ca_config_param_save(&config, path, (size_t)id, 77) == 0);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.mavlink_system_id == 77U && config.mavlink_tcp_port == 12345U);
    assert(ca_config_param_save(&config, path,
        (size_t)ca_config_param_find("MAV_UDP_PORT"), 54321) == 0);
    assert(ca_config_param_save(&config, path,
        (size_t)ca_config_param_find("IMG_EXPOSURE"), -6) == 0);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.mavlink_udp_port == 54321U && config.exposure_compensation == -6);
    assert(config.mavlink_system_id == 77U && config.mavlink_tcp_port == 12345U);
    assert(strcmp(config.timezone, "UTC") == 0);
    struct ca_config before_failure = config;
    assert(ca_config_param_save(&config, "/dev/null/missing.ini", (size_t)id, 3) < 0);
    assert(memcmp(&config, &before_failure, sizeof(config)) == 0);
    assert(unlink(path) == 0);
    assert(ca_config_param_save(&config, path, (size_t)id, 23) == 0);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.mavlink_system_id == 23U);

    fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    write_config(fd, "[capture]\nphoto_scope = visible\n");
    ca_config_defaults(&config);
    errno = 0;
    assert(ca_config_load(&config, path, error, sizeof(error)) < 0);
    assert(errno == EINVAL);
    assert(strstr(error, "photo_scope") != NULL);
    assert(unlink(path) == 0);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    write_config(fd, "[support_proxy]\nenabled = true\nhost = localhost\n"
                     "signing = true\nsigning_passphrase = \" a phrase & ? \"\n"
                     "video1_name = Front Camera\nnetwork_address = 192.0.2.25/24\n"
                     "network_gateway = 192.0.2.1\n");
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(config.support.enabled && config.support.signing);
    assert(config.support.video1_port == 0U && config.support.video2_port == 0U);
    assert(strcmp(config.support.signing_passphrase, " a phrase & ? ") == 0);
    assert(strcmp(config.support.video1_name, "Front Camera") == 0);
    const char *invalid_support[] = {
        "enabled = true\n", "host = bad/host\n", "network_address = 192.0.2.25\n",
        "network_address = 192.0.2.25/33\n", "network_gateway = bad\n",
        "enabled = true\nhost = localhost\nsigning = true\n",
        "enabled = true\nhost = localhost\nvideo1_port = 40001\nvideo2_port = 40001\n",
        "network_interface = eth0;command\n", "mavlink_port = 65536\n",
    };
    for (size_t i = 0; i < sizeof(invalid_support) / sizeof(invalid_support[0]); i++) {
        fd = open(path, O_WRONLY | O_TRUNC);
        assert(fd >= 0);
        assert(write(fd, "[support_proxy]\n", 16) == 16);
        write_config(fd, invalid_support[i]);
        ca_config_defaults(&config);
        assert(ca_config_load(&config, path, error, sizeof(error)) < 0);
    }
    const char *invalid_network[] = {
        "primary_address = 192.0.2.25\n", "primary_address = 192.0.2.25/33\n",
        "primary_address = 192.0.2.0/24\n", "primary_address = 192.0.2.255/24\n",
        "primary_address = 127.0.0.1/8\n", "secondary_address = 224.0.0.1/24\n",
        "primary_address = 192.0.2.25/24\nsecondary_address = 192.0.2.25/25\n",
        "primary_address = 192.0.2.25/24\ngateway = 198.51.100.1\n",
        "primary_address = 192.0.2.25/24\ngateway = 192.0.2.25\n",
        "interface = eth0;command\n", "interface = \"\"\n",
    };
    for (size_t i = 0; i < sizeof(invalid_network) / sizeof(invalid_network[0]); i++) {
        fd = open(path, O_WRONLY | O_TRUNC);
        assert(fd >= 0);
        assert(write(fd, "[network]\n", 10) == 10);
        write_config(fd, invalid_network[i]);
        ca_config_defaults(&config);
        assert(ca_config_load(&config, path, error, sizeof(error)) < 0);
    }
    fd = open(path, O_WRONLY | O_TRUNC);
    write_config(fd, "[network]\nprimary_address=198.51.100.27/24\n"
                     "secondary_address=192.0.2.25/24\ngateway=192.0.2.1\n");
    ca_config_defaults(&config);
    assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
    assert(!config.support.enabled);
    assert(strcmp(config.network.primary_address, "198.51.100.27/24") == 0);
    assert(strcmp(config.network.secondary_address, "192.0.2.25/24") == 0);
    assert(strcmp(config.network.gateway, "192.0.2.1") == 0);
    /* Legacy proxy networking migrates only when enabled, and an explicitly
     * empty new setting overrides a legacy value. */
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        for (unsigned override = 0; override < 2; override++) {
            fd = open(path, O_WRONLY | O_TRUNC);
            assert(dprintf(fd, "[support_proxy]\nenabled=%s\nhost=localhost\n"
                           "network_interface=eth1\nnetwork_address=192.0.2.25/24\nnetwork_gateway=192.0.2.1\n",
                           enabled ? "true" : "false") > 0);
            write_config(fd, override ? "[network]\ninterface=eth0\nsecondary_address=\"\"\ngateway=\"\"\n" : "");
            ca_config_defaults(&config);
            assert(ca_config_load(&config, path, error, sizeof(error)) == 0);
            assert(strcmp(config.network.interface, enabled && !override ? "eth1" : "eth0") == 0);
            assert(strcmp(config.network.secondary_address, enabled && !override ? "192.0.2.25/24" : "") == 0);
            assert(strcmp(config.network.gateway, enabled && !override ? "192.0.2.1" : "") == 0);
        }
    }
    assert(unlink(path) == 0);
    puts("camera configuration tests passed");
    return 0;
}
