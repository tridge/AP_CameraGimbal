#pragma once
#include "APC_Config.h"

// Preserve saved legacy settings while rejecting writes to controls the
// native pipeline cannot apply.
class APC_ZT30_Config {
public:
    static bool editable(const APC_Config::Field &field)
    {
        const char *s = field.section, *k = field.key;
        if (!strcmp(s, "video")) return true;
        if (!strcmp(s, "general") || !strcmp(s, "logging") || !strcmp(s, "capture") ||
            !strcmp(s, "image") || !strcmp(s,"survey")) return true;
        if(!strcmp(s,"overlay"))return true;
        if (!strcmp(s,"stream.main") || !strcmp(s,"recording")) return true;
        if (!strcmp(s,"stream.sub")) return true;
        if (!strcmp(s,"mavlink") || !strcmp(s,"network") || !strcmp(s,"support_proxy")) return true;
        if (!strcmp(s,"thermal")) return !strcmp(k,"stream_fps") || !strcmp(k,"record_fps") || !strcmp(k,"palette") || !strcmp(k,"hfov_cd");
        return false;
    }

    static bool validate_change(const ca_config &before, const ca_config &after,
                                char *error, size_t error_size)
    {
        if(after.thermal_hfov_cd && after.thermal_hfov_cd<100) {
            snprintf(error,error_size,"Thermal FOV must be 0 (geometry disabled) or at least 100 (1 degree)");
            errno=EINVAL;return false;
        }
        if(after.osd_thermal_fov && after.thermal_hfov_cd<100) {
            snprintf(error,error_size,"Set a thermal horizontal FOV before enabling its overlay");
            errno=EINVAL;return false;
        }
        ca_config supported;
        ca_config_defaults(&supported);
        effective(supported);
        for (size_t i = 0; i < APC_Config::count(); i++) {
            const auto &field = APC_Config::fields()[i];
            if (editable(field) || !memcmp((const char *)&before + field.offset,
                                          (const char *)&after + field.offset, field.size) ||
                !memcmp((const char *)&supported + field.offset, (const char *)&after + field.offset, field.size)) continue;
            snprintf(error, error_size, "[%s] %s is not configurable on the native ZT30 build",
                     field.section, field.key);
            errno = ENOTSUP;
            return false;
        }
        return true;
    }

    // Runtime normalization is deliberately separate from parsing/validation:
    // the web editor must compare the original saved values, not clamped ones.
    static bool effective(ca_config &config)
    {
        bool changed = config.orientation != CA_MOUNT_AUTO;
        config.orientation = CA_MOUNT_AUTO;
        return changed;
    }
};
