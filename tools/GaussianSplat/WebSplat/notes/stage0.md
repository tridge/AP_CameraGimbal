# Stage 0: reference, contracts, skeleton, harness

2026-11-08. Reference record with full numbers and paths: `$WEBSPLAT_DATA/ref/REFERENCE.md`.
Contracts: `CONTRACTS.md` (v1.1). App: `app/`. Harness: `test/harness/`. Metrics script:
`test/stage0/metrics_table.py`.

## 1. Frozen reference

All native outputs below were made on the desktop (RTX 4070, WSL2) with the tools venv
(pycolmap 3.13.0, torch 2.14.1+cu130, gsplat 1.5.3, OpenCV 5.0.0, PyAV 19.0.2, numpy 2.5.4).

### Extraction — reproduced bit-exact

    extract_frames.py ORBIT.mkv WORK --lat -35.5802022 --lon 148.9065438 --rotation 0

All other options default. Recovered from the transcript of the session that made `ref/work`;
consistent with extract.log (hot spot at ENU 9.8, 32.4 m from the given position). Proof:

- desktop: `frames.json` byte-identical, all 201 `images/` and `thermal/` PNGs identical.
- laptop (same library versions): PNGs identical; `frames.json` identical apart from `source`
  and `target_enu[2]` (45.42321477101038 vs ...0485, a last-digits CPU/BLAS difference in the ray
  triangulation); per-frame poses identical.

### Mapping — sfm.py --mapper prior

Command `sfm.py WORK --mapper prior` with defaults (SIMPLE_RADIAL single camera, 8192 SIFT,
pairs 25 deg / 2.2 range ratio / 30 neighbours / 5 sequential = 3175 pairs, ratio 0.8, prior
sigma 5 m stored as CARTESIAN covariance 25 I, thresholds 40,16,8,4; per-round options in
REFERENCE.md). Reference model: 201 images, 37959 points, 185255 observations, track 4.88,
reprojection 0.517 px, f 1503.948, k1 -0.0972.

- Mapping from a copy of the reference database reproduces it exactly (camera centres within
  3.5e-12 m, rotations 2e-6 deg, identical counts per round): **the prior mapper is
  deterministic given the database**, so G2 can be compared on identical inputs.
- A full fresh `sfm.py` run does not reproduce it: native **GPU SIFT is not deterministic** (50
  of 201 images got different keypoints; the database holds up to 10099 keypoints per image
  despite max 8192). That run's model differs from the reference by camera position median
  0.15 m / p95 0.39 m / max 2.07 m and rotation median 0.062 deg / p95 0.132 deg, 40408 points.
  This is the native run-to-run floor under G2's tolerances (0.3 m / 0.1 deg medians).

### Training — native SH-only baseline

    train.py WORK --sparse WORK/colmap/sparse/final --out baseline_sh

(run13 without `--lobe --source-gain --gain-scope scene`; every other run13 option is a
default.) 418 s on the desktop, 549219 Gaussians, 1.0 GB GPU. Repeated once (`baseline_sh_rep2`,
547172 Gaussians) to measure run-to-run spread. Both in `$WEBSPLAT_DATA/ref/`.

Frozen holdouts (same in run13 and both baselines): cold `00015 00035 00066 00086 00115 00135
00167 00187`, hot `00009 00050 00096 00140 00149` (41 hot frames above 32.60 C; scale log1p
t0 10.9068, t1 126.4231, k 8).

| run | split | PSNR mean dB | abs peak err median / mean / max C | hot IoU mean | false hot views |
|---|---|---|---|---|---|
| baseline_sh | hot | 37.88 | 12.53 / 17.04 / 33.78 | 0.286 | - |
| baseline_sh | cold | 41.07 | 0.76 / 0.79 / 1.78 | - | 0 |
| baseline_sh_rep2 | hot | 37.83 | 12.25 / 16.17 / 33.43 | 0.233 | - |
| baseline_sh_rep2 | cold | 40.72 | 0.61 / 0.62 / 1.31 | - | 0 |
| run13 | hot | 42.96 | 5.73 / 12.55 / 27.79 | 0.261 | - |
| run13 | cold | 44.53 | 0.40 / 0.54 / 1.22 | - | 0 |

