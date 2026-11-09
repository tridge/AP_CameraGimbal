# WebSplat v2: starting a splat from the video viewer

Detailed plan for the v2 phase of PLAN.md "Future versions". The video viewer
(`tools/viewer`, plot.ardupilot.org/video/) selects what to splat; WebSplat
(plot.ardupilot.org/websplat/) computes it. Both are served from one origin, so a recording open
in the viewer can be handed over without a second download or a second file choice.

Scope (v2): time range, target and height, view capture and Go in the viewer; a job request,
range-limited proposal and selection, and the captured start view in WebSplat. RGB sources with a
selectable processing resolution were added to v2 (see RGB sources). Not in v2 (v3): coverage
ring, snap to orbit, showing the finished splat in the viewer's map.

## User flow

1. The user opens an APCG raw thermal MKV in /video/ (local file or `?url=`). **Create splat**
   (map panel heading) is enabled once the file is indexed and is an FFV1 gray16 Matroska with
   pose telemetry; otherwise it is disabled with a tooltip saying why.
2. Pressing it opens the **splat panel** (in the map panel, above the map options) and switches
   the map to 3D. The panel has:
   - **Range**: start and end times with "Set start here" / "Set end here" buttons (current
     frame), and two handles drawn on the seek bar. Default: the whole recording. The panel
     shows the frame count and duration of the range.
   - The map shows only the in-range part of the video track; out-of-range parts are hidden
     (a "Show whole flight" checkbox shows them dimmed instead). Ground points where each
     sampled in-range frame's optical axis meets the terrain are drawn as small dots (at most
     500, evenly sampled), so the user sees where the camera looked.
   - **Target**: defaults to the median of those ground points ("where the camera looked").
     **Pick on map** makes the next map click set it (terrain intersection; track seeking is
     suspended while picking). Latitude, longitude and height (m AMSL) are shown and editable;
     ± buttons nudge the height by 1 m (shift: 10 m). A marker with a vertical line from the
     terrain to the target height shows it in the map.
   - **View**: **Use this view** captures the current 3D camera as the splat's start view;
     the panel shows it as bearing/elevation/distance from the target, with **Clear**. With no
     captured view WebSplat uses its own default (the hottest training view).
   - **Title** (default: the recording name without extension).
   - **Go** opens WebSplat in a new tab and hands it the request and the file. **Cancel**
     leaves splat mode and restores the track.
3. WebSplat starts the job without the confirmation form: ingest, proposal and selection run
   over the range, the target and height are the user's, and export starts the result view
   from the captured view. Its tab must stay visible during the run (as now).

## Request (`websplat.request` v1)

