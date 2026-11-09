# WebSplat: thermal Gaussian splats made entirely in the browser

Plan v2.1, 2026-11-08: v2 revised after codex's review of v1 (REVIEW-plan.md); v2.1 adds what
stage 0 measured about the reference (notes/stage0.md).

Goal: a static web page where a user opens an APCG radiometric thermal MKV (with embedded
per-frame telemetry) and gets a thermal Gaussian splat, computed on their own GPU with no
installed software, then views and downloads it. Release 1 is scoped to thermal orbits, in
Chrome on two machines: Tridge's Linux laptop (RTX PRO 1000 8 GB; WebGPU needs
`--use-angle=gl --enable-features=Vulkan`) and Windows 11 (RTX 4070 12 GB). Large RGB datasets,
a CPU fallback, WebGPU SIFT and a custom trainer each need a separate justification later.

Canonical input: the 5 minute orbit `2025-12-18_log80_thermal_orbit_1202-1207UTC.mkv`
(201 frames, 640x512). The native pipeline (`tools/GaussianSplat`: extract_frames.py →
sfm.py --mapper prior → train.py) is the reference; every browser stage is checked against its
output for the same input.

Files here: PLAN.md (this), PROGRESS.md (status, measurements, decisions, issues),
CONTRACTS.md (stage data formats), REVIEW-<stage>.md (codex reviews), `app/` (the web app),
`test/` (acceptance checks and the browser harness). Local paths are given as `$WEBSPLAT_DATA`
(shared data: input, native reference, builds) and `$WEBSPLAT_RESEARCH` (feasibility research
and prototypes); their values for a machine are in the untracked `local.env`. Nothing large or
generated goes in git.

## Order of work

1. Freeze the reference and the contracts (stage 0).
2. Two feasibility gates, independently: G1 thermal training in Brush, G2 telemetry-seeded
   mapping in wasm. A credible completion estimate is written after both.
3. An export/viewer round trip with a tiny known model (stage 7a), early.
4. Ingest and frame selection (1-2), features and matching (3-4), mapping (5), scene
   preparation (5b), training (6), result (7), integration and recovery (8).

Stages 1-2, 3-4 and G1/6 can proceed in parallel after stage 0. Each stage is done by an agent,
reviewed by codex (REVIEW-<stage>.md), fixed, and committed by the coordinating session as one
commit per stage. Nobody pushes; Tridge pushes.

## User experience

- One page. Choose an MKV (read locally, never uploaded) or the sample. After indexing, the
  page proposes the target (hot spot location, altitude), rotation and HFOV with a frame
  preview, all editable, and explains absent or inconsistent telemetry.
- A stage list always visible. States: waiting, running, checkpointing, paused, interrupted,
  done, failed. Each running stage shows its natural unit: bytes while indexing, frames, pairs,
  bundle-adjustment rounds and iterations (with uncertain remaining work), training iterations
  plus Gaussian count. Elapsed and estimated remaining time per stage and overall.
- Estimates: start as a broad overall range from the stage work counts and the measured
  baselines in PROGRESS.md for the detected GPU class, refined from completed work (smoothed
  rates; training modelled as slowing with Gaussian count). Shader compilation, loading,
  evaluation and checkpoint time are tracked separately from throughput. No per-kernel startup
  benchmark until real measurements show it helps. Estimates are rounded and labelled "about".
- Live previews: selected frames with the hot spot; camera path and sparse points after
  mapping; the splat during training every few hundred iterations (capped resolution).
- Stop, and resume after reload from the last completed stage. Training resume restarts from
  saved splat parameters (not exact optimiser state) in v1. Screen Wake Lock is requested while
  visible; the page warns that hiding the tab can slow or stop work.
- Result: the splat in the viewer (temperature colormap, range, pointer readout), downloads
  (PLY with the `apcg_splat` header, SPZ plus meta.json, COLMAP model).
- Clear errors: no WebGPU (with the Chrome-on-Linux flag hint), allocation failure or
  `device.lost` (with what was saved and how to resume), MKV without telemetry, storage quota.