Per view (baseline_sh; rep2 in REFERENCE.md and `$WEBSPLAT_DATA/ref/metrics_tables.md`):

| hot view | 00009 | 00050 | 00096 | 00140 | 00149 |
|---|---|---|---|---|---|
| PSNR dB | 41.77 | 32.02 | 37.54 | 40.30 | 37.77 |
| peak err C | -4.43 | -33.78 | -25.33 | -12.53 | +9.11 |
| IoU | 0.000 | 0.000 | 0.000 | 0.714 | 0.714 |

| cold view | 00015 | 00035 | 00066 | 00086 | 00115 | 00135 | 00167 | 00187 |
|---|---|---|---|---|---|---|---|---|
| PSNR dB | 40.96 | 38.17 | 43.26 | 35.36 | 43.21 | 41.48 | 41.02 | 45.08 |
| peak err C | +0.20 | +0.77 | +0.01 | -0.75 | +1.05 | -1.78 | +0.28 | -1.49 |

Hot-region overlap is 0 for three of five hot views in every run (the source is a few pixels;
1-2 px pose error misses it). Pixel-level false hot detections are not in train.py's output;
G1's `metrics.py` renders them.

**Baseline run-to-run spread (two identical runs)**: hot PSNR per view within 0.25 dB, but cold
PSNR per view up to 2.3 dB (00035), absolute hot peak error per view up to 3.7 C (00149: +9.11 vs
-5.46 signed), hot IoU per view up to 0.21 (00149). Several of G1's per-view tolerances (cold
PSNR -2.0 dB per view, IoU -0.15 per view) are inside this noise: see proposals.

## 2. Contracts

