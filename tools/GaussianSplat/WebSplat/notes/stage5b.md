# Stage 5b: scene preparation

2026-11-08. **The browser stage reproduces train.py's scene loading bit for bit** on the native
final model and on the browser's own model, in Node and in the page on the laptop and win11:
same views, holdouts and hot set, identical undistorted Celsius images and float16 targets,
identical temperature scale, bounds, canopy ceiling and initial points, the same learning rates,
schedule and densification/pruning settings, and the hot spot within 2e-6 m.

## What it does

`app/src/stages/prepare/`, the stage after map in the 'thermal' set (unit images, inputs select and
map, config key `prepare` = train.py option values under train.py's names):

- `colmap-model.js`: COLMAP binary reader (cameras, images with Eigen's quaternion matrix and
  projection centre, points3D in file order).
- `opencv.js`: the OpenCV 5.0 calls train.py makes, ported exactly:
  - `initUndistortRectifyMap` (k1 only, newK = K);
  - `remap` INTER_LINEAR + BORDER_REFLECT on float32. OpenCV 5's SIMD path interpolates with the
    fractional coordinate and FMA. It does not use OpenCV 4's 1/32-pixel tables: that variant
    differs at 326k pixels of one frame, plain mul+add at 718. FMA is emulated in float64 (the
    product of two float32 values is exact);
  - `undistortPoints` (5 iterations, P = K).
- `scene.js`: `prepareScene(io, options)`, the steps of CONTRACTS.md 5b in train.py's order and
  NumPy 2's arithmetic:
  - float32 with weak Python scalars, including `np.percentile`'s float32 lerp for `t0`;
  - `log1p` as `fround(Math.log1p)`, which equals NumPy's float32 log1p on this CPU (no AVX512).
  - Two passes over the chosen images: the scale's sample and maximum first, then targets, point
    values and hot-spot pixels. Celsius images are never all held: peak memory is one image plus
    the 1M-value sample.
  - It also writes the `training` block: train.py's Adam rates, means schedule, `DefaultStrategy`
    fields and `scene_scale`.
  - Undistortion is done as train.py does it. Brush's distorted-camera path is not used, so it was
    not validated.
- `prepare.worker.js`: reads select's `frames.json` and `thermal.u16` and map's `final/`. It
  streams `targets.f16` to OPFS one image at a time, then writes `points.f32`, `ceiling.f32` and
  `dataset.json`. Stop is checked per image; there are no checkpoints, since the stage is
  seconds long.

## Tests (`test/stage5b/run_tests.sh`, Node, exits nonzero on any failure)

- `dump_scene.py WORKDIR SPARSE OUT [--options scale.json] [--train-arg=...]` runs train.py's
  own `main()` up to the strategy set-up. No training is done. It monkeypatches `load_scene`
  (CPU), `cv2.remap`, `Adam`, `ExponentialLR` and `DefaultStrategy` to capture the values, and
  re-locates the hot spot the way main() does after training. Nothing is re-implemented.
- `run_node.mjs`: runs the stage core on files. `compare_dataset.py` checks the result against
  the dump with these tolerances:

  | item | tolerance |
  |---|---|
  | views, order, holdouts, hot set, skipped | identical (`--frozen`: the 13 frozen holdouts) |
  | viewmats, K | identical float32 |
  | t0 / t1 | 1e-6 C |
  | hot_c | 1e-9 |
  | v_hot | 1e-12 |
  | undistorted Celsius | 1e-4 C |
  | targets | 1 float16 ulp, on at most 1e-5 of pixels |
  | z bounds | 1e-9 m |
  | ceiling | identical |
  | points | count and positions identical; values 1e-6 |
  | hot spot | 1e-3 m from both native hot spots |
  | lr, schedule, strategy, scene_scale, options | identical |

  Points are matched by position. The model has about 330-380 points at duplicated positions;
  those are matched by value.
- Schema and settings checks (after codex round 1): required fields, scale kind, the undistortion
  record against the camera, dtypes, shapes and file sizes, split and hot enums, each view's
  `frame` and `image_id`, each view's rejection reason and the pose-offset limit, complete
  scene/training option key sets, init constants, near plane, bounds-pruning frequency and the
  training `v_hot`. The native side gets these from train.py itself:
  - rejection reasons from `train.choose_images` run with the later filters relaxed;
  - init and near-plane constants, the seed, the SH degree schedule and the exact undistortion calls
    read from train.py's source, failing if the code changes; the undistortion method string must
    be exactly the contract's (round 2).
- `negative.py`: 28 perturbed copies of passing datasets, each of which must fail (exit 1):
  - numeric: a target off by 2 ulp, a point value off by 1e-5, the means rate off by 0.1%, t0
    off by 1e-4 C, one split flipped;
  - codex's round 1 corruptions, one per control: invalid frame/image_id (all views and one),
    wrong init, near plane, pruning frequency and training v_hot, scale kind, missing
    undistortion, target and point dtype/shape, deleted options (all and one), invalid splits,
    non-boolean hot, all skips labelled elevation, pose limit;
  - relabelled rejection reasons on the `reject` case.

  - round 2: seed changed or deleted, SH schedule replaced or deleted, undistortion method replaced.

  All 28 fail as they should.
- `test_units.mjs`: float32 to float16 rounding against `Float16Array` (1M values, every exponent),
  and the decode table.
- Cases, all against the dump of the same model and options:
  - `native-run13`: native model, run13's `scale.json` options;
  - `native-default`: train.py defaults (the SH baseline's scene options);
  - `browser-run13`: the browser's model from a harness run;
  - `budget`: `max_images=60`. 141 cold views are dropped and 13484 points are valued by
    projection, which exercises the fallback;
  - `reject`: `min_obs=1500 min_elevation=42`. 58 views are rejected by elevation and 107 by
    observations;
  - `explicit`: t0, t1, k, hot_delta, holdouts, radius, margins, cell, iterations, growth,
    refine_every, max_scale, sh_degree;
  - `all-views`: `max_images=0`, no holdouts;
  - `pose-offset`: three frames' telemetry moved 60 m, all three rejected as misregistered;
  - `pinhole`: the native model's camera rewritten as PINHOLE with unequal focal lengths and a
    shifted principal point. There is no undistortion; the dump builds K from `cam.params`.

## Results

All cases pass, and none has a tolerance in use. Every case has 0 differing Celsius pixels and 0
differing target pixels. Point positions are identical, and point values equal train.py's float64
values rounded to float32 (max 2e-8). The hot spot is 0.8-9.7e-6 m from train.py's: train.py
inverts the float32 viewmats in float32.

| model | views (test, hot) | scale t0..t1 C | points | hot spot ENU m |
|---|---|---|---|---|
| native final | 201 (13, 41) | 10.906774..126.423141 | 37781 | -0.942, 0.213, 31.567 |
| browser (stages 3-5 in the page) | 201 (13, 41) | 10.907346..126.395699 | 40632 | -0.927, 0.194, 31.574 |

Both models give the 13 frozen holdouts. The native-model numbers match the reference
`scale.json` and `train_baseline_sh.log` exactly.

Page runs, `test/harness/thermal.mjs --reference` (whole chain from the MKV):
- The harness copies `prepare/` out of OPFS in 8 MB chunks.
- It runs `dump_scene.py` on that run's map model and `compare_dataset.py --frozen` on the
  result: 29/29 pass on both machines.
- No console errors on either machine.

| | laptop Chrome (RTX PRO 1000) | win11 Chrome (RTX 4070) | Node (laptop) |
|---|---:|---:|---:|
| prepare stage | 8.1 s (pass 1 2.8 s, pass 2 5.1 s) | 3.9 s | 4.2 s |
| whole pipeline to dataset, wall | 220.5 s | 170.5 s | |

Timings are noisy:
- the laptop GPU and CPU were shared with G1's training runs and another harness run;
- the win11 GPU was shared with G1's Chrome instances.

Prepare is CPU-only, and its laptop page time is twice Node's on the same machine. Peak RSS in
Node is 230 MB.

Harness changes:
- `--profile NAME` selects a separate Chrome profile;
- `WEBSPLAT_APP_DIST` serves a separate build, so app/dist, which other agents use, is not
  rebuilt;
- `win11.sh` takes `PROFILE=` so that `stop` only stops that profile's Chrome. Runs here used
  `PROFILE=websplat-stage5b CDP_PORT=9365 APP_PORT=8805`, leaving G1's win11 Chrome alone.

## For stage 6 (Brush)

Decided (coordinator, after codex round 1; PROGRESS.md Decisions):
- keep float16 targets;
- stage 6 builds `init.ply`.

Codex's requirement for stage 6: the dataset only records train.py's rules. Stage 6 must
implement or enforce them in Brush:
- the recorded initialisation;
- learning rates and means schedule;
- densification and pruning;
- the Gaussian cap;
- cylinder and canopy-ceiling pruning.

Stock Brush applies none of these by itself. Point order and RNG differ from train.py, so stage 6
cannot claim identical initial Gaussians. The PNG16 transport must be validated against the
float16 source, and G1's metrics rerun with it.

Original proposals (for the record):

G1 currently feeds Brush `test/g1/prepare_g1.py`'s directory. Stage 6 must build the same
directory from this dataset:
- a COLMAP text model with the PINHOLE camera `K` and the chosen views' `viewmat`;
- 16-bit PNG targets;
- `init.ply`;
- `eval_views.txt` (the `split: test` names);
- Brush options: hot threshold = `v_hot`, the growth threshold mapping from `training`.

Decisions for G1 / the coordinator:

1. **Target precision.** `targets.f16` is exactly what train.py trains on (float16). G1 transports
   `round(65535 * v)` of float32 `v`, which is finer than train.py. Built from the f16 targets,
   the transport differs from G1's by up to the float16 half-ulp (2.4e-4 near v = 1). G1's
   criterion 7 (2e-5) then holds against the f16 targets, not against float32 `v`.
   - Proposal: keep f16, the train.py-exact choice; stage 6 encodes `round(65535 * f16)`.
   - Alternative: an extra `targets.u16` (G1's transport) written by prepare: 131 MB more OPFS.
2. **init.ply.** Brush needs scales and quaternions. train.py's are the kNN scale (mean distance to
   the 3 nearest points, min 1e-3), opacity 0.1 and `torch.rand` quaternions. These belong to the
   trainer, so stage 6 computes them from `points.f32`. The dataset only records the constants in
   `training.init`.
3. **Point order** follows `points3D.bin`; train.py's follows pycolmap's hash map. Only the
   initialisation order (random quaternions, kNN ties) can differ.
4. **Bounds.** Brush does not prune to the cylinder and canopy ceiling (notes/g1.md).
   `bounds` and `ceiling.f32` are there for the fork if stage 6 adds it.

CONTRACTS.md: additive clarifications only (v1.3.1). These cover `image_id`, `frame` = frames-list
position, `skipped.budget` and `pose_offset_limit_m`, the `training` block, dtype/shape records,
`stats`, point order, and the hot spot's precision.

## Open

- Exactness rests on OpenCV 5.0's AVX2/FMA remap and NumPy's float32 log1p on the reference
  machines. A NumPy build using AVX512 SVML log1p, or OpenCV 4's fixed-point remap, would make
  train.py itself differ (targets by about 1 float16 ulp at rounding boundaries). The tolerances
  above allow for this.
- Codex checked Stop in both passes, reload during target writing, restart and resume in an
  isolated Linux browser, all byte-identical to Node. The harness does not repeat these checks.
- Harness fixes after codex round 1:
  - `win11/stop.ps1` matches the complete `--user-data-dir` argument: exact quoted forms
    (`--user-data-dir="P"`, `"--user-data-dir=P"`) or unquoted `P`, each ending at whitespace or
    end of line (round 2). `stop.ps1 -test` checks the predicate only. 8 sample command lines,
    including `"...profile g1"` and `...profile-g1`, gave the expected matches. Tested on win11
    with dummy profiles `websplat-dummy5b` and `websplat-dummy5b-x`: stopping the first left the
    second running. G1's Chrome was not touched.
  - `WEBSPLAT_APP_DIST` is resolved to an absolute path.
- A served build older than the stage source fails the harness's dataset check (for example the
  init record changed in round 1). Rebuild the app before running `thermal.mjs`.
