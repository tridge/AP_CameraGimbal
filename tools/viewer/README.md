# Video flight review

A local-file web viewer for reviewing AP_CameraGimbal imagery and investigating
thermal observations. It runs as a static site: videos, logs, raw thermal pixels
and findings stay in the browser. There is no upload endpoint or video backend.
A video alone supplies playback, embedded telemetry and its aircraft track.
Optional flight logs add the mission, geofences, home and actual flight track.

## Run locally

Requires Node24 or newer for development, and a current browser with WebGL.
Qualification uses Google Chrome; H.264 decoding uses WebCodecs where supported.
Other codecs use the included local software decoder. No system FFmpeg is needed
for normal browser use.

```sh
cd tools/viewer
npm ci
npm run prepare:assets
npm run dev
```

Open **http://127.0.0.1:5173/viewer/** and select a local recording. HTTPS is
required when hosting away from localhost, including for native WebCodecs.
The app is not intended to be opened directly as a `file://` document.

```sh
npm run build
npm run preview
# http://127.0.0.1:4173/viewer/
```

The deployment artifact is `dist/`. It includes the decoder, map library,
workers, CSS and runtime configuration; no public CDN is needed. Browser
runtime does not require Node or Python. Do not commit generated assets,
`node_modules`, recordings or local test results.

## Supported recordings

| Container                                         | Video        | Embedded telemetry                                               |
| ------------------------------------------------- | ------------ | ---------------------------------------------------------------- |
| MP4 / MOV, including fragmented camera recordings | H.264 / HEVC | APCG JSON SEI                                                    |
| MPEG-TS                                           | H.264 / HEVC | APCG JSON SEI and MISB ST0601 KLV                                |
| APCG raw thermal Matroska                         | FFV1 gray16  | `apcg.thermal.v1` block additions and nested `apcg.telemetry.v1` |

The two camera telemetry encodings are detected automatically. Co-carried APCG
SEI takes precedence over KLV because it preserves additional fields and full
precision. KLV checksums and invalid sentinels are checked; unavailable values
stay unavailable. Standard full-range tags90/91 supersede legacy pitch/roll
fields, including when explicitly invalid. Existing raw thermal archives from
`tools/thermal_to_video.py` retain native uint16 pixels and calibration.

The container index is incremental and uses bounded `File.slice()` reads.
Playback begins before the full telemetry scan finishes. A malformed tail keeps
its playable prefix and an explicit error. Indices scale with frame count; full
video payloads and decoded images are not retained in them. See
[codec details and measured limits](docs/CODECS.md).

MP4 edit intervals control which images are presented; excluded keyframe preroll
remains available to the decoder. Captures starting mid-GOP open at the first
available decodable frame while retaining their original elapsed timeline.
Saved review times are migrated with their original frame/UTC associations;
observations outside an edited clip remain exportable and are labelled outside
the clip. Calibration anchors may precede its first presented image.
MPEG-TS indexing allows up to two decode-clock seconds or180 video descriptors
for delayed KLV; metadata beyond that bound is explicitly diagnosed.

Native HEVC support varies by browser/platform. The bounded software fallback
works with the tested1080p/4K recordings, but4K HEVC may review at approximately
one frame per second on the tested machine. It is not a real-time4K promise.
Unsupported dimensions or excessively long/large decode groups fail explicitly.
Raw thermal review is substantially faster and retains exact sensor values.

## Review workflow

1. **Open video.** Playback, frame stepping, absolute recorded UTC and pose UTC
   are separate. Space plays/pauses, arrows step frames, and `M` marks the selected
   pixel. Telemetry is outside the image. Raw X/Y fields allow keyboard pixel
   selection; mouse hover also selects a sensor pixel.
2. **Choose a palette and stretch.** White hot, black hot, Inferno, iron and
   Viridis change only display pixels. Raw Celsius readings require explicit
   calibration; an encoded display image never acquires invented temperatures.
   Fixed Celsius limits are available only on calibrated raw frames.
3. **Add a BIN or tlog** when mission/fence context is useful. Plan selection
   defaults to the video pose time; the latest-log overview is explicit. Unknown
   time, unknown home and unconfirmed/incomplete mission uploads are labelled.
   Inclusion/exclusion polygons and circles retain their types in2D and3D.