`CONTRACTS.md` v1.1: conventions (byte order, frame identity, ENU datum, both transform
directions named, COLMAP pixel convention, temperatures, hashes), source identity, ingest index
and `proposal.json`, frames (native frames.json schema plus `selection`), features (`WSFT`),
pairs, matches and verified geometry (stage 4's `WSMT` container for both), reconstruction
(COLMAP binary model), training dataset (splits, scale, undistortion, ceiling, initial points,
float16 targets), splat checkpoint and PLY, the job manifest (stage keys, invalidation,
generations, alternating manifest slots, states, single tab, persistence) and the stage module
interface. Folded in from stages 1-2 and 4: `ctx.sourceFile`, the vendored FFmpeg core,
imports from `tools/viewer/src`, `proposal.json`, `selection.mode`, `selection.considered`,
`track.codecPrivate`, verified geometry in the matches container.

## 3. App skeleton (`app/`)

vite 8.3.6 static build (`npm ci && npm run build` → `dist/`), prettier 3.6.2 with the viewer's
config, `@ffmpeg/core` pinned to the viewer's 0.12.10 and copied to `vendor/ffmpeg/0.12.10/`.

- `pipeline/registry.js` (stage definitions, stage sets), `pipeline/controller.js` (ordered run,
  stage keys and invalidation, resume, Stop → paused, reload → interrupted, checkpoint
  publication, `ready()` gate for user input, snapshot for UI and harness),
  `pipeline/eta.js` (per-stage clock with phases; work-only throughput over a sliding window;
  prior blended out as work completes; `cumCost` for non-linear work such as training slowing
  with Gaussians; future checkpoints at their measured duration; overall = current + waiting
  priors scaled by completed stages' actual/prior ratio; range narrowing with completed stages;
  "about N min" rounding).
- `storage/` OPFS helpers, `sha256`/`sha256c` hashing, manifest in two alternating slots with
  checksum and seq (no rename needed, safe against reload mid-write), job store with
  `gen-<g>`/`ckpt-<g>` directories published only after their files are written and hashed,
  unpublished generations removed at startup, Web Locks single-tab lock, persistent storage
  request (denial logged and shown).
- `workers/host.js`, `workers/runtime.js`: module worker protocol (progress, phase, log,
  checkpoint with acknowledgement after publication, stop then terminate).
- `device/check.js`: WebGPU presence, adapter info and limits, GPU class, a real smoke test
  (16 MiB storage buffer compute, rgba32float storage texture, 1600x1000 rgba16float render
  target, readbacks, error scopes, device loss) and the Linux Vulkan loop zero-initialisation
  bug as a warning; clear errors with the Chrome-on-Linux flag hint; `?adapter=low-power`.
- Page: device panel, recording chooser (hash names the job, optional OPFS copy, re-selection
  verified by size and hash), proposal form (from `proposal.json`) gating selection, stage
  list (state, phase, natural unit, Gaussians, elapsed, remaining), overall ETA, Start/Resume,
  Stop, Reset, storage status, hidden-tab warning, Screen Wake Lock.
- Stage sets: `thermal` (stages 1-2 registered: ingest, select) and `dummy` (8 fake stages
  in a module worker with the canonical input's counts and times: bytes, frames, pairs, BA
  rounds, iterations with Gaussian growth and checkpoints every 5000, a GPU buffer sized like
  the Gaussians; `?speed=N`).

## 4. Harness (`test/harness/`)

    node test/harness/dummy.mjs --machine laptop [--adapter low-power] [--speed 30]
    test/harness/win11/win11.sh start      # Chrome on win11 + ssh -L 9345 / -R 8795
    node test/harness/dummy.mjs --machine win11 --port 8795 --cdp http://127.0.0.1:9345
    test/harness/win11/win11.sh stop
    node test/harness/thermal.mjs --machine laptop|win11 [...] [--reference]

The laptop runs its own headless Chrome (flags in `test/harness/lib/browser.mjs`, profile under
`$WEBSPLAT_DATA/harness/profiles/`); the win11 machine runs Chrome 156 with
`--force_high_performance_gpu` and its own profile, driven over CDP. Reports
(checks, per-stage timings and phases, ETA rows, GPU samples, every console message, HTTP
errors) go to `$WEBSPLAT_DATA/harness/runs/`. GPU memory: nvidia-smi whole-GPU samples and, on
Linux, the test Chrome's own GPU process (other agents share the GPUs, so whole-GPU numbers are
contaminated; Windows reports no per-process memory).

Linux Chrome uses ANGLE on Vulkan with no surface (`--use-angle=vulkan --use-vulkan=native
--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan --disable-vulkan-surface
--ignore-gpu-blocklist --enable-unsafe-webgpu`), which gives the hardware adapter on a headless
GPU server as well as the laptop. `launchLocal` opens `chrome://gpu` for 3 s first: until the GPU
process is up `requestAdapter()` can return null or SwiftShader. On the laptop both GPUs still
work (`--adapter low-power` gives the Intel one); WebGL moves from Intel GL to NVIDIA Vulkan.
Running as root needs `extraArgs: ['--no-sandbox']`.

### Acceptance (dummy pipeline, speed 30): 22/22 checks on both machines

Checks: device check and smoke test; reload mid-stage keeps completed stages (same generations)
and marks the stage interrupted, Resume restarts it; Stop pauses; a second tab is refused while
the first keeps running; reload during a checkpoint write (after one was published) keeps the
last published checkpoint, removes the partial generation and training resumes from it;
completion with every published output matching its manifest hash; progress monotonic within
totals and in the right units; all of waiting, running, checkpointing, paused, interrupted,
done observed; stage ETA within 25% from half way; overall ETA within 30% in the last run; no
page/console errors; no Dawn allocation or validation warnings.

| run | wall | training stage ETA error at 25/50/75% | overall ETA (predicted / actual s) | GPU |
|---|---|---|---|---|
| laptop NVIDIA | 66 s | -0.1 / -0.7 / -1.0% | 42/43, 31/36, 21/22, 7/7 | test process 267 MiB peak |
| laptop Intel (`low-power`) | 65 s | -1.2 / -1.4 / -0.9% | 42/43, 32/36, 21/21, 7/7 | Intel, not measured by nvidia-smi |
| win11 RTX 4070 | 44 s | +0.1 / -1.1 / -2.7% | 28/29, 21/24, 14/15, 6/5 | whole GPU only |

