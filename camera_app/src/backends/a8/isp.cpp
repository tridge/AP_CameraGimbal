/*
  SIYI A8 mini ISP image settings.

  The SSC8836 SDK headers are not public; the structure layouts below were
  taken from how the vendor app calls the same libmi_isp.so entry points.
  All calls take (device, channel, attr *) and every attribute is read
  back with the matching Get first, so only the fields we know are touched.
*/
#include "isp.h"

#include "camera_app/log.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ISP_DEV 0
#define ISP_CHN 0
#define ISP_ATTR_MAX 2048U

/* bEnable, enOpType, stAuto, stManual */
#define IQ_ENABLE 0U
#define IQ_OP_TYPE 4U
#define IQ_LEVEL_MANUAL 0x48U
#define IQ_SATURATION_MANUAL 0x188U
#define IQ_OP_MANUAL 1U

/* bEnable, enOpType, stManual { u16 R, Gr, Gb, B }, stAuto */
#define AWB_OP_TYPE 4U
#define AWB_GAINS 8U

/* u32 shutter us, u32 sensor gain, u32 ISP gain; 1024 = 1x */
#define EXPO_SHUTTER 0U
#define EXPO_SENSOR_GAIN 4U
#define EXPO_ISP_GAIN 8U
#define GAIN_UNITY 1024U

/* u32 min shutter us, u32 max shutter us, ... */
#define LIMIT_MIN_SHUTTER 0U
#define LIMIT_MAX_SHUTTER 4U

#define AE_MODE_AUTO 0U
#define AE_MODE_MANUAL 4U

typedef int (*isp_attr_fn)(uint32_t device, uint32_t channel, void *attr);

struct isp_api {
    void *handle;
    isp_attr_fn get_brightness, set_brightness;
    isp_attr_fn get_contrast, set_contrast;
    isp_attr_fn get_saturation, set_saturation;
    isp_attr_fn get_awb, set_awb;
    isp_attr_fn get_ev, set_ev;
    isp_attr_fn set_expo_mode;
    isp_attr_fn get_manual_expo, set_manual_expo;
    isp_attr_fn get_expo_limit, set_expo_limit;
    isp_attr_fn get_expo_table;
    isp_attr_fn set_win_wgt_type;
    bool have_default_limit;
    uint32_t default_limit[2];
};

static struct isp_api api;

static void put_u32(uint8_t *attr, unsigned offset, uint32_t value)
{
    memcpy(attr + offset, &value, sizeof(value));
}

static void put_u16(uint8_t *attr, unsigned offset, uint16_t value)
{
    memcpy(attr + offset, &value, sizeof(value));
}

static uint32_t get_u32(const uint8_t *attr, unsigned offset)
{
    uint32_t value;
    memcpy(&value, attr + offset, sizeof(value));
    return value;
}

static int load_api(void)
{
    static const struct {
        const char *name;
        size_t offset;
    } symbols[] = {
#define SYM(field, name) {name, offsetof(struct isp_api, field)}
        SYM(get_brightness, "MI_ISP_IQ_GetBrightness"),
        SYM(set_brightness, "MI_ISP_IQ_SetBrightness"),
        SYM(get_contrast, "MI_ISP_IQ_GetContrast"),
        SYM(set_contrast, "MI_ISP_IQ_SetContrast"),
        SYM(get_saturation, "MI_ISP_IQ_GetSaturation"),
        SYM(set_saturation, "MI_ISP_IQ_SetSaturation"),
        SYM(get_awb, "MI_ISP_AWB_GetAttr"),
        SYM(set_awb, "MI_ISP_AWB_SetAttr"),
        SYM(get_ev, "MI_ISP_AE_GetEVComp"),
        SYM(set_ev, "MI_ISP_AE_SetEVComp"),
        SYM(set_expo_mode, "MI_ISP_AE_SetExpoMode"),
        SYM(get_manual_expo, "MI_ISP_AE_GetManualExpo"),
        SYM(set_manual_expo, "MI_ISP_AE_SetManualExpo"),
        SYM(get_expo_limit, "MI_ISP_AE_GetExposureLimit"),
        SYM(set_expo_limit, "MI_ISP_AE_SetExposureLimit"),
        SYM(set_win_wgt_type, "MI_ISP_AE_SetWinWgtType"),
#undef SYM
    };

    if (api.handle != NULL) return 0;
    api.handle = dlopen("libmi_isp.so", RTLD_NOW | RTLD_GLOBAL);
    if (api.handle == NULL) {
        ca_log("libmi_isp.so: %s", dlerror());
        errno = ENOENT;
        return -1;
    }
    for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
        void *symbol = dlsym(api.handle, symbols[i].name);
        if (symbol == NULL) {
            ca_log("libmi_isp.so lacks %s", symbols[i].name);
            errno = ENOENT;
            return -1;
        }
        memcpy((uint8_t *)&api + symbols[i].offset, &symbol, sizeof(symbol));
    }
    /* Older SDKs can still use their inherited exposure limits. */
    void *table = dlsym(api.handle, "MI_ISP_AE_GetPlainLongExpoTable");
    memcpy(&api.get_expo_table, &table, sizeof(table));
    return 0;
}

