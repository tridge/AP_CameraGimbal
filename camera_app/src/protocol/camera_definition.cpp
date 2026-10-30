#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/camera_definition.h"
#include "apcam/lens.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#if APCAM_HAVE_FOCUS
static const struct ca_config_option autofocus[] = {{"Idle", 0}, {"Autofocus", 1}};
#endif
#if APCAM_HAVE_ZOOM_LENS
static const struct ca_config_option lenses[] = {{"Wide RGB", 0}, {"Zoom RGB", 1}};
#endif
#if APCAM_HAVE_THERMAL_VIDEO
static const struct ca_config_option sources[] = {{"RGB", 0}, {"Thermal", 1}};
#endif
#if APCAM_HAVE_THERMAL
/* Values follow the thermal module protocol. */
static const struct ca_config_option gains[] = {{"Low gain", 0}, {"High gain", 1}};
#endif
#if APCAM_HAVE_PHOTO
static const ca_config_option modes[] = {{"Photo",0}, {"Video",1},
#if APCAM_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV)
    {"Survey",2},
#endif
};
#endif
#define OPTIONS(name_, description_, op_, initial_, options_) \
    {name_, description_, op_, 6, initial_, 0, 0, options_, ARRAY_SIZE(options_), -1}
#define CONFIG(name_, description_) \
    {name_, description_, CA_CAMERA_CONFIG, 6, 0, 0, 0, NULL, 0, -1}
