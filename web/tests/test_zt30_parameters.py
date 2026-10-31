#!/usr/bin/env python3
"""Native settings must preserve legacy INI values and reject unsupported writes."""
from pathlib import Path
import subprocess
import tempfile

WEB = Path(__file__).resolve().parents[1]
SOURCE = r'''
#define main camera_web_main
#include "mt11-web.cpp"
#undef main
#include <cassert>
#include <string>
#include "camera_app/camera_definition.h"

static const char *initial =
    "# Preserve this legacy format and unknown key\n"
    "[general]\ntimezone=Australia/Canberra\nfuture_key=kept\n"
    "[stream.main]\nresolution=3840x2160\ncodec=h265\n"
    "[capture]\nphoto_scope=all\n"
    "[thermal]\nstream_fps=5\n";

static std::string contents(const char *path)
{
    size_t n; char *p = read_file(path, MAX_CONFIG, &n);
    if (!p) return {};
    std::string result(p, n); free(p); return result;
}

static void denied(const std::string &candidate)
{
    char error[512] = {};
    auto before = contents(REPLACEMENT_CONFIG_PATH);
    auto backup = contents(REPLACEMENT_CONFIG_BACKUP_PATH);
    assert(!save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                        candidate.data(), candidate.size(), error, sizeof(error)));
    assert(error[0]);
    assert(contents(REPLACEMENT_CONFIG_PATH) == before);
    assert(contents(REPLACEMENT_CONFIG_BACKUP_PATH) == backup);
}

int main()
{
    char error[512] = {};
    FILE *f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f); fputs(initial, f); fclose(f);
    ca_config loaded; ca_config_defaults(&loaded);
    assert(ca_config_load(&loaded, REPLACEMENT_CONFIG_PATH, error, sizeof(error)) == 0);
    assert(loaded.main_resolution == CA_VIDEO_2160P && loaded.main_codec == CA_VIDEO_H265);
    assert(loaded.thermal_hfov_cd == 2420); // User-requested MT11 assumption.
    bool raw_survey = false;
    for (const auto &lens : survey_lenses) raw_survey |= !strcmp(lens.value, "thermal");
    assert(raw_survey); // Native raw thermal is available without generic thermal controls.

    // A web restart changes the camera's TMP without changing the web's TMP.
    const std::string original_tmp = std::string(APP_DIR) + "/original-tmp";
    assert(mkdir(original_tmp.c_str(), 0700) == 0);
    assert(mkdir(RUNTIME_DIR, 0700) == 0);
    assert(setenv("TMPDIR", original_tmp.c_str(), 1) == 0);
    char original_status[4096], restarted_status[4096];
    assert(apcam_survey_status_path(REPLACEMENT_CONFIG_PATH, original_status, sizeof(original_status)));
    assert(apcam_survey_status_path(REPLACEMENT_CONFIG_PATH, restarted_status, sizeof(restarted_status), RUNTIME_DIR));
    FILE *status = fopen(original_status, "w"); assert(status);
    fputs("{\"state\":\"idle\"}", status); fclose(status);
    const timespec stale[] = {{time(NULL)-10, 0}, {time(NULL)-10, 0}};
    assert(utimensat(AT_FDCWD, original_status, stale, 0) == 0);
    status = fopen(restarted_status, "w"); assert(status);
    fputs("{\"state\":\"surveying\",\"captured\":17}", status); fclose(status);
    size_t status_length = 0;
    char *body = read_survey_status(&status_length);
    assert(body && strstr(body, "\"captured\":17")); free(body);
    assert(utimensat(AT_FDCWD, restarted_status, stale, 0) == 0);
    assert(!read_survey_status(&status_length)); // Never revive a dead camera's snapshot.
    assert(unlink(original_status) == 0 && unlink(restarted_status) == 0);
    unsetenv("TMPDIR");
    loaded.orientation=CA_MOUNT_UPRIGHT;
    assert(APC_ZT30_Config::effective(loaded));
    assert(loaded.orientation==CA_MOUNT_AUTO && loaded.main_resolution == CA_VIDEO_2160P && loaded.main_codec == CA_VIDEO_H265);
    assert(contents(REPLACEMENT_CONFIG_PATH) == initial); // startup never rewrites INI

    for (const auto &p : replacement_parameters) {
        if (parameter_shown(&p)) assert(APC_Config::find(p.section, p.key));
        if (!strcmp(p.form_name, "orientation")) assert(!parameter_shown(&p));
        if (!strcmp(p.form_name, "timezone") || !strcmp(p.form_name, "photo_scope") ||
            !strcmp(p.form_name,"raw_stream_fps") || !strcmp(p.form_name,"raw_record_fps") ||
            !strcmp(p.section,"image") || !strcmp(p.section,"stream.main") || !strcmp(p.section,"stream.sub") ||
            !strcmp(p.form_name,"thermal_palette") || !strcmp(p.form_name,"thermal_hfov_cd") ||
            !strcmp(p.form_name,"osd_thermal_fov") ||
            !strcmp(p.section,"recording") || !strcmp(p.section,"survey") ||
            !strcmp(p.form_name,"position_targeting") || !strcmp(p.form_name,"tracking_method") ||
            !strcmp(p.section,"network") || !strcmp(p.form_name,"osd_cross") || !strcmp(p.form_name,"osd_recording")) { if(!parameter_shown(&p)) fprintf(stderr,"Unexpectedly hidden: %s [%s] %s\n",p.form_name,p.section,p.key); assert(parameter_shown(&p)); }
    }
    for (const auto &p : replacement_parameters) {
        if (strcmp(p.form_name,"orientation")) continue;
        APC_HTTPRequest request;
        char body[]="orientation=inverted";
        request.body = body; request.body_len = strlen(body);
        ini_update updates[2]; size_t count = 0;
        assert(!collect_described_parameters(&request, &p, 1, updates, 2, &count, error, sizeof(error)));
        assert(count == 0 && strstr(error, "not configurable"));
    }
    denied(std::string(initial) + "[mount]\norientation=inverted\n");
    std::string fov = initial;
    fov += "hfov_cd=99\n"; denied(fov);
    fov = initial; fov += "hfov_cd=0\n[overlay]\nthermal_fov=true\n"; denied(fov);
    fov = initial; fov += "hfov_cd=2420\n[overlay]\nthermal_fov=true\n";
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       fov.data(), fov.size(), error, sizeof(error)));
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       initial, strlen(initial), error, sizeof(error)));
    denied(std::string(initial) + "[thermal]\nstream_fps=6\n"); // duplicate
    denied(std::string(initial) + "[mavlink]\nsystem_id=invalid\n");
    denied(std::string(initial) + "[image]\nbrightness=invalid\n");
    auto nul = std::string(initial); nul.push_back(0); nul += "extra"; denied(nul);

    std::string supported = initial;
    supported.replace(supported.find("stream_fps=5"), 12, "stream_fps=6");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       supported.data(), supported.size(), error, sizeof(error)));
    assert(contents(REPLACEMENT_CONFIG_PATH) == supported);
    assert(contents(REPLACEMENT_CONFIG_BACKUP_PATH) == initial);
    assert(supported.find("future_key=kept") != std::string::npos);

    ca_config_defaults(&loaded);
    assert(ca_config_load(&loaded, REPLACEMENT_CONFIG_PATH, error, sizeof(error)) == 0);
    for (const char *name : {"MOUNT_ORIENT"}) {
        int index = ca_config_param_find(name); assert(index >= 0);
        auto previous = loaded;
        auto saved = contents(REPLACEMENT_CONFIG_PATH);
        assert(ca_config_param_assign(&loaded, index, 1) < 0 && errno == ENOTSUP);
        assert(!memcmp(&loaded, &previous, sizeof(loaded)));
        assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, index, 1) < 0 && errno == ENOTSUP);
        assert(contents(REPLACEMENT_CONFIG_PATH) == saved);
    }
    int scope = ca_config_param_find("PHOTO_SCOPE"); assert(scope >= 0);
    assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, scope, CA_PHOTO_SCOPE_THERMAL) == 0);
    assert(loaded.photo_scope == CA_PHOTO_SCOPE_THERMAL);
    int rate = ca_config_param_find("RAW_STREAM_FPS"); assert(rate >= 0);
    assert(ca_config_param_save(&loaded, REPLACEMENT_CONFIG_PATH, rate, 7) == 0);
    assert(loaded.raw_stream_fps == 7);
    for (const char *name : {"PHOTO_SCOPE","RAW_STREAM_FPS","RAW_RECORD_FPS","LOG_DISARMED",
                             "REC_AUTOSTART","REC_RESOLUTION","VIDEO_MAIN_RES","VIDEO_MAIN_CODEC",
                             "VIDEO_SUB_RES","VIDEO_SUB_CODEC","THERMAL_HFOV_CD","OSD_THERMAL_FOV","MAV_POS_TARGET","TRACK_METHOD","OSD_CROSS","OSD_RECORD","SURV_LENS","SURV_PATTERN","SURV_OVERLAP"}) {
        int index = ca_camera_param_find(name); assert(index >= 0);
        ca_camera_parameter parameter;
        assert(ca_camera_param_info(index, &parameter));
        assert(parameter.config_index >= 0);
    }
    // Editing legacy values back to the supported mode must be possible.
    for(const char *name:{"IMG_BRIGHTNESS","IMG_CONTRAST","IMG_SATURATION","IMG_EXPOSURE",
                          "IMG_ISO","IMG_SHUTTER","IMG_METERING","IMG_WHITE_BAL"}) {
        int index=ca_config_param_find(name);assert(index>=0);
        assert(ca_camera_param_find(name)>=0);
        assert(ca_config_param_assign(&loaded,index,1)==0);
    }
    const char *clean = "[stream.main]\nresolution=1280x720\ncodec=h264\n";
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    // Repair corrupt current configuration, including missing files.
    f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f);
    fputs("[stream.main]\ncodec=bogus\n", f); fclose(f);
    denied("[mount]\norientation=inverted\n");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    assert(unlink(REPLACEMENT_CONFIG_PATH) == 0);
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       clean, strlen(clean), error, sizeof(error)));
    // Legacy proxy settings retain their normal migration behavior.
    const char *legacy = "[support_proxy]\nenabled=false\nhost=localhost\nnetwork_address=192.168.9.2/24\n";
    f = fopen(REPLACEMENT_CONFIG_PATH, "w"); assert(f); fputs(legacy, f); fclose(f);
    std::string enabled = legacy; enabled.replace(enabled.find("false"), 5, "true");
    assert(save_config(REPLACEMENT_CONFIG_PATH, REPLACEMENT_CONFIG_BACKUP_PATH,
                       enabled.data(), enabled.size(), error, sizeof(error)));
    puts("PASS native ZT30 setting visibility, strict INI saves, startup normalization and numeric write policy");
}
'''
with tempfile.TemporaryDirectory(prefix='zt30-parameters-') as directory:
    root = Path(directory)
    source = root / 'test.cpp'; source.write_text(SOURCE)
    binary = root / 'test'
    subprocess.run(['c++', '-std=gnu++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-missing-field-initializers',
                    '-DMT11_WEB_TEST', '-DCAMERA_APP_ZT30_DEV', '-DAPCAM_TARGET=APCAM_TARGET_ZT30',
                    f'-I{WEB}', f'-I{WEB.parent / "include"}', f'-I{WEB.parent / "camera_app/include"}',
                    f'-DAPP_REQUEST_PATH="{root / "request"}"',
                    f'-DAPP_REQUEST_LOCK_PATH="{root / "request.lock"}"',
                    f'-DAPP_STARTED_PATH="{root / "started"}"',
                    f'-DRUNTIME_DIR="{root / "run"}"',
                    f'-DAPP_DIR="{root}"', f'-DREPLACEMENT_CONFIG_PATH="{root / "camera.ini"}"',
                    f'-DREPLACEMENT_CONFIG_BACKUP_PATH="{root / "camera.ini.web.bak"}"',
                    str(source), str(WEB.parent / 'camera_app/src/config.cpp'),
                    str(WEB.parent / 'camera_app/src/protocol/camera_definition.cpp'), '-o', str(binary), '-lm'], check=True)
    subprocess.run([str(binary)], check=True)
