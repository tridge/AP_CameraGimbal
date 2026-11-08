# Stage 3 (features) and the pipeline integration of stages 3-5

2026-11-08. **The whole chain runs in the page and passes: ingest → select → features →
match + verify → map.** Runs on the laptop and on the win11 machine produce byte-identical
verified geometry and models, and so does a run that was stopped inside mapping and resumed.
The model meets G2's tolerances against the native reference, and the verified pairs and
inliers are above 90% of the native database's. That closes G2's pending criterion, now with
features extracted in the browser.

## Stage 3: features

- Fork (patch 0004): `ws_sift` runs COLMAP 4.2.1's SIFT (VLFeat on the CPU) with
  `FeatureExtractionOptions()` defaults and `max_num_features` 8192 on an 8-bit grey image in
  memory. The output is keypoints n x 6 and descriptors n x 128 uint8 (L1-root, as COLMAP stores
  them).
- `app/src/stages/features/`:
  - `format.js`: WSFT encode/decode, moved here from `map/features.js`, which now re-exports it.
  - `sift.js`: `siftImage()`.
  - `sift.worker.js`: one fork instance per worker.
  - `pool.js`: `siftInPool()`. One image per message; any failure or Stop terminates every
    worker. It uses `map/pool.js`'s `call()`, now exported.
  - `features.worker.js`: the stage worker. It reads select's `frames.json` and `features.u8`,
    runs SIFT in a nested worker pool, and streams `features.bin` (WSFT) to OPFS without
    concatenating it. It also writes `features.json`.
  - `stage.js`: the stage definition.
- Equivalence:
  - The native reference used GPU SIFT. `sfm.py` sets `use_gpu`, but pycolmap's `device=auto`
    overrides it, so SiftGPU runs whenever there is a GPU. GPU SIFT is not deterministic.
  - The like-for-like check is therefore native COLMAP CPU SIFT (`test/stage3/native_sift_cpu.py`,
    pycolmap 3.13 with `device=cpu`) on the same CLAHE images. Against it the fork has:
    - identical keypoint counts in all 201 images (1501042 in total);
    - every keypoint at the same place (within 0.01 px, same scale and orientation);
    - descriptors identical in 99.97% of keypoints, and at most 1 unit apart otherwise.
  - Against the reference database's GPU SIFT the result is statistical, and identical for the
    fork and native CPU SIFT:
    - 1.9% more keypoints;
    - 69% of GPU keypoints within 0.5 px;
    - median scale 1.367 vs 1.378 px.
- `test/stage3/run_tests.sh`, in order:
  1. `test_pool.mjs`: pool failure paths with fake workers. A worker that fails at startup
     while the pool awaits an image read must reject, terminate every worker and remove its
     listeners; a throwing `onImage` and Stop are covered too.
  2. Native CPU SIFT.
  3. Fork SIFT in Node.
  4. `compare_features.py --mode exact --selftest`:
     - equal counts per image;
     - row k corresponds to row k;
     - positions within 1e-3 px and every affine component within 1e-4;
     - descriptor bytes within 1, and at most 1e-3 of rows differing (measured: 0 px, 9.5e-7,
       517 rows = 3.4e-4).
     The self-test requires four corrupted copies (reordered rows, an extra keypoint,
     orientations rotated 90 deg, descriptors +2) to fail.
  5. `--mode stats` against GPU SIFT: report only.
- The browser stop test (`g2_page.js mode=stop`) also checks that a SIFT pool of nested workers
  that fail to load rejects instead of hanging.

## The browser chain against the reference

These numbers come from `test/harness/thermal.mjs --reference`: browser features, the stage 4
WebGPU matcher, fork verification, then prior mapping.

