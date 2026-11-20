#!/bin/sh
# Historical filename retained for upgrades; AP CameraGimbal is the only app.
export LD_LIBRARY_PATH="/app/libs:${LD_LIBRARY_PATH:-}"
export TMP=/run
if pidof camera-app >/dev/null 2>&1; then
    echo 'camera-app is already running' >&2
    exit 0
fi
if pidof siyi_camera_app >/dev/null 2>&1; then
    echo 'Vendor camera is still running; refusing concurrent hardware ownership' >&2
    exit 1
fi
if [ ! -x /app/bin/camera-app ]; then
    echo 'camera-app is missing; web UI remains available for recovery' >&2
    exit 1
fi
# Unload SoC blocks camera-app never uses (display, DSP, NPU, decode, audio)
# so they stop drawing power; the boot uses clk_ignore_unused.
for module in ot_hdmi ot_mipi_tx gfbg ot_vo ot_dsp ot_npu_tsfw ot_npu_dfx \
    ot_npu_aicpu ot_npu_device ot_uvc ot_vdec ot_vfmw ot_jpegd ot_avs \
    ot_acodec ot_adec ot_aenc ot_ao ot_ai ot_aio; do
    rmmod "$module" 2>/dev/null
done
if [ -x /app/bin/mt11-web ]; then
    /app/bin/camera-app 2>&1 | /app/bin/mt11-web --capture-app-log replacement &
else
    /app/bin/camera-app >>/run/camera_app.log 2>&1 &
fi
