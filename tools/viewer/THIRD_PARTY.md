# Dependencies and source references

Versions are pinned in package.json/package-lock.json. Application runtime assets
are copied locally by scripts/prepare-assets.mjs, with versioned dependency URLs.
No UAVLogViewer source was copied; it was inspected as a mission/fence behavior
reference. The new parsers use MAVLink and ArduPilot record definitions.

| Component                   | Pinned version | License / upstream                                          |
| --------------------------- | -------------- | ----------------------------------------------------------- |
| CesiumJS                    | 1.146.0        | Apache-2.0, https://github.com/CesiumGS/cesium              |
| FFmpeg WebAssembly core     | 0.12.10        | GPL-2.0-or-later, https://github.com/ffmpegwasm/ffmpeg.wasm |
| Vite (build only)           | 8.3.5          | MIT, https://github.com/vitejs/vite                         |
| Playwright (test only)      | 1.64.1         | Apache-2.0, https://github.com/microsoft/playwright         |
| Prettier (development only) | 3.6.2          | MIT, https://github.com/prettier/prettier                   |

Cesium's embedded credits/notices are preserved, including imagery-provider
attribution. The viewer disables implicit Cesium ion services; a visible SDK
credit is not evidence of file upload or an ion dependency.

The pinned @ffmpeg/core package is a full GPL build (not an FFV1-only LGPL build).
Its decoder is an isolated, replaceable same-origin worker dependency. Preserve
its license and provide the corresponding build/source materials when deploying
that binary. Upstream source/build definitions and release history are at
https://github.com/ffmpegwasm/ffmpeg.wasm/tree/main and its versioned releases;
package-lock.json records the exact distributed artifact and integrity digest.
The FFmpeg core's dependencies and build provenance should be retained with the
server release. A future custom codec-only core can replace it through the same
worker interface without changing the application model.

Useful primary API/format references:

- https://ffmpegwasm.netlify.app/docs/api/ffmpeg/ — worker API and core builds
- https://cesium.com/learn/cesiumjs/ref-doc/ — scene/map/imagery APIs
- https://developers.google.com/maps/documentation/tile/2d-tiles-overview — Google map tiles
- https://mavlink.io/en/messages/common.html — mission, fence and log messages
- camera_app/src/recording/misb.cpp and misb_ts.cpp — this repository's KLV encoder
- tools/thermal_to_video.py — lossless thermal archive format/calibration
