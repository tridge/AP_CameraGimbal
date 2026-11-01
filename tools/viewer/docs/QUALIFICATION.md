# Browser release qualification

14 tests passed (1.1m) against the latest production build at http://127.0.0.1:4173/viewer/, in visible Chrome 155 on dedicated Xephyr :99. The complete run log is [browser-release.log](/data/review/web-viewer/browser-release.log); the machine-readable report is [qualification.json](/data/review/web-viewer/qualification.json).

The original 2,759,203,562-byte thermal recording indexed all 16,630 frames. First frame appeared in 0.766s and indexing finished in 13.791s. Browser raw SHA-256 matched independent PyAV and embedded source hashes for frames 0, 8315, 16629. The final packet offset was 2,759,094,706 bytes, exercising offsets beyond 2 GiB. Empirical main-page JS heap was 97,524,720 bytes (93.0 MiB); this excludes workers, WASM, GPU and other browser-process memory and is not a portable memory guarantee.

All 611 audited application requests were GETs to the local preview origin; no file upload or nonlocal request occurred. Audits cover the About popup as well as the viewer. All 14 tests left persistent network-request JSON files under [browser-release](/data/review/web-viewer/browser-release).

Coverage includes APCG MP4, MISB-only KLV TS, full raw thermal image and all four corner hit-tests at 0/180 degrees, keyboard raw X/Y, five palettes, first/middle/last seeks, playback, rapid replacement and pending frame steps, calibrated hover, peak review, JSON/GeoJSON findings, explicit mission time selection, unknown UTC, missing/transitioning calibration, malformed containers and playable-prefix end stop, invalid stretch fallback, rejected terrain settings atomicity, and anchors+offset disclosure. The real DataFlash has 81 mission items and one 26-vertex inclusion polygon, verified independently against FNCE records. Both map modes passed.

The 390×844 mobile viewport has no horizontal overflow. The deployed About link opens a noopener popup; component notices, full GPL2, Cesium licences and pinned dependency integrity manifest all returned 200 local GET responses. Manifest versions match FFmpeg 0.12.10/Cesium 1.146.0.

Review images:

- [Mobile thermal](/data/review/web-viewer/browser-release/mobile-mobile-review-fits3-c9c0b-ces-load-through-local-GETs/mobile-thermal-review.png)
- [Deployed About/licences](/data/review/web-viewer/browser-release/mobile-mobile-review-fits3-c9c0b-ces-load-through-local-GETs/mobile-about-licences.png)
- [Complete thermal corners at 180 degrees, physical Xephyr](/data/review/web-viewer/browser-release/regressions-entire-thermal-8f26c-in-reachable-after-rotation/complete-thermal-corners-rotated-xephyr.png)
- [Original thermal middle frame, physical Xephyr](/data/review/web-viewer/browser-release/real-files-real-large-ther-15d80--middle-and-last-raw-frames/real-thermal-frame-8315-xephyr.png)

All 12 physical Xephyr captures and viewport screenshots remain under browser-release. Build index SHA-256: `4bcf33169f44ce357fc8ff3b13745735a77443d5fe7df6b2220d2c14d11a3fa0`. Asset names: `/viewer/assets/index-BqRiU9aw.js`, `/viewer/assets/cesium-BZlDMVmh.js`, `/viewer/assets/cesium-B9DDZdqH.css`, `/viewer/assets/index-DMjUH2Cl.css`.

Actual command, from `/home/tridge/project/UAV/AP_CameraGimbal/tools/viewer`:

```sh
VIEWER_TEST_OUTPUT=/data/review/web-viewer/browser-release \
VIEWER_REAL_THERMAL=/home/tridge/project/UAV/Phoenix/logs/2026-08-28/2026_08_28_raw_thermal_tlog.mkv \
VIEWER_REAL_RGB=/home/tridge/project/UAV/Phoenix/logs/2026-08-28/2026_08_28_13_01_45.v3_tlog.mp4 \
VIEWER_REAL_BIN=/home/tridge/project/UAV/Phoenix/logs/2026-08-28/2026_08_28_12_45_49.bin \
./node_modules/.bin/playwright test --reporter=list
```
