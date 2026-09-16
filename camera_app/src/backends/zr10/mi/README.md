# SigmaStar MI ABI declarations

Unmodified header subset from OpenIPC/divinus revision
`0244156023a2ff13fc6571c7851332e0a1818d76` (MIT, see LICENSE):
https://github.com/OpenIPC/divinus/tree/0244156023a2ff13fc6571c7851332e0a1818d76/src/hal

The ZR10-specific 52-byte VIF device declaration and dynamically resolved API
subset are in ../mi_api.h. These were checked against the installed ZR10 SDK
and real GC4663 capture. The upstream 48-byte VIF declaration alone does not
match this camera. No vendor libraries are included here or in the package.

The region (`i6_rgn.h`) bindings from that same revision provide optional hardware overlays.