At speed 5 on the laptop (6 min, 22/22): every stage's ETA error from half way was within 18%
(worst: features and select at 75%, -17/-18%, a few seconds absolute), training within 0.4%.

Device checks: laptop NVIDIA (`blackwell`, class laptop) and Intel (`xe-lpg`) pass the smoke test
but **both report the loop zero-initialisation bug** (12 instead of 6); win11 (`lovelace`, class
desktop) is correct. Chrome on Linux and Windows leaves `adapter.info.description` empty, so the
GPU class comes from vendor + architecture.

Real stages through the page (`thermal.mjs`): ingest + select on the canonical MKV, proposal
shown and confirmed, 201 frames; with the reference inputs poses identical to `ref/work`, with
the proposal within 1.9e-4 m. Laptop: ingest 0.8 s, select 4.1-4.2 s; win11: ingest 1.1 s,
select 5.6-6.0 s (the MKV crosses the ssh tunnel first).

## Decisions

- The reference extraction command is the recovered one above; it is exact.
- Native-vs-native floors are recorded (SIFT nondeterminism, training spread) rather than
  assuming exact native repeatability.
- Manifest durability by alternating checksummed slots instead of rename.
- Checkpoints are separate `ckpt-<g>` directories; a stage's final output is `gen-<g>`.
- Autostart (`?autostart=1`) starts only a fresh job; an interrupted job waits for Resume.
- The baseline was trained with the desktop's train.py (identical training code; its PLY lacks
  the `apcg_splat` header).

## Proposed changes to PLAN.md / PROGRESS.md (coordinator)

- PROGRESS.md "Shared data": the extraction command is now known (above); add
  `ref/baseline_sh`, `ref/baseline_sh_rep2`, `ref/reproduce`, `ref/REFERENCE.md`,
  `ref/metrics_tables.md`.
- G1 tolerances (notes/g1.md): the per-view criteria should allow for the baseline's own spread
  measured here (cold PSNR per view 2.3 dB, hot IoU per view 0.21, abs hot peak error per view
  3.7 C): e.g. compare per view against the worse of the two baselines, or drop per-view
  bounds on cold PSNR and IoU in favour of the means.
- G2 / stage 5: compare against the reference model from the same database (deterministic);
  note that a fresh native run already differs by 0.15 m / 0.06 deg median.
- Stage 3: native GPU SIFT returns more than `max_num_features` (up to 10099) and is not
  deterministic; feature equivalence must be statistical / downstream.
- WGSL authors: the Linux Vulkan loop zero-initialisation bug is detected by the device check;
  always initialise loop-local `var`s.

## Open issues / not done

- Device check does not reproduce Brush's "requested allocation size is smaller than the image
  requires" condition (its exact texture formats/sizes are not known here); the harness flags
  such console warnings if they appear. Add Brush's texture shapes to the smoke test once G1
  identifies them.
- Not tested yet (stage 8): quota exhaustion, `device.lost` during a stage, stale app versions
  across a deploy, source re-selection path in the harness (the copy path is tested).
- The ETA model is checked only against the dummy stages' synthetic timing; real calibration
  needs stage measurements. Per-GPU-class priors live in each stage's `estimate()`.
- Windows nvidia-smi gives no per-process memory: win11 GPU numbers are whole-GPU and shared
  with other agents' work.

## Review round 1 fixes (REVIEW-stage0.md)

1. Manifest saves hash and write one immutable snapshot; `seq` advances only after the write.
2. `loadManifest` distinguishes no job (no manifest, empty directory) from damaged, unsupported
   (other `version`) and data-without-manifest jobs; those raise `ManifestError`, nothing is
   deleted, and the page offers an explicit "Delete this job". Harness: both slots rewritten as
   version 2 → refused with data kept; restoring them reopens the finished job.
3. Outputs, generation, `state: done`, result and timings are one manifest transaction
   (`Job.publishOutputs`); old generations and checkpoints are removed afterwards.
