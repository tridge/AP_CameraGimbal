# Visible browser qualification

Run from `tools/viewer`. The tests use the built application at
`http://127.0.0.1:4173/viewer/`; build and start `npm run preview -- --host
127.0.0.1` separately. `VIEWER_TEST_URL` can select another server, including
`http://127.0.0.1:5173/viewer/` for development. Qualification should finish
against the production build.

Chrome runs visibly inside a dedicated Xephyr X server, default `:99`. Start
that display separately with a suitable viewport; do not run these tests on
the operator's desktop. The config rejects `:0` and never falls back to headless
mode. Set `VIEWER_TEST_DISPLAY` and `VIEWER_CHROME` when necessary. The default
Chrome executable is `/opt/google/chrome/chrome`. Software WebGL flags allow
Cesium to run in Xephyr without a physical GPU.

```sh
npm run build
npx playwright test
```

Global setup generates small ignored fixtures with Python, NumPy, PyAV,
FFmpeg/libx264. PyAV must provide `add_data_stream` for KLV muxing. The MISB
fixture combines real H.264 with the independently checked ST0601 golden vector;
no camera build or MISB producer changes are required. Generated fixtures include calibrated FFV1 gray16 Matroska,
APCG H.264 MP4, MISB-only KLV MPEG-TS, typed mission/fence DataFlash and a broken
container, missing-calibration and unknown-UTC archives, plus a truncated
playable prefix. Regenerate with
`python3 scripts/generate-fixtures.py --force`.
No real flight data is added to the repository.

The default suite checks local file selection, exact raw pixel temperatures,
five palettes, display rotation and raw coordinates, keyboard and playback,
rapid seeks and file replacement, peak review aids, finding JSON/GeoJSON,
mission and inclusion/exclusion fences in both map modes, and error recovery.
Regression checks reach all four image corners at both display rotations, reject
unknown-time mission selection, retain playable prefixes after indexing errors,
keep rapid frame-step requests distinct during decoding, recover from invalid
Celsius stretches and calibration loss between frames, and preserve the active
map datum after failed settings. `mobile.spec.js` also checks a 390×844 viewport
for horizontal overflow and follows the deployed About / licences link,
verifying local notices and the pinned dependency integrity manifest.
Every test records HTTP requests and fails if it observes a method other than
GET, HEAD or OPTIONS. File inputs are populated locally; fixtures are never
served as HTTP assets or uploaded. Imagery is disabled through the normal
runtime configuration request to keep these tests offline and deterministic.

Opt into original local flight files explicitly:

```sh
VIEWER_REAL_THERMAL=/path/to/2026_08_28_raw_thermal_tlog.mkv \
VIEWER_REAL_RGB=/path/to/2026_08_28_13_01_45.v3_tlog.mp4 \
VIEWER_REAL_BIN=/path/to/2026_08_28_12_45_49.bin \
npx playwright test
```

The large thermal test generates an independent PyAV reference for first,
middle and last frames. It compares the full decoded raw-frame SHA-256 and
sample pixel value, then tests hover, seek, playback and export. The >2 GiB
recording exercises file offsets past the signed 32-bit boundary. The real BIN
check expects 81 mission items and one inclusion polygon with 26 vertices for
this flight. These tests are skipped when their environment variables are absent.

Artifacts default to `test-results/browser`; set
`VIEWER_TEST_OUTPUT` to change the destination. Screenshots include the visible
Chrome viewport and a physical Xephyr display capture after bringing that test
page forward. Failed tests retain screenshots and a Playwright trace; successful flows also save review screenshots, network audit
JSON, findings and empirical large-archive timing/heap measurements. Chrome's
`performance.memory` describes the page's JS heap only: it excludes workers,
WASM decoder memory, GPU and other browser processes, and is not a portable
memory guarantee.

## Terrain and stock Firefox

Set `VIEWER_REAL_TERRAIN_URL=/quantized/` alongside the real thermal/BIN paths
to enable `terrain-real.spec.js` in Chrome. For local production preview use
`VIEWER_BASE_PATH=/video/ VIEWER_TERRAIN_PROXY=https://plot.ardupilot.org npm run preview`;
copy the plot runtime config into dist/viewer-config.js first. The deployed
site already supplies both. This test checks the hovered raw pixel,2D/3D
invariance, absence of manual elevation controls, and a mark resolving against
its captured frame while playback advances.

Stock Firefox is qualified separately through geckodriver/W3C WebDriver, without
a Selenium dependency. It starts a private profile in the dedicated Xephyr
window and closes only its own browser. Supply a recent geckodriver executable:

```sh
python3 scripts/qualify-firefox.py \
  --driver /path/to/geckodriver --display :99 \
  --url https://plot.ardupilot.org/video/ \
  --thermal /path/to/2026_08_28_raw_thermal_tlog.mkv \
  --log /path/to/2026_08_28_12_45_49.bin \
  --output /tmp/viewer-firefox-results
```

The defaults select frame8315, raw pixel337,373 from the current test flight;
`--frame`, `--x` and `--y` support other recordings. Results include browser
version, projected coordinates/terrain provenance, the marked observation and
screenshots in both map modes. No recording bytes are uploaded. Default OSM
imagery still makes normal map-tile requests revealing the viewport to OSM.

For a genuine cross-origin terrain test, serve dist below /video/ using a plain
static server and set VIEWER_REAL_TERRAIN_URL=https://plot.ardupilot.org/quantized/.
Do not enable the Vite terrain proxy. The real-terrain Chrome case and the stock
Firefox qualifier also force a Content-Type JSON preflight when the terrain
origin differs. A local /quantized/layer.json request should return404.

## Follow and map-track navigation

map-navigation.spec.js tests the checked Follow control below the map, zoom
preservation, frozen camera when disabled,3D camera transport, dragging and
click-vs-drag behaviour. Track clicks are exercised in2D/3D at devicePixelRatio2
and resolutionScale1.5, with exact and near-line positions, background misses
and the original thermal/BIN recording. Unit cases cover gaps before decimation,
low-rate long tracks, overlapping and stationary passes, display bounds, aligned
pose mapping and cancelled asynchronous builds.

Pass --navigation to scripts/qualify-firefox.py for stock Firefox Follow and
physical map clicks on the real recording, in both modes. The resulting report
records selected frame numbers and stores navigation screenshots.

The Firefox navigation qualification also checks on-map zoom buttons and actual
keyboard +/− input. map-zoom-footprint.spec.js covers ordinary/numpad keys,
editable-field and browser-shortcut handling, and zoom retention during Follow.
Its moving partial-footprint test reads rendered yellow pixels in2D/3D to detect
the asynchronous geometry flicker that entity visibility flags cannot reveal.