4. **Inspect ground positions.** The configured quantized terrain service supplies
   pixel-to-ground intersections automatically, including in2D. There are no
   manual elevation files or flat-altitude controls. Missing/stale camera pose,
   upward rays, missing terrain coverage and loading/network failures are
   explained next to the pixel reading. Picking requires the terrain endpoint
   to be reachable; it may be a local server for offline field use.
   Images crossing the horizon show the ground-visible perimeter as open yellow
   lines, with a partial-footprint explanation. Lines never close across sky or
   unavailable terrain. Cameras at/below DEM height cannot produce a footprint.
5. **Check alignment.** A telemetry offset selects earlier/later embedded pose
   without changing the image or recorded UTC. Time anchors can map playback
   seconds to corrected UTC piecewise; two or more anchors do not extrapolate
   outside their interval. The offset is additional and remains visible.
6. **Inspect thermal peaks and mark observations.** Local contrast highlights
   assist visual review; they do not classify ignitions or prove a restored
   surface temperature. Findings keep raw sensor coordinates, frame/PTS/UTC,
   calibration, raw-orientation hypothesis, pose, terrain and clock provenance.
   Add notes and export JSON/GeoJSON. Unmapped observations are retained.

The projection uses raw sensor orientation0. Display rotation0/180° changes
only presentation and pointer conversion. Existing metadata rotation is retained
in findings; it does not silently change the projection hypothesis. Field FOV,
boresight, timing and elevation calibration still determine ground accuracy.
A horizontal FOV override is available in Settings and is recorded in findings.
For raw thermal Matroska, the outer thermal HFOV defines the lens; the nested
snapshot may describe RGB and is never used as a thermal FOV fallback. An
explicit user override takes precedence and is identified in exported findings.
Some cameras report commanded gimbal targets without physical angle feedback;
unless telemetry identifies this, the viewer cannot distinguish the two. Check
alignment while the camera is moving before interpreting projected locations.

The **Follow** checkbox below the map is enabled by default. It centres on the
current video's aligned aircraft position while preserving zoom and3D camera
orientation. Uncheck it to pan and inspect a fixed area; **Center** recentres
once without enabling Follow. Dragging temporarily suspends automatic movement.

Use the map's **+ / −** buttons, or keyboard **+ / −** (also **=** and numpad
keys), to zoom in2D and3D. Follow retains the selected zoom. Shortcuts also work
from the timeline or focused map buttons; text/number inputs, dialogs and browser
Ctrl/Command zoom shortcuts keep their normal behaviour.

Click the cyan **video track**, or within8 screen pixels, to pause and seek to
that location in the recording. At overlapping passes the time closest to the
current playback position wins. White log tracks and mission/fence overlays do
not seek the video. Position/clock gaps stay disconnected, and the track is
rebuilt when telemetry offsets or time anchors change. Long tracks build in
chunks; a visible hint identifies any display simplification.

Flight-log receipt UTC, autopilot boot-derived UTC and video PTS remain distinct.
Loading a log does not replace valid camera pose with vehicle pose. When a log
has multiple plan/home versions, frame-time selection avoids using a future
upload or a later arming's home. Diagnostic details are in Settings.

## Maps and Google keys

`public/viewer-config.js` is a **public runtime configuration template** copied
to `dist/viewer-config.js`. Configure it before building, or maintain the deployed
copy separately from application releases. All browser keys are public to the
page; restrict Google keys to the intended hosting origin.

```js
window.VIEWER_CONFIG = {
  imagery: 'google', // none, osm, satellite, google
  googleMapsApiKey: 'YOUR_ORIGIN_RESTRICTED_MAP_TILES_API_KEY',
  terrainUrl: 'https://plot.ardupilot.org/quantized/', // shared service, CORS enabled
  terrainDatum: 'amsl', // plot SRTM tiles; use ellipsoid for ellipsoidal terrain
  amslToEllipsoidMetres: 0,
};
```