Built by the viewer, validated by WebSplat. All heights are metres AMSL. The viewer inverts
its own `ReviewMap.position()`: AMSL = Cesium cartographic height − `map.offset` (the active
offset, which is 0 with AMSL terrain such as plot's even when `amslToEllipsoidMetres` is set).
Terrain samples (`TerrainElevation.at`) are already AMSL.

```json
{"format":"websplat.request","version":1,
 "source":{"name":"flight2_thermal.mkv","size":123456789,"lastModified":1762600000000},
 "range":{"timescale":1000,
          "start":{"pts":1234,"offset":40960,"size":81234,"index":12},
          "end":{"pts":56789,"offset":98765432,"size":80111,"index":3456}},
 "target":{"lat":-35.1234567,"lon":149.1234567,"alt_amsl":612.4,"refine":true},
 "hfov":null,
 "view":{"camera":{"lat":-35.12,"lon":149.12,"alt_amsl":700.0},"fov_deg":60.0},
 "title":"flight2_thermal"}
```

- `range.start` / `end`: the first and last frame (inclusive) as packet identity from the
  shared demuxer (`tools/viewer/src/media/containers.js`, which ingest also uses): integer
  `timestampTicks` (`pts`) with the track `timescale`, plus the packet `offset` and `size`.
  `index` is a hint only. WebSplat finds each endpoint in ingest's packet-order frame list by
  offset and checks size, pts and timescale; the range is the packet-order index interval
  between them. If the in-range pts are not strictly increasing, or an endpoint does not match,
  the job fails with a message saying so (no silent fallback to the whole recording).
- `target.refine`: true lets selection's hot spot search refine the target (the native flow;
  it may move it up to ~300 m and prepare's hot spot then becomes the export target); false
  makes the picked target authoritative (`locate_hotspot: false`, export uses
  `frames.target_enu`). The viewer's checkbox "Refine to the hot spot" sets it (default on).
- `hfov`: the viewer's horizontal FOV override when set, else null (WebSplat then uses the
  recording's metadata as now). Viewer timing corrections (telemetry offset, time anchors) are
  not carried: WebSplat uses the recorded telemetry; the panel warns when they are non-zero.
- `view` is optional; `view.camera` is the captured camera position. The look-at point is the
  target (the splat orbits it), so only the camera position and vertical FOV are carried.
- `title` is optional (≤ 120 characters).
- Validation is strict: known keys only, finite numbers, lat in [-90, 90], lon in [-180, 180],
  start ≤ end, heights within [-500, 9000] m, fov in [10, 120] degrees; anything else is
  rejected with a message naming the field.

## Handover between the tabs

- The viewer opens `${websplatUrl}?request=<nonce>` with `window.open` (not `noopener`),
  where `websplatUrl` comes from `viewer-config.js` (default `../websplat/`) and the nonce is
  128 random bits in hex.
- WebSplat, seeing `?request=`, posts `{type:'websplat.ready', nonce}` to `window.opener`
  once its device check has passed (or a failure notice if it has not, so the viewer can say
  so) and waits up to 30 s.
- The viewer answers only messages whose `event.source` is the window it opened, whose
  origin is the WebSplat URL's origin, and whose nonce matches, with
  `{type:'websplat.request', nonce, request, file}` posted with `targetOrigin` = that origin.
  The request and File are frozen when Go is pressed (later viewer changes do not alter a
  pending handover). Each nonce is answered once. The listener stays until `websplat.accepted` /
  `websplat.rejected` arrives or the popup is closed (polled); the 30 s timeout applies only
  to waiting for `websplat.ready`, not to the copy that follows, which can take minutes. A blocked popup (`window.open`
  returns null) is reported in the panel.
- `file` is the File the viewer has open (local choice, or the OPFS-backed File of a `?url=`
  download), sent by structured clone. The bytes stay valid only while the underlying file
  does, so the viewer keeps its download's ownership lock (`remote-file.js`) and defers
  releasing or pruning it while a handover is unacknowledged and the popup is open, even if
  the user opens another recording meanwhile.
- WebSplat always copies a handed-over file into its job's OPFS storage (the "keep a copy"
  box does not apply), hashing it while copying, then saves the job and posts
  `{type:'websplat.accepted', nonce, job}` (or `websplat.rejected` with a reason). Only then
  does the viewer release the deferred lock.
- WebSplat accepts only messages from `window.opener` whose origin is its own (or, for local
  development only, any `http://localhost:*` / `http://127.0.0.1:*` origin when the page is
  itself served from one), with the matching nonce and a File whose name and size match
  `request.source`. After the job is saved it replaces `?request=` with `?job=<job id>`
  (`history.replaceState`), so a reload resumes exactly that job; `?request=` takes precedence
  over the remembered job at startup. A crash before that leaves `?request=` with no opener
  reply: the page then says so and offers to open the latest remembered job.
- Without an opener, after a timeout, or on a bad message: a notice "Open this from the video
  viewer's Create splat again" plus the normal file chooser.

## WebSplat changes

- `app/src/request.js` (new): `validateRequest(obj)` → normalised request or throws
  `RequestError`; `requestIdentity(request)` = the canonical output-affecting fields (range
  endpoints, target incl. refine, hfov, view); `requestKey` = first 16 hex digits of its
  `hashJson`; `resolveRange(ingestFrames, range)` → `{start, end}` indices or throws.