## Architecture

Static files (vite build). Main thread: UI and the pipeline controller. Compute stages in module
Web Workers; CPU stages in a worker pool (one wasm instance each; no SharedArrayBuffer or
COOP/COEP in v1), the pool size bounded by measured heap use. GPU stages each own a WebGPU
device, request the limits they need (storage binding and buffer size defaults are 128/256 MiB)
and release it when done. Stages exchange data through OPFS using the versioned formats in
CONTRACTS.md; each output records its inputs' hashes and configuration so changed inputs
invalidate downstream outputs. A checkpoint generation is published only after its data is
written; persistent storage is requested and denial handled; two tabs on one job are refused.
The source MKV is either copied into OPFS or re-selected and verified by size and hash on resume.

Resource budget for the canonical input (planned peak use, checked by tests): raw frames
125.6 MiB as uint16 (251 MiB as float32 targets), descriptors 201 MiB (201 x 8192 x 128 uint8,
matched in batches), plus each wasm heap, optimiser and rasteriser scratch, preview and export.
Full runs must succeed on the 8 GB laptop GPU, with allocation failure and `device.lost`
handled.

Reuse:
- `tools/viewer/src/media`: MKV/EBML parsing, telemetry block additions, FFmpeg-wasm FFV1
  decoder (gray16). It decodes one requested frame at a time for seeking; stage 1 adds a
  bounded batch interface if its throughput is too low.
- `tools/GaussianSplat/viewer`: the Spark viewer and PLY metadata. It is a page-level WebGL
  app; stage 7a defines how WebSplat embeds it (iframe or shared module) next to WebGPU work.
- colmap-wasm (BSD-3, COLMAP 4.2.1 in wasm), as a pinned fork. Its stock C API hard-codes a
  SIMPLE_PINHOLE camera with a guessed focal length, combines verification, calibration and the
  GLOMAP mapper in one call, and disables the filesystem; the fork must add explicit cameras,
  telemetry poses with covariance, separate verification, prior mapping, full reconstruction
  export, and progress/cancellation points. Building it needs emsdk (see PROGRESS.md).
- Brush (Apache-2.0, Rust/Burn/WebGPU), as a pinned fork for thermal training. It packs
  training images into RGBA8, so the fork must keep float targets.

## Stages

Rough times are hypotheses, labelled: [M] measured, [E] estimated. They are for the canonical
input on the laptop / win11.

### 0. Reference, contracts, skeleton, harness
- Freeze the reference: reproduce `$WEBSPLAT_DATA/ref/work` natively with a recorded
  extract_frames.py command line; record the sfm.py prior-mapper run; train a native SH-only
  baseline (train.py without `--lobe`/`--source-gain`, same holdouts) for the first training
  milestone, keeping run13 (lobe, source gain) as the later target. Freeze holdout file names.
- CONTRACTS.md: versioned schemas for frames (identity, order, timestamps, raw encoding and
  byte order, per-frame radiometry), poses (camera-to-world vs world-to-camera, ENU datum,
  camera model and intrinsics), features (keypoint layout), matches and verified geometry,
  tracks, reconstruction, training dataset (splits, temperature scale, undistortion), splat
  output; plus the job manifest with hashes and invalidation rules.
- App shell, stage list and progress/ETA model, worker and OPFS plumbing, device checks.
- Harness: drives the page in headless Chrome on the laptop (flags above) and on win11 (CDP
  over ssh), collects per-stage timings and GPU memory, runs the acceptance checks.
- Accept: a dummy pipeline of fake stages shows correct progress, states and ETA, resumes after
  a reload (including a reload during a checkpoint write), refuses a second tab, and runs under
  the harness on both machines.

