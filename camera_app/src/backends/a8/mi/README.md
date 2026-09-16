# SigmaStar Mercury6 MI bindings

Struct and function-pointer definitions for the SigmaStar Mercury6 (SSC8836)
MI libraries (`libmi_sys/sensor/vif/isp/scl/venc.so`), loaded at run time with
`dlopen`. Taken from OpenIPC divinus (https://github.com/OpenIPC/divinus,
MIT licence), `src/hal/star/m6_*.h` at commit ec15216, with `MI_SNR_InitDev`
added to `m6_snr.h`. No SigmaStar SDK headers are involved.

The region (`m6_rgn.h`) bindings from that same revision provide optional hardware overlays.