static const struct ca_camera_parameter parameters[] = {
    /* A GCS lists parameters in this order: live controls first, then thermal,
     * streams, recording, photo and image, overlays, tracking, survey. */
#if APCAM_HAVE_PHOTO
    OPTIONS("CAM_MODE", "Camera mode", CA_CAMERA_MODE, 0, modes),
#endif
#if APCAM_HAVE_ZOOM && !APCAM_ZOOM_NATIVE_RATE
    /* Native rate-only zoom has no trustworthy absolute position readback. */
    {"CAM_ZOOM", APCAM_HAVE_ZOOM_LENS ? "Wide digital zoom (x)" : "Zoom (x)",
     CA_CAMERA_ZOOM, 9, 1, 1, APCAM_TARGET==APCAM_TARGET_ZT30?1:APCAM_ZOOM_CONTROL_MAX, NULL, 0, -1},
#if APCAM_HAVE_ZOOM_LENS
    {"CAM_OPT_ZOOM", APCAM_TARGET==APCAM_TARGET_ZT30?"Zoom camera optical and digital zoom (x)":"Optical zoom (x)", CA_CAMERA_OPTICAL_ZOOM, 9,
     1, 1, APCAM_TARGET==APCAM_TARGET_ZT30?APCAM_ZOOM_CONTROL_MAX:APCAM_ZOOM_LENS_OPTICAL_MAX, NULL, 0, -1},
#endif
#endif
#if APCAM_HAVE_FOCUS
    OPTIONS("CAM_AUTOFOCUS", "Autofocus selected RGB lens", CA_CAMERA_AUTOFOCUS, 0, autofocus),
#endif
#if APCAM_HAVE_ZOOM_LENS
    OPTIONS("CAM_LENS", "RGB lens", CA_CAMERA_LENS, 0, lenses),
#endif
#if APCAM_HAVE_THERMAL_VIDEO
    OPTIONS("CAM_SOURCE", "Main video source", CA_CAMERA_SOURCE, 0, sources),
#endif
#if APCAM_HAVE_THERMAL
    OPTIONS("CAM_THERM_GAIN", "Thermal gain", CA_CAMERA_GAIN, 1, gains),
#endif
#if APCAM_HAVE_THERMAL_PALETTE
    {"CAM_PALETTE", "Thermal palette", CA_CAMERA_PALETTE, 6, 0, 0, 0, NULL, 0, -1},
#endif
#ifdef CAMERA_APP_ZT30_DEV
    CONFIG("THERMAL_HFOV_CD", "Thermal horizontal FOV (0.01 deg, default assumes MT11)"),
#endif
#if APCAM_HAVE_RAW_THERMAL
    CONFIG("RAW_STREAM_FPS", "Raw thermal streaming rate (fps)"),
#endif
#if APCAM_HAVE_THERMAL || APCAM_HAVE_RAW_THERMAL
    CONFIG("RAW_RECORD_FPS", "Raw thermal recording rate (fps, 0 disables)"),
#endif
    CONFIG("VIDEO_MAIN_RES", "Main RGB stream resolution"),
    CONFIG("VIDEO_MAIN_CODEC", "Main RGB stream codec"),
    CONFIG("VIDEO_META_FMT", "Video telemetry format (restart required)"),
#if APCAM_HAVE_VIDEO_BITRATE
    CONFIG("VIDEO_MAIN_KBPS", "Main RTSP bitrate (kbps)"),
#endif
#if APCAM_NUM_STREAMS >= 2
    CONFIG("VIDEO_SUB_RES", "Secondary RGB stream resolution"),
    CONFIG("VIDEO_SUB_CODEC", "Secondary RGB stream codec"),
#endif
#if APCAM_HAVE_RECORDING_RESOLUTION
    CONFIG("REC_RESOLUTION", "Recording resolution"),
#endif
    CONFIG("REC_AUTOSTART", "Automatic recording"),
#if APCAM_HAVE_THERMAL_VIDEO
    CONFIG("PHOTO_SCOPE", "Photo capture lenses"),
#endif
#if APCAM_HAVE_PHOTO_RESOLUTION
    CONFIG("PHOTO_RES", "Photo resolution (larger sizes briefly pause video)"),
#endif
#if APCAM_HAVE_IMAGE_CONTROLS
    CONFIG("IMG_BRIGHTNESS", "Brightness"),
    CONFIG("IMG_SATURATION", "Saturation"),
    CONFIG("IMG_CONTRAST", "Contrast"),
    CONFIG("IMG_EXPOSURE", "Exposure compensation"),
    CONFIG("IMG_ISO", "ISO"),
    CONFIG("IMG_SHUTTER", "Shutter speed"),
    CONFIG("IMG_METERING", "Exposure metering"),
    CONFIG("IMG_WHITE_BAL", "White balance"),
#endif
    CONFIG("OSD_CROSS", "Targeting cross"),
#if APCAM_HAVE_OVERLAY_RECORDING_SELECT
    CONFIG("OSD_RECORD", "Overlays in recordings"),
#endif
#if APCAM_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV)
    CONFIG("OSD_THERMAL_FOV", "Thermal field of view in RGB (nominal alignment)"),
#endif
    CONFIG("MAV_POS_TARGET", "Position targeting"),
    CONFIG("TRACK_METHOD", "Location tracking control method"),
    CONFIG("LOG_DISARMED", "Log when disarmed"),
#if APCAM_HAVE_PHOTO && (APCAM_HAVE_THERMAL || defined(CAMERA_APP_ZT30_DEV))
    CONFIG("SURV_LENS", "Survey camera"),
    CONFIG("SURV_PATTERN", "Survey pattern"),
    CONFIG("SURV_OVERLAP", "Overlap (%)"),
    CONFIG("SURV_FORE", "Forward distance (% AGL)"),
    CONFIG("SURV_AFT", "Rear distance (% AGL)"),
    CONFIG("SURV_BURST", "Images per position"),
    CONFIG("SURV_BURST_MS", "Burst interval (ms)"),
    CONFIG("SURV_DWELL_MS", "Capture dwell limit (ms)"),
    CONFIG("SURV_SETTLE_MS", "Settling window (ms)"),
    CONFIG("SURV_ERROR_CD", "Pointing tolerance (0.01 deg)"),
    CONFIG("SURV_RATE_CD", "Residual rate tolerance (0.01 deg/s)"),
    CONFIG("SURV_TERR_MS", "Terrain maximum age (ms)"),
    CONFIG("SURV_TERR_M", "Terrain report radius (m)"),
    CONFIG("SURV_TERR_TOL", "Terrain consistency (m)"),
    CONFIG("SURV_MIN_SPEED", "Minimum ground speed (m/s)"),
    CONFIG("SURV_TURN_DEG", "Turn threshold (deg)"),
#endif
};

size_t ca_camera_param_count(void) { return ARRAY_SIZE(parameters); }

int ca_camera_param_find(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(parameters); i++) {
        if (strcmp(name, parameters[i].name) == 0) return (int)i;
    }
    return -1;
}