### G1. Thermal training feasibility in Brush (gate)
Prove, using the native reference poses and undistorted frames, that a worker-hosted Brush
build trains on scaled-temperature float targets with no 8-bit packing or gamma, applies the
hot-pixel weighted loss, evaluates the frozen holdouts, and exports correct thermal values.
RGB-shaped rendering is acceptable at first. Measure time and peak GPU memory on both machines.
Accept: against the native SH-only baseline, per-view hot and cold PSNR, absolute peak error
distribution, hot-region overlap and false hot detections within tolerances written in
notes/g1.md before the work starts. Native training itself varies between identical runs (per
view: cold PSNR up to 2.3 dB, hot IoU 0.21, absolute hot peak error 3.7 C), so per-view bounds
are taken against the worse of the two native baseline runs, and cold PSNR and IoU are judged
on their means. If Brush cannot be adapted, stop and report; a custom WGSL
trainer is a separate project decision.

### G2. Telemetry-seeded mapping in wasm (gate)
Prove in Node (then a worker) that the colmap-wasm fork reproduces `sfm.py --mapper prior` from
the native verified two-view geometries in the reference database: telemetry poses with
position priors, triangulation, pose-prior BA with re-triangulation at 40/16/8/4 px and
re-admission. Compare against the reference model mapped from the same database (the native
prior mapper is deterministic given the database; a fresh native run with new GPU SIFT
features already differs by 0.15 m / 0.06 degree median). Accept: camera position median
within 0.3 m and the 95th percentile within 1 m,
rotation median within 0.1 degree and 95th percentile within 0.3 degree of the native model;
observations per image, track lengths and triangulation angles comparable; reprojection error
within 10%. The `ba.c` prototype counts as a fallback only after it reproduces this from raw
verified matches (tracks, conflicts, triangulation, merging), not from the native final tracks.

### 1. Ingest   [E] under 1 min, to be measured
Index the MKV (progress in bytes), then decode FFV1 frames and telemetry in batches. Accept:
raw pixel equality and identical telemetry and timestamps against the native reader for every
frame of the canonical MKV.

### 2. Frame selection and pose priors   [E] under 1 min
Port extract_frames.py's multi-pass flow: metadata scan, hot spot location, view selection,
selected-frame temperature statistics, drift rejection, then CLAHE 8-bit images and raw frames
for the retained frames. Decide between repeated decoding and caching candidate frames in OPFS
and count both in the estimates. Inputs the native tool takes on the command line (target
latitude/longitude, fallback altitude, rotation, HFOV) come from the proposal UI. Accept: the
same frames, hot spot within 1 m, poses identical to 1e-6, CLAHE images pixel-identical (or a
documented, justified tolerance).

### 3. Features   [E] about 30 s laptop / 10 s win11
COLMAP's SIFT via the colmap-wasm fork in a worker pool, 8192 features max. VLFeat CPU SIFT
versus the native GPU SIFT is an equivalence question, and the native GPU SIFT is itself not
deterministic (different keypoints on 50 of 201 images between runs, sometimes more than
max_num_features), so equivalence is statistical and judged downstream: keypoint and
descriptor statistics, verified geometry, and the mapped model against the G2 tolerances.

### 4. Pairs, matching, verification   [E] about 1-1.5 min / 30 s
Port sfm.py's telemetry pair selection exactly. Port sfm.py's matching rule to WebGPU exactly:
normalised descriptors, forward L2 ratio test, reverse nearest-neighbour identity (ties, empty
descriptors, numerical tolerance), descriptors uploaded in batches. Verify with the fork's
separate two-view verification in workers. Accept: pair list identical; with identical
descriptors, matches identical to the native matcher within float tolerance; verified pairs and
inliers at least 90% of the native database's with the browser features.

### 5. Mapping   [E] about 20-60 s
G2's prior mapper run on stage 4's output, with progress per round. Accept as G2, from the
browser's own features and matches.