- Job identity: a request job is `${pipeline}-${sourceHash16}-${requestKey}`, so the same
  recording with another range or target is a separate job, and the same request resumes. On
  reopen the stored `config.request` identity must equal the new one exactly (else the page
  refuses with a collision message). A different title on reopen updates
  `config.export.title`, which re-runs only export (its `configKeys: ['export']`).
  `websplat.job.${pipeline}` in localStorage still remembers the latest job.
- Config written at job creation:
  - `config.request` = the canonical identity plus source name/size; it is not in any stage's
    `configKeys` (display and reopen checks only).
  - `config.proposal.range = request.range` (ingest's `configKeys` is already `['proposal']`,
    so the range is in ingest's stage key from the start and nothing is invalidated later).
    The ingest worker resolves it after indexing and before `propose()`, passes the resolved
    `start`/`end` to `propose()`, and records `range: {start, end, frames}` in its result. A
    resolution failure fails ingest with the message. Ingest still indexes and hashes the whole
    file (FFV1 indexing is cheap, ~0.2 s per 32 MB measured); proposal and selection are
    range-limited.
  - `config.proposal.hfov = request.hfov` when set.
  - `config.export.title` from `request.title`; `config.export.view = request.view` if given.
- Ingest always records `homeAltitude` (the whole-recording median, as `homeAltitude()`), also
  when the proposal fails, so a request never needs the hot spot or optical-axis triangulation.
- When select needs input and the job has a request: build `config.select` without showing
  the form: `{lat, lon, target_alt: alt_amsl − homeAltitude, locate_hotspot: refine,
  rotation: proposal's or null, hfov: request.hfov ?? proposal's ?? null, start, end, step}`
  from ingest's resolved range. If the proposal failed the job still proceeds (rotation and
  hfov fall back to select's defaults: the mkv's `rotation_deg` and metadata FOV).
  The non-request form path also keeps `start`/`end` from the resolved range when present.
- Frame budget: a request job sets `step = max(1, ceil(frames_in_range / 1000))`, so at most
  ~1000 candidate frames reach features and matching (prepare's `max_images=500` is a soft
  limit after mapping and keeps all hot images). The viewer shows the effective step.
- Export: when `config.export.view` is present the start camera is computed in the export
  worker from `frames.origin` (selection's ENU origin: target lat/lon, home altitude) with
  `select/geometry.js` `LocalFrame`: `position = ENU(view.camera)`, `target` = export's
  target (below), `fov_deg =
  view.fov_deg`. The distance from target is clamped to [1, 8] × `dataset.bounds.radius`
  along the same direction (a non-finite or non-positive radius skips the clamp and logs it).
  It is passed as the explicit `camera` that `stream.js` / `export.js` already accept. Without
  a view, `startCamera` as now.
- Export target policy: `config.export.target = 'picked'` for request jobs with
  `target.refine` false; export then uses `frames.target_enu` instead of prepare's hot spot
  (`export.js` currently prefers the hot spot) for both the metadata `target` and the start
  camera, with or without a captured view. Otherwise the existing precedence is unchanged.
- Page: a request job shows a short summary under the job header (range, frame count, target,
  view captured or not) and starts automatically (the user pressed Go). The pipeline selector
  stays hidden.
- CONTRACTS.md: `websplat.request` v1 section, the `config.request` / `config.export.view`
  manifest keys, and a change-log entry.

## Viewer changes

- `src/domain/splat-request.js` (new, pure, unit-tested): build and validate the request from
  viewer state; `rangeFrames(frames, startPts, endPts)`; `opticalAxisGround(record, elevation)`
  sampling helper (reusing `projection-pose.js` / `geometry.js` ray-terrain code);
  `medianPoint(points)`; `viewFromCamera(cameraLLA, target)` → bearing/elevation/distance for
  display.
- `src/ui/splat-panel.js` (new): the panel DOM, state (range, target, view, picking), the
  seek-bar range handles (two thin markers over `#seek` plus a shaded band), and the
  handover (`window.open`, message listener, nonce, 30 s timeout, errors shown in the panel).
- `src/map/map-view.js`: `setTrackRange(frames, recordForFrame, {dimOthers})` (rebuild the
  drawn video track from the in-range frames before decimation, with `buildVideoTrack`, so a
  short range inside a decimated long track still draws; track seeking uses the drawn
  segments; works with both the temperature and the plain track), `setLookPoints(points)`, `setSplatTarget(point|null)` (marker plus vertical
  polyline from terrain to target), `pickGround(screenPosition)` (globe pick + terrain AMSL,
  shared with the pointer-height code), `cameraPose()` → camera lat/lon/AMSL and vertical FOV
  in 3D mode, and a pick mode that routes the next left click to a callback instead of track
  seeking.
- `src/main.js`: the Create splat button, enabling rules, wiring the panel to state (current
  frame, frames, records via `alignedRecord`), and resetting splat mode when a new video is
  opened. The viewer keeps working normally while the panel is open (play, seek, hover).
- `public/viewer-config.js` and `deploy/plot-viewer-config.js`: `websplatUrl: '../websplat/'`.
- `index.html` CSP is unchanged (same origin; `window.open` is not restricted by CSP).
- README: a "Create a splat" section.

## RGB sources

Added to v2 (requested 2026-11-09): splats from the camera's RGB recordings (MPEG-TS with
MISB ST 0601 KLV, MP4/TS with APCG SEI), with a lower processing resolution selectable.
The native reference is `extract_ts_frames.py` (frames and pose priors from KLV), `sfm.py`
and `train_rgb.py` (undistort to pinhole, scale to `--width` 1280, plain 3DGS colour, L1 +
SSIM 0.2, SH degree 1).