static int set_level(const char *name, isp_attr_fn get, isp_attr_fn set,
                     unsigned offset, bool byte, unsigned value)
{
    uint8_t attr[ISP_ATTR_MAX] __attribute__((aligned(8)));
    int result = get(ISP_DEV, ISP_CHN, attr);

    if (result == 0) {
        put_u32(attr, IQ_ENABLE, 1U);
        put_u32(attr, IQ_OP_TYPE, IQ_OP_MANUAL);
        if (byte) attr[offset] = (uint8_t)value;
        else put_u32(attr, offset, value);
        result = set(ISP_DEV, ISP_CHN, attr);
    }
    if (result != 0) ca_log("ISP %s=%u failed: 0x%x", name, value, (unsigned)result);
    return result;
}

static int set_white_balance(enum ca_white_balance mode)
{
    /* R, Gr, Gb, B gains, 1024 = 1x; the presets the vendor app uses */
    static const uint16_t presets[][4] = {
        {0, 0, 0, 0},
        {2149, 1024, 1024, 1850},
        {2480, 1024, 1024, 1550},
        {2341, 1024, 1024, 1856},
        {1532, 1024, 1024, 2220},
    };
    uint8_t attr[ISP_ATTR_MAX] __attribute__((aligned(8)));
    int result = api.get_awb(ISP_DEV, ISP_CHN, attr);

    if (result == 0) {
        if (mode == CA_WB_AUTO) {
            put_u32(attr, AWB_OP_TYPE, 0U);
        } else {
            put_u32(attr, AWB_OP_TYPE, 1U);
            for (unsigned i = 0; i < 4U; i++) {
                put_u16(attr, AWB_GAINS + i * 2U, presets[mode][i]);
            }
        }
        result = api.set_awb(ISP_DEV, ISP_CHN, attr);
    }
    if (result != 0) ca_log("ISP white balance failed: 0x%x", (unsigned)result);
    return result;
}

static int set_ev(int tenths)
{
    uint8_t attr[ISP_ATTR_MAX] __attribute__((aligned(8)));
    int result = api.get_ev(ISP_DEV, ISP_CHN, attr);

    if (result == 0) {
        put_u32(attr, 0U, (uint32_t)(int32_t)tenths);
        result = api.set_ev(ISP_DEV, ISP_CHN, attr);
    }
    if (result != 0) ca_log("ISP EV compensation failed: 0x%x", (unsigned)result);
    return result;
}

/* The A8 ISP can report a 5653 us floor even though its exposure table starts
 * at 147 us. This stops AE compensating for bright scenes at minimum gain.
 * Follow the table without raising an already lower limit. Its ABI is a u32
 * count followed by up to 16 {fnumber, shutter_us, total_gain, sensor_gain}.
 */
static uint32_t auto_min_shutter(uint32_t inherited, uint32_t maximum)
{
    uint8_t table[ISP_ATTR_MAX] __attribute__((aligned(8))) = {};
    if (!api.get_expo_table || api.get_expo_table(ISP_DEV, ISP_CHN, table) != 0) {
        ca_log("ISP exposure table unavailable; retaining minimum shutter %u us", inherited);
        return inherited;
    }
    uint32_t count = get_u32(table, 0);
    uint32_t minimum = inherited;
    bool have_usable_row = false;
    if (count == 0 || count > 16U) goto invalid;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t us = get_u32(table, 8U + i * 16U);
        /* A long-exposure table may include rows beyond the current video
         * shutter limit. Ignore those and empty rows without losing shorter
         * usable exposures; do not impose an unrelated absolute time limit. */
        if (us == 0 || us > maximum) continue;
        have_usable_row = true;
        if (us < minimum) minimum = us;
    }
    if (!have_usable_row || minimum == 0 || minimum > maximum) goto invalid;
    return minimum;