### 5b. Scene preparation
What train.py does before training: reject weak or misregistered views, hot/cold selection and
the frozen holdouts, undistort the radiometric images with the refined camera (or validate
Brush's distorted-camera path against undistortion), compute the global temperature scale,
filter sparse points spatially, build the canopy ceiling, initialise point values from thermal
observations (not CLAHE colours), re-locate the hot spot with the refined poses, and set the
scene-relative learning rates, densification and pruning. Accept: the same training set, scale
and initial points as train.py's scene loading for the same model.

### 6. Training   [M] Brush stock: 17.5 min win11 Chrome, 19.2 min laptop native; thermal
fork to be measured
G1's trainer run from stage 5b's dataset, with checkpoints and a capped preview. Accept as G1,
end to end, on both machines; full run on the 8 GB laptop GPU.

### 7a. Export/viewer round trip (early)
With a tiny known model: PLY with the `apcg_splat` header, SPZ plus meta.json for the thermal
scale, the viewer embedding. Check SH order and direction, quaternion layout, opacity and log
scale encoding, axes, units, temperature metadata, start camera.

### 7. Result
Export the trained splat (PLY, SPZ + meta.json, COLMAP model) and show it. Evaluate the
training output, the exported PLY and the SPZ separately (lobe projection and SPZ quantisation
add error). Accept: the downloaded PLY opens with temperatures at plot.ardupilot.org/splat/.

### 8. Integration, recovery, release
Full runs from the MKV on both machines with timings in PROGRESS.md and calibrated estimates.
Tested: stop/resume at every stage, reload during checkpoint writes, quota exhaustion, stale
versions, two tabs, `device.lost`, allocation failure, missing telemetry. A short README.
Deploy at plot.ardupilot.org/websplat/ (decided 2026-11-09, replacing uav.tridgell.net/Splats/make/):
the same origin as /video/ and /splat/, so a later version can start from a recording /video/
has already downloaded into browser storage (see Future versions).

## Future versions: starting a splat from the video viewer

Recorded 2026-11-09; not part of release 1, which ships first as the standalone page.

plot.ardupilot.org/video/ already holds the recording and telemetry, the flight path in a 3D
map with terrain, ray/terrain picking and video projection. It is the natural place to choose
what to splat. The viewer selects; WebSplat computes.

- **Create splat** in /video/ enters a selection mode:
  1. Time range: two handles on the timeline. The 3D map shows only that part of the flight
     (the rest dimmed) and the ground points the camera looked at. "Snap to orbit" proposes
     GUIDED or loiter segments, or segments where the camera rays converge.
  2. Coverage: a ring around the target showing which bearings the range covers and how well
     the gimbal held the target; warnings for a partial orbit or a frame-count cap; frame count
     and an estimated time before starting.
  3. Target: click in 3D (terrain intersection), with the hot-spot estimate offered; a vertical
     handle adjusts the height. This replaces the latitude/longitude confirmation form.
  4. View: the 3D camera pose relative to the target becomes the splat's start camera and up
     axis, so the default view is chosen at creation.
  5. Go: opens WebSplat in a new tab with a job description. Kept as a separate page so video
     review does not load tens of MB of wasm, and review can continue during a 10-15 minute
     job. The WebSplat tab must stay visible (hidden tabs are throttled).
- WebSplat accepts a job description: source path in browser storage (same origin, so no
  second download), time range, target, view, camera settings; the confirmation step is skipped
  when they are given. Ingest and selection process only the range, not the whole recording.
- Later: show the finished splat in the viewer's 3D map, georeferenced from the splat origin;
  RGB `.ts` recordings as sources.

Phasing: v2 = range, target and height, view capture and Go in /video/, plus job description
and range-only ingest in WebSplat. v3 = coverage ring and snap to orbit, splat in the 3D map,
RGB sources.

## Risks

- Thermal training semantics in Brush (G1) and the colmap-wasm fork (G2) are the two big
  unknowns; both are gated before integration.
- GPU memory: no portable free-memory query; budgets, batching and recovery as above. Brush's
  demo failed to allocate on the laptop with a 1600x1000 canvas but not at 1280x720 (PROGRESS.md).
- Toolchains: emsdk installation was blocked in this session; Rust wasm target and wasm-pack
  for Brush may be too.
- WebGPU on Linux Chrome needs flags and is fragile; test on both machines from stage 0.
- Reference uncertainty: the orbit's extract_frames.py options were not recorded (stage 0).
- Timings in the research report are partly estimates (colmap-wasm was not run); carry the
  labels.