- Viewer: Create splat is also enabled for H.264/HEVC recordings whose frames carry pose
  telemetry (position, platform attitude and sensor angles from KLV or APCG SEI) and an
  HFOV (recorded, or the FOV override). The request gains `"kind": "thermal" | "rgb"` (the
  viewer sets it from the source; WebSplat checks it against what ingest finds) and, for RGB,
  `"rgb": {"width": W}`: the processing width in pixels, chosen in the panel from Full
  (source width), 1920, 1280 (default) and 960 / 640, never above the source width.
  `target.refine` is false and hidden for RGB (no hot spot). Thermal requests keep the v1
  shape (`kind` defaults to thermal); `rgb.width` is part of the request identity.
- WebSplat: an `rgb` stage set alongside `thermal`, chosen by `request.kind` (and offered on
  the standalone page for a chosen .ts/.mp4 file). Stages reuse the thermal ones where the
  data allow; each stage keeps its own version and contract.
  1. ingest: index with the shared demuxer (`openMedia`), normalised per-frame telemetry
     (`telemetry.js normalizeTelemetry`) to the pose fields select needs; same range
     resolution as thermal. KLV has no relative altitude, so `homeAltitude` is the takeoff
     altitude as `extract_ts_frames.py` derives it (documented in CONTRACTS.md); APCG SEI
     carries relative altitude and uses the thermal rule. No hot spot proposal.
  2. select: frames at `fps` (default 3, as `extract_ts_frames.py`) within the range, target
     in view and within `max_range`, frame budget as thermal; decoded with the viewer's
     `FrameDecoder` (WebCodecs, falling back to its FFmpeg software decoder, e.g. HEVC on
     Linux Chrome), decoding each group of pictures once, scaled to `rgb.width` (aspect
     kept) with a high-quality resize, stored as RGB8 plus the grey image SIFT uses.
  3-5. features, match, map: as thermal on the grey images; the camera model is
     SIMPLE_RADIAL (one shared camera) seeded from the KLV HFOV at the processing width.
  5b. prepare: undistort to pinhole (as `train_rgb.py`), RGB8 targets, scene bounds and
     ceiling as `train_rgb.py` (`--radius`, ceiling grid); no temperature scale or hot spot.
  6. train: Brush's stock RGB path (the fork's numeric-target changes must leave it intact),
     L1 + SSIM 0.2, SH degree 1, Gaussian cap scaled to the GPU class.
  7. export: standard 3DGS PLY / SPZ with `kind: "rgb"` metadata (the splat viewer already
     renders RGB in true colour); start camera from the captured view, else the middle view.