invalid:
    ca_log("ISP exposure table invalid; retaining minimum shutter %u us", inherited);
    return inherited;
}

static int set_exposure(const struct ca_config *config)
{
    static const uint32_t exposure_us[] = {
        0U, 33333U, 20000U, 10000U, 4000U, 2000U, 1333U, 1000U, 500U,
    };
    uint8_t attr[ISP_ATTR_MAX] __attribute__((aligned(8)));
    uint32_t shutter = exposure_us[config->shutter];
    uint32_t mode;
    int result;

    /* shutter limits are only honoured by the auto modes */
    result = api.get_expo_limit(ISP_DEV, ISP_CHN, attr);
    if (result != 0) goto done;
    if (!api.have_default_limit) {
        api.have_default_limit = true;
        uint32_t inherited = get_u32(attr, LIMIT_MIN_SHUTTER);
        api.default_limit[1] = get_u32(attr, LIMIT_MAX_SHUTTER);
        api.default_limit[0] = auto_min_shutter(inherited, api.default_limit[1]);
        ca_log("ISP auto shutter range=%u..%u us (inherited minimum=%u us)",
               api.default_limit[0], api.default_limit[1], inherited);
    }
    put_u32(attr, LIMIT_MIN_SHUTTER,
            shutter != 0U ? shutter : api.default_limit[0]);
    put_u32(attr, LIMIT_MAX_SHUTTER,
            shutter != 0U ? shutter : api.default_limit[1]);
    result = api.set_expo_limit(ISP_DEV, ISP_CHN, attr);
    if (result != 0) goto done;

    mode = config->iso == CA_ISO_AUTO ? AE_MODE_AUTO : AE_MODE_MANUAL;
    result = api.set_expo_mode(ISP_DEV, ISP_CHN, &mode);
    if (result != 0 || mode == AE_MODE_AUTO) goto done;

    result = api.get_manual_expo(ISP_DEV, ISP_CHN, attr);
    if (result != 0) goto done;
    put_u32(attr, EXPO_SENSOR_GAIN, GAIN_UNITY << ((unsigned)config->iso - 1U));
    put_u32(attr, EXPO_ISP_GAIN, GAIN_UNITY);
    if (shutter != 0U) put_u32(attr, EXPO_SHUTTER, shutter);
    result = api.set_manual_expo(ISP_DEV, ISP_CHN, attr);
done:
    if (result != 0) ca_log("ISP exposure failed: 0x%x", (unsigned)result);
    return result;
}

static int set_metering(enum ca_metering_mode mode)
{
    uint32_t type = (uint32_t)mode;
    int result = api.set_win_wgt_type(ISP_DEV, ISP_CHN, &type);
    if (result != 0) ca_log("ISP metering failed: 0x%x", (unsigned)result);
    return result;
}

int ca_a8_apply_isp_config(const struct ca_config *config)
{
    if (config == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (load_api() < 0) return -1;
    if (set_level("brightness", api.get_brightness, api.set_brightness,
                  IQ_LEVEL_MANUAL, false, (unsigned)config->brightness) != 0 ||
        set_level("contrast", api.get_contrast, api.set_contrast,
                  IQ_LEVEL_MANUAL, false, (unsigned)config->contrast) != 0 ||
        set_level("saturation", api.get_saturation, api.set_saturation,
                  IQ_SATURATION_MANUAL, true, (unsigned)config->saturation) != 0 ||
        set_white_balance(config->white_balance) != 0 ||
        set_ev(config->exposure_compensation) != 0 ||
        set_exposure(config) != 0 ||
        set_metering(config->metering) != 0) {
        errno = EIO;
        return -1;
    }
    ca_log("ISP configured brightness=%d saturation=%d contrast=%d ev=%d "
           "iso=%d shutter=%d metering=%d white_balance=%d",
           config->brightness, config->saturation, config->contrast,
           config->exposure_compensation, config->iso, config->shutter,
           config->metering, config->white_balance);
    return 0;
}