Esri satellite imagery is the default. Map tile requests reveal the viewport to
the selected imagery provider, including the aircraft area once a video is opened.
No file bytes are sent. OpenStreetMap and an offline globe are selectable. Google imagery uses the
[Google2DImageryProvider Map Tiles API](https://cesium.com/learn/cesiumjs/ref-doc/Google2DImageryProvider.html).
No Google account, Cesium ion token or cloud upload is needed for local review.
Provider keys and terrain/imagery choices entered in Settings affect the current
session. Review annotations and per-file alignment preferences use localStorage.

The default terrain URL is `https://plot.ardupilot.org/quantized/`, so another
static host does not need its own terrain files or proxy. The viewer permits
this shared HTTPS endpoint and same-origin terrain services; its CSP allowlist
includes the shared terrain path. If an installation already serves terrain,
`terrainUrl: '/quantized/'` continues to work. The plot service supports public,
credential-free GET/HEAD access and OPTIONS preflights. It does not receive
recording/log bytes; terrain tile requests reveal the area being viewed.

The configured quantized-mesh endpoint supplies both3D visual terrain and camera
ray intersections, independently of the visible map mode/zoom. The plot server's
SRTM tiles store orthometric/EGM96 (AMSL) heights: set `terrainDatum: 'amsl'`.
For an ellipsoidal terrain service use `terrainDatum: 'ellipsoid'` and set
`amslToEllipsoidMetres` to the local geoid separation. The picker then subtracts
that offset from terrain heights to match AMSL camera altitude. Displayed AMSL
entities use the same reference as the terrain. Findings retain the datum,
source URL, sampling level and tile keys.

CPU-side tile interpolation uses a bounded cache and an index of mesh triangles.
Each ray keeps a fixed available level (up to12); missing coverage at that level
has no flat/ellipsoid fallback. The tile service must advertise availability.
Map tiles connect to the selected provider; source file bytes are never sent
with them. Choose Offline globe for no external imagery requests.

## Deployment prefixes

The default development prefix is `/viewer/`. Set `VIEWER_BASE_PATH` at build
and preview time to install at another prefix, including map workers and WASM:

```sh
VIEWER_BASE_PATH=/video/ npm run build
VIEWER_BASE_PATH=/video/ npm run preview
# http://127.0.0.1:4173/video/
```

Copy **all** of `dist/` to the static web directory. Do not copy only
`index.html`. See [the plot installation](docs/DEPLOYMENT.md) for the isolated
release layout, runtime terrain configuration and rollback procedure.

Example nginx shape (adapt the filesystem path to the server):

```nginx
location = /viewer { return 301 /viewer/; }
location /viewer/ {
    alias /srv/fire-viewer/;
    index index.html;
    add_header X-Content-Type-Options nosniff always;
}
```

Serve JavaScript/CSS/WASM with their normal MIME types (`application/wasm` for
`.wasm`). Single-thread decoding deliberately does not require cross-origin
isolation headers, which can interfere with map imagery. The HTML CSP permits
same-origin assets/workers and the listed imagery providers. `unsafe-eval` is
needed by Cesium's bundled Knockout widget binding; filenames/log text are never
inserted as executable HTML. There are no remote script/CDN dependencies.

Deploy releases atomically and preserve the public runtime configuration.
Dependency assets use versioned paths to avoid mixing cached workers/WASM after
upgrades. Keep old release assets available while old tabs remain open. Verify
`/viewer/`, a native RGB frame, a raw thermal frame and both map modes after a
server update. The deployed About / licences link includes full licence text, notices and the pinned dependency manifest. Third-party notices/source references are in
[THIRD_PARTY.md](THIRD_PARTY.md).

## Structure and future live sources

[Architecture and extension contracts](docs/ARCHITECTURE.md) describe the module
boundaries. In brief:

- `src/media`: file-backed containers, telemetry decoders, worker index, native
  WebCodecs and bounded software decoder. Source PTS and raw payloads stay here.
- `src/domain`: independent clocks, calibrated thermal tools, projection/DEM and
  flight-log models. These modules have no DOM or map-library dependency.
- `src/map`: one map/entity model shared by2D and3D; atomic provider changes.
- `src/ui`: layout and styling. `src/main.js` coordinates review state and events.
- `tests`: format golden vectors, domain tests, real-file opt-ins, and headed
  browser qualification with reproducible small generated fixtures.

SupportProxy live ingestion is the next source adapter, not implemented yet.
It should emit the same frame/track batches and `readPacket` API, with bounded
packet retention, generation/cancellation, PTS discontinuity handling and explicit
clock provenance. An HTTPS-hosted viewer will require an HTTPS/CORS-capable proxy
endpoint; raw RTSP is not a browser transport. The playback, telemetry, thermal
and map layers can then be reused.

## Checks

```sh
npm test
npm run build
# On a desktop, start an isolated test display:
Xephyr :99 -screen 1500x1000 -ac -noreset -nolisten tcp
# In another terminal:
npm run preview
VIEWER_TEST_DISPLAY=:99 npm run test:browser
```

Browser fixtures additionally require Python with PyAV/NumPy, system FFmpeg and
a C++ toolchain. No real flight recording is committed. See
[TESTING.md](docs/TESTING.md) for real-data opt-ins and screenshot/network audits,
[REVIEWS.md](docs/REVIEWS.md) for the staged Claude reviews, and
[PLAN.md](docs/PLAN.md) for the accepted plan and feasibility evidence.