| | browser chain | native fresh run (GPU SIFT, notes/stage0.md) | tolerance |
|---|---:|---:|---:|
| verified pairs / inliers | 2858 / 426211 (106% / 110% of 2702 / 388488) | | >= 90% |
| position median / p95 | 0.250 / 0.510 m | 0.150 / 0.390 m | 0.3 / 1 m |
| rotation median / p95 | 0.081 / 0.162 deg | 0.062 / 0.132 deg | 0.1 / 0.3 deg |
| points, reprojection | 40806, 0.498 px | 40408, 0.508 px | within 10% |

Observations per image, track lengths and triangulation-angle medians are all within the 10%
checks. The margin is smaller than with native features (0.25 m against the 0.3 m median
tolerance) because CPU SIFT finds different keypoints from GPU SIFT.

That is confirmed natively: `test/stage3/native_cpu_sfm.py` runs `sfm.py --mapper prior` on a
database built like `sfm.py`'s but with pycolmap CPU SIFT. Its model is 0.259 / 0.506 m and
0.088 / 0.201 deg from the GPU reference, with 2859 pairs / 426275 inliers and 40816 points. So
the native pipeline on CPU SIFT sits just as close to the margin. Against that native CPU-SIFT
model the browser chain is 0.084 / 0.251 m and 0.033 / 0.108 deg (2858 / 426211, 40806
points), which is the verification-RANSAC floor of G2. The thin margin comes from CPU vs GPU
SIFT, not from the browser implementation. The reference stays the GPU-SIFT model.

## Pipeline integration

Stages registered in `stages/index.js` 'thermal', in order: ingest, select, features, match,
map. Each runs one stage worker (`workers/runtime.js` protocol), and pools are nested workers
inside it. Terminating the stage worker (Stop: the host terminates after 3 s) also terminates
its nested workers.

- **features** (unit images). Inputs: select. Key: `config.features` (`maxNumFeatures`).
  Pool size: `config.cpuWorkers` (default min(8, cores - 1); not part of the key).
- **match** (unit pairs). New files `match/stage.js` and `match/match.stage.worker.js`; stage 4's
  committed files are unchanged. Inputs: select, features. Key: `config.match` (ratio, pair
  options, seed, legacy, powerPreference). Steps:
  1. `selectPairs` on `frames.json` (ties logged).
  2. WebGPU `matchPairs` on the WSFT descriptors.
  3. `verifyInPool` with nested `map/map.worker.js` instances and the frames.json camera.

  Writes `pairs.json`, `matches.bin`, `matches.json`, `geometry.bin` and `geometry.json`.
  Progress: matching counts the first half of 2 x pairs, verification the second half.
- **map** (unit `BA rounds`, fractional progress within a round). New files
  `map/map.stage.worker.js` and `map/stage.js`. Inputs: select, features, match. Key:
  `config.map` (thresholds, priorSigma, maxIterations). Runs the fork in the stage worker and
  writes `final/{cameras,images,points3D,rigs,frames}.bin` and `model.json`.
  - Checkpoints: one after each of the first three rounds, `ckpt-<g>/state.json` holding
    poses, camera parameters and COLMAP's PRNG state. On resume the stage restarts after the
    last published round.
  - Exact resume needed two fork changes, now in place:
    - Every round after the first starts from a model rebuilt from its own poses and camera.
      Point-id counters and the hash-table history otherwise carried over and changed the result
      (0.1 m between resumed and uninterrupted runs).
    - The PRNG state is saved with the checkpoint and restored.

    `test/g2/test_resume.mjs` resumes from each of the three round checkpoints (after a
    JSON round trip) and asserts that all five model binaries are byte-identical. The rebuild
    changes the uninterrupted model slightly too. From the native geometry it is now 0.040 m /
    0.114 m position and 0.013° / 0.029° rotation from the reference (G2 run before:
    0.020 m / 0.078 m), still well inside tolerance.
  - Stop: one round is a single synchronous wasm call. Stop therefore takes effect through the
    host's terminate after 3 s; the harness measured 3.1 s to `paused`.
  - Progress: the stage list shows the round plus "bundle adjustment iteration d of at most t"
    or "triangulating image d of t" (`ui/stage-list.js`). The estimate is `uncertain`, since BA
    may stop before its iteration limit.
  - Not tested: a page reload while a mapping checkpoint (`state.json`, about 20 kB) is being
    written. After a reload the harness would have to re-select the MKV. The dummy-stage
    harness covers reload during checkpoint writes for the controller; a real-stage case is
    stage 8 work.