- Time and memory scale with frames × pixels; the panel shows an estimate per width from
  measured rates, and warns above 1280 that the job may exceed GPU memory on small GPUs.

## Local development serving

`tools/GaussianSplat/WebSplat/test/v2/serve.mjs` (new): one static server for
`/video/` → `tools/viewer/dist` and `/websplat/` → the WebSplat site build
(`app/dist`, as `deploy.sh` stages it), with the headers both need, so the handover is tested
same-origin as on plot.ardupilot.org.

## Tests

- Viewer unit (`node --test`): request building and validation, range mapping including
  edge frames and missing pts, median target, view display maths, `amsl`/ellipsoid offset.
- Viewer browser (Playwright, the synthetic thermal MKV fixture): Create splat is disabled for
  a non-thermal video and enabled for the thermal fixture; set start/end from frames; the track
  shows only the range; pick a target on the map; nudge height; capture a 3D view; Go opens
  a popup, whose page (a test stub at the WebSplat URL) receives a request matching the
  expected JSON and a File of the right name and size; wrong nonce and wrong origin messages
  are ignored.
- WebSplat unit (node): `validateRequest` accept/reject cases, `requestKey` stability,
  `rangeIndices`, select-config derivation, export view → ENU camera and distance clamp.
- WebSplat page (Playwright on the v2 server, no GPU needed where possible): handover accepted,
  copied and acknowledged, job created with the request key and URL replaced by `?job=`;
  wrong origin/nonce/size rejected; two request tabs on one file then reload of the first
  resumes the first; same request with a new title re-runs export only; identity mismatch on
  reopen refused; proposal failure keeps the range; ingest fails clearly on an endpoint
  mismatch or non-monotonic pts (synthetic index); refine false keeps the picked target as the
  export target; export view uses `frames.origin` and `bounds.radius` with the clamp.
- Viewer browser additions: a short range inside a decimated long track still draws; a
  non-zero geoid offset with ellipsoid and AMSL terrain gives the right camera AMSL; the
  timing-correction warning; another recording opened during a pending handover does not
  prune the handed-over download until acknowledged; popup blocked; opener closed.
- RGB end to end: the 2025-10-16 RGB orbit recording (2560x1440 HEVC with KLV, and its 720p
  H.264 copy) at widths 1280 and 640, same flow; compare against the thermal splat of the
  same orbit for registration, and record timings per width.
- End to end (laptop GPU, Intel/AMD not NVIDIA): the orbit sample opened in the built viewer,
  a range around the orbit, target picked at the hot spot, view captured, Go; the job runs to
  export and the result view opens from the captured view. Record timings and screenshots in
  `notes/v2.md`.
- Existing suites stay green: viewer `npm test` and browser specs, WebSplat stage tests that
  run without a GPU.

## Commits (one per subsystem)

1. `viewer: Create splat selects a time range, target and view and hands them to WebSplat`
2. `tools/GaussianSplat: WebSplat accepts a job request from the video viewer`
3. `tools/GaussianSplat: WebSplat v2 plan, contracts and tests` (if not folded into 2)

## Risks and open points

- Ellipsoid vs AMSL: the viewer's offset is one configured constant; plot's terrain is AMSL,
  so on plot the conversion is exact for terrain picks. Camera height errors only move the
  start view slightly.
- Very long ranges: the request step caps candidates at ~1000; the panel shows the step and
  warns above 3000 frames in range that the job will take longer.
- File handles from `<input type=file>` stay valid in the other tab only while the user has
  not moved or edited the file; the hash check catches a changed file.
- `?url=` downloads live in the viewer's OPFS and may be deleted when the viewer starts the
  next download; the deferred lock release until `websplat.accepted` covers that. A handed-over
  recording is stored twice in OPFS (viewer download plus WebSplat copy) until the viewer's
  copy is pruned.