4. Each stage attempt has a run number; checkpoint handlers of a stale or stopped attempt are
   refused (checked again just before the manifest write); `runWorker` settles only after its
   pending publications. The page snapshot reports only checkpoints whose manifest write
   completed (`Job.durable`), so nothing is shown as published before it is durable.
5. Reset awaits `controller.shutdown()` (run loop, publications, manifest writes) before
   deleting the job and releasing the lock. Harness: reset during a run leaves a clean new job.
6. Every manifest write in a stage attempt is inside its error handling; periodic saves are
   observed (a failure, e.g. quota, aborts the run and fails the stage with `error.quota`);
   `Job.transact` rolls the in-memory record back on a failed write; unpublished generation
   directories are deleted before every attempt and after a failure (also the stage 1-2 request).
   Not exercised by the harness (no quota injection yet).
7. CONTRACTS.md v1.2: verified geometry is the single `WSTV` container (pair table, inliers,
   f64 models at a 4-byte offset) as `format.js` and `export_native.py` implement.
8. CONTRACTS.md: training dataset steps with defaults (cold budget `max_images - hot`, `v_hot`,
   `extent_m`, learning rates, filters, bounds, ceiling, initial points, float16 rounding), one
   checkpoint layout (`ckpt-<g>`), canonical JSON for all hashes, stage key and `outputsHash`.
9. Phase labels are separate from their kind: `phase(name, kind)`; `compile`, `load`,
   `startup`, `eval`, `checkpoint` default to overhead, any other label is work, so ingest's
   `index`/`hash`/`propose` and select's `select`/`output` count as throughput. Select: its
   re-index and source check are now `load`, and its progress is one monotonic count over both
   passes (minimal edit to `select.worker.js`). Timings record `workMs`.
10. Unknown-total work counts down its prior, then reports unknown (never zero); overall with an
    unknown current stage is shown as "more than …"; `uncertain` is carried in the snapshot.
11. Harness: required ETA coverage (every dummy stage with a prior of at least 3 s at the run's
    speed must have samples at 25/50/75%; none sampled fails), at least 3 overall samples, and
    the app's own manifest validator plus re-hash of every published file (`websplat.verify`).
12. `thermal.mjs` requires frames.json, features.u8 and thermal.u16 and compares frame
    identities, image size/HFOV/rotation, positions, rotations, radiometry and timestamps, and
    per-frame SHA-256 of thermal and CLAHE pixels against the native PNGs
    (`test/stage0/ref_hashes.py` → `$WEBSPLAT_DATA/ref/work_pixel_hashes.json`).
13. Worker construction and the start `postMessage` are inside the cleanup path.
14. `npm run dev` prepares the FFmpeg assets first.

The four stage-4 files already passed `prettier --check` when this round started; nothing to do.

## Review round 2 fixes

1. `Job.transact` runs in the manifest write queue on a copy of the manifest; the live record
   takes the result (same object) only after the write succeeded. No earlier save can publish an
   uncommitted change and a failed write changes nothing. Harness: a checkpoint transaction with
   an injected `QuotaExceededError` amid queued saves leaves checkpoint 1 live, on disk and its
   files intact, checkpoint 2 cleaned up.
2. A slot of another manifest version refuses the job even next to a valid v1 slot. Harness:
   newest slot rewritten as checksummed v2 → refused, data kept, restore reopens.
3. Completion becomes visible only with its write (follows from 1). `dummy.mjs` waits for the
   controller to stop and checks every stage is `done` in the validated manifest.
4. `select.worker.js` progress: single mode is one pass (201/201); redecode and cache two
   (402/402). The manifest keeps the stage's own last count (`progress.reported`); `thermal.mjs`
   checks it equals the total, and passes in all three modes (`--select-mode`).
5. CONTRACTS.md v1.3 documents native train.py's projection fallback for points without
   observations in the chosen images (kept, not a deviation).

Harness: dummy 30/30 on laptop NVIDIA, laptop Intel and win11; thermal on laptop (three modes)
and win11 all pass.