- Harness `test/harness/thermal.mjs` (extended):
  - prints a progress line every 15 s;
  - waits until every stage is done or one has failed (it used to wait forever on a stage left
    'waiting');
  - copies `features.json`, the match outputs and the map model out of OPFS;
  - checks verified pairs and inliers against the native database (>= 90%) and the model with
    `test/g2/compare_models.py`;
  - `--stop-map` stops inside round 1, checks the pause and the checkpoint, starts again and
    requires `resumedAfter === 0`;
  - the progress timer is cleared in `finally`, and the process exits with the run's code, so
    errors and timeouts end the run non-zero.

## Measurements (canonical orbit, `--reference`)

| stage | laptop (RTX PRO 1000, 8 workers) | win11 (RTX 4070, 8 workers) |
|---|---:|---:|
| ingest | 0.7-0.9 s | 1.2 s |
| select | 4.0-4.4 s | 6.1 s |
| features (SIFT 201 images) | 23.8-27.6 s | 21.6 s |
| match: WebGPU matching | 43.5 s | 12.5 s |
| match: verification | 55.4 s | 37.0 s |
| map (4 rounds) | 74-83 s | 79 s |
| whole pipeline, wall | 207-210 s | 175 s |

- SIFT takes about 0.7 s per image per worker in wasm. Native CPU SIFT on 8 threads takes 18 s
  for all 201 images; native GPU SIFT takes 3-4 s.
- The laptop's matching (43.5 s) was slower than stage 4's 27.5 s measurement. A G1 Brush
  training run shared the laptop GPU during these runs.
- Peaks:
  - wasm heap: 150 MB per SIFT worker, 35 MB per verification worker, 375 MB for mapping;
  - whole-GPU peak on win11: 3.3 GB (1.7 GB above start; shared with other work).
- wasm SIMD (`-msimd128`): outputs are identical to the scalar build for SIFT, verification
  and mapping (bytes compared). Speed is the same for SIFT and 3-4% faster for verification and
  mapping; Eigen 3.4 has no wasm SIMD packet math, so this is only clang's autovectorisation.
  It is kept: `build-colmap.sh` builds with `WASM_SIMD=1` by default (patch 0003 adds the build
  option). `websplat-colmap.wasm` is 6.89 MB (2.06 MB gzip).

## Proposals for CONTRACTS.md

- Section 3: `features.json` as written: `{format: websplat.features, version 1, extractor,
  max_num_features, options, images, keypoints, counts}`. Note that native SIFT was GPU SIFT
  (`device=auto`), and that the browser's CPU SIFT equals native CPU SIFT.
- Section 4: `pairs.json` carries `ambiguous`. `matches.json` and `geometry.json` record as
  written above: ratio, rule, fp16, adapter; verifier, seed, `per_pair_seed`, camera.
- Section 5: `model/` is the map stage's `gen-<g>/` with `final/` and `model.json`; no
  `telemetry/` model is written (it is reproducible from frames.json). Checkpoint `ckpt-<g>/state.json`
  is `{round, rounds, poses (7 per image, cam_from_world), params, prng}`.
- Stage interface: pools as nested workers in the stage worker; `config.cpuWorkers` as a
  non-key option for pool sizes.

## Open

- Stop inside a mapping round or a SIFT/verification call waits for the 3 s terminate (no
  SharedArrayBuffer).
- The laptop's verification pool (8 workers: 55 s) scales worse than win11's; laptop cores
  are shared with other work.
- Matching and verification could overlap: verification could start on matched batches.
