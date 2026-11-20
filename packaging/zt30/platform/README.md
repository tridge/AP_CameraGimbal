# ZT30 retained runtime

These are the minimal runtime libraries needed to link the native ZT30 SDK
adapter and qualify application bundles for the tested SIYI ZT30 platform
(installed vendor camera version 0.1.9, glibc 2.30 and GCC 9 C++ runtime).
They are copied byte-for-byte from `/lib` and `/config/lib` on that platform;
`SHA256SUMS` pins every included file and symlink target's contents.

The build checks these inputs, then resolves every native application's and
adapter's transitive ELF dependencies and required symbol versions against
them. The bundle records the required runtime files' paths, lengths and hashes.
The camera must have matching retained libraries to accept that bundle.

These files are build inputs only. Application updates do not install them or
change the camera's retained platform partitions. No vendor camera executable,
configuration, credentials, calibration, kernel modules or full firmware image
is included here. The libraries retain their respective upstream/vendor
licenses; they do not inherit the AP CameraGimbal application's license.