bool ca_camera_param_info(size_t index, struct ca_camera_parameter *parameter)
{
    if (index >= ARRAY_SIZE(parameters)) return false;
    *parameter = parameters[index];
    if (parameter->operation == CA_CAMERA_CONFIG || parameter->operation == CA_CAMERA_PALETTE) {
        const char *name = parameter->operation == CA_CAMERA_PALETTE ? "THERMAL_PALETTE" : parameter->name;
        parameter->config_index = ca_config_param_find(name);
        if (parameter->config_index < 0) return false;
        if (ca_config_param_is_bool((size_t)parameter->config_index)) parameter->type = 1;
        int minimum, maximum;
        parameter->option_count = ca_config_param_options((size_t)parameter->config_index,
            &parameter->options, &minimum, &maximum);
        parameter->minimum = (float)minimum;
        parameter->maximum = (float)maximum;
        struct ca_config defaults;
        ca_config_defaults(&defaults);
        parameter->initial = (float)ca_config_param_get(&defaults, (size_t)parameter->config_index);
    }
    return true;
}

bool ca_camera_param_valid(const struct ca_camera_parameter *parameter, float value)
{
    if (!isfinite(value)) return false;
    if (parameter->option_count) {
        for (size_t i = 0; i < parameter->option_count; i++) {
            if (value == (float)parameter->options[i].value) return true;
        }
        return false;
    }
    return value >= parameter->minimum && value <= parameter->maximum &&
           (parameter->type == 9 || truncf(value) == value);
}

static void xml_text(FILE *out, const char *text)
{
    for (; *text; text++) {
        switch (*text) {
        case '&': fputs("&amp;", out); break;
        case '<': fputs("&lt;", out); break;
        case '>': fputs("&gt;", out); break;
        case '"': fputs("&quot;", out); break;
        case '\'': fputs("&apos;", out); break;
        default: fputc(*text, out); break;
        }
    }
}

char *ca_camera_definition(size_t *length)
{
    char *xml = NULL;
    FILE *out = open_memstream(&xml, length);
    if (!out) return NULL;
    fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<mavlinkcamera>\n"
                 "  <definition version=\"%u\"><model>", CA_CAMERA_DEFINITION_VERSION);
    xml_text(out, APCAM_MODEL_NAME);
    fputs("</model><vendor>ArduPilot</vendor></definition>\n  <parameters>\n", out);
    for (size_t i = 0; i < ca_camera_param_count(); i++) {
        struct ca_camera_parameter p;
        if (!ca_camera_param_info(i, &p)) { fclose(out); free(xml); return NULL; }
        fprintf(out, "    <parameter name=\"%s\" type=\"%s\" default=\"%.9g\"",
                p.name, p.type == 9 ? "float" : p.type == 1 ? "bool" : "int32", (double)p.initial);
        if (!p.option_count) {
            fprintf(out, " min=\"%.9g\" max=\"%.9g\"", (double)p.minimum, (double)p.maximum);
            if (p.operation == CA_CAMERA_ZOOM || p.operation == CA_CAMERA_OPTICAL_ZOOM)
                fputs(" step=\"0.1\"", out);
            else if (p.type != 9) fputs(" step=\"1\"", out);
        }
        fputs(">\n      <description>", out);
        xml_text(out, p.description);
        fputs("</description>\n", out);
        if (p.option_count) {
            fputs("      <options>\n", out);
            for (size_t j = 0; j < p.option_count; j++) {
                fputs("        <option name=\"", out);
                xml_text(out, p.options[j].name);
                fprintf(out, "\" value=\"%d\"", p.options[j].value);
                if (p.operation == CA_CAMERA_LENS) {
                    fprintf(out, "><exclusions><exclude>%s</exclude></exclusions></option>\n",
                            p.options[j].value == 0 ? "CAM_OPT_ZOOM" : "CAM_ZOOM");
                } else fputs("/>\n", out);
            }
            fputs("      </options>\n", out);
        }
        if (p.operation == CA_CAMERA_AUTOFOCUS) {
            fputs("      <updates><update>CAM_AUTOFOCUS</update></updates>\n", out);
        } else if (p.operation == CA_CAMERA_LENS) {
            fputs("      <updates><update>CAM_ZOOM</update><update>CAM_OPT_ZOOM</update></updates>\n", out);
        } else if (p.operation == CA_CAMERA_SOURCE && APCAM_HAVE_ZOOM_LENS) {
            fputs("      <updates><update>CAM_LENS</update><update>CAM_ZOOM</update><update>CAM_OPT_ZOOM</update></updates>\n", out);
        }
        fputs("    </parameter>\n", out);
    }
    fputs("  </parameters>\n</mavlinkcamera>\n", out);
    bool failed = ferror(out) != 0;
    if (fclose(out) != 0 || failed) { free(xml); return NULL; }
    return xml;
}

uint16_t ca_camera_definition_version(const char *xml, size_t length)
{
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)(uint8_t)xml[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc != 0U ? crc : 0xffffU;
}
