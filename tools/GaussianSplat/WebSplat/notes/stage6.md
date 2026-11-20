# Stage 6: training

G1's Brush fork as the pipeline stage `train` (after `prepare` in the 'thermal' set; the
'export-test' set keeps the stand-in). Code: `app/src/stages/train/` (`stage.js`,
`train.stage.worker.js`, `brush-dataset.js`, `params.js`, `brush-worker.js`); fork patches
`brush-patches/brush/0001-0013` (0013 is stage 6's). Tests: `test/stage6/`.

## Data path

5b's `dataset.json`, `targets.f16`, `points.f32` and `ceiling.f32` become a Brush input directory
in the stage's scratch `brush/` dir (removed after training, never published):

- `sparse/0/{cameras,images,points3D}.txt` (PINHOLE K, world-to-camera quaternion and
  translation from each view's viewmat), `eval_views.txt` (5b's test split);
- `images/NAME.png`: 16-bit grey PNG of the float16 target, `round(65535 v)`. The worst
  transport error over every float16 code in [0, 1] is 7.6e-6 (half a PNG step;
  `test_units.mjs`); on the canonical dataset `check_brush_dataset.py` measures the same 7.63e-6
  against `targets.f16` (and poses 7.8e-8, kNN scales 2.7e-7 relative);
- `init.ply` (below) and `bounds.json` (below).

G1's metrics are scored on the run's own dataset, ground truth = 5b's float16 targets
(`g1_view.py`). The train.py baselines rescored on that ground truth move by under 0.02 dB /
0.02 C, so the G1 tolerances stand.

## train.py rules: what is enforced and how

| rule (dataset.json `training`) | Brush | how |
|---|---|---|
| init: 5b points, DC from value, opacity 0.1, log kNN scale (mean of 3 nearest), random quaternion | enforced | `initPly()` writes them to `init.ply`, Brush starts from it. Not identical Gaussians: same points and order, but the quaternions come from our RNG (mulberry32, seed 0), not torch's |
| learning rates means, scales, quats, opacities, sh0, shN | enforced | Brush flags (`brushConfig`). Means use train.py's absolute rate (1.6e-4 x extent) via the fork's `--lr-mean-absolute` (Brush otherwise multiplies by its median scale) |
| means schedule `gamma^step` | enforced | Brush's exponential decay from `lr` to `lr * gamma^iterations`; on resume the step count continues (fork `set_step_count`) |
| Adam eps 1e-15 | already Brush's | |
| SSIM weight 0.2, hot weight 20 above `v_hot` | enforced | G1's hot-weighted loss patch |
| refinement window: gsplat refines while 500 < step < 15000 | enforced (codex rounds 1-2) | the fork's `--refine-start-iter 500` and `--refine-stop-iter 15000` (patches 0015-0016) gate all of Brush's refinement (gradient growth, screen-size splits, opacity pruning and replacement, opacity decay) to that window; `--growth-start-iter 501` and `--growth-stop-iter 15000` as well. Before round 1 only gradient growth stopped at 15000; before round 2 refinement also ran at steps 200 and 400 |
| refine every 100 | **not enforced: 200** | Brush's own densification period (its densification differs from gsplat's). With train.py's 100, Brush grows to the cap and a hot view loses 2.2-2.7 dB on the reference reconstruction (runs A, C, D, E below; not on the page reconstruction, see "Refine every 100"). Bounds pruning no longer depends on it (next rows) |
| Gaussian cap 800000 | enforced | `--max-splats`; e2e checks `gaussians <= cap` |
| cylinder (radius 165 m about the centre), floor z_min | enforced, every 100 to the end | the fork's `--prune-bounds-every 100` (train.py's `prune_outside_bounds_every`): `prune_bounds()` every 100 steps through the last, removing Gaussians with their optimiser state, refine statistics and 3D-filter floor and replacing none, as train.py's `remove()`; a prune that would empty the model fails training. Patch 0016 fixes 0015's version, which dropped the optimiser after every refine (series 3 trained with that bug) |
| canopy ceiling (72x72 grid, 5 m cells) | enforced, every 100 to the end | same, z above the ceiling cell of the Gaussian's x, y (clamped to the grid); e2e checks that no final Gaussian is outside |
| too big: max scale > prune_scale3d x scene_scale (5 m) for reset_every < step < refine_stop_iter | enforced | same hook, every 100 inside that step window (gsplat prunes it at its refine steps, every 100) |
| SH degree 3 and its schedule `min(step // 1000, 3)` | enforced | `--sh-degree`; fork option `--sh-degree-interval 1000` (patch 0015 replaces 0014's masking): each step renders only the current degree's bands, sliced on the autodiff graph, so bands above it get exactly zero gradient; their values and Adam first moments stay bit-zero until enabled. Brush keeps one shared Adam second moment per splat (not per coefficient, unlike train.py's per-element Adam), reduced over the rendered bands only. Unit test `websplat_tests::inactive_bands_and_moments_stay_zero` checks values and first moments |
| grow / split / clone criteria (gsplat grad2d 0.0002) | **not enforced** | Brush grows by its own gradient statistic: `growth_grad_threshold` has no gsplat equivalent; stage 6 uses the value below |
| opacity reset every 3000, prune_opa 0.005 | reset: train.py does none; prune_opa: **not enforced** | gsplat 1.5.3's reset test `step % reset_every == 0 & step > 0` parses as `(step % 3000 == 0) and (0 > 0)` and is never true, so train.py never resets opacity. Brush instead decays opacity (0.004 per refine, now only until 15000) and prunes below 1/255 (train.py 0.005) |
| replacement of pruned splats | different | Brush replaces Gaussians pruned at refine by splitting others (before 15000 only); train.py does not. Brush also adds a small means noise on near-transparent splats (train.py has none) and a mip 3D filter |
| options train.py has but Brush does not (hot_tolerance, lobe, source_gain, pose_opt, appearance, focus_radius) | refused | the stage fails if any is set |

## Stage behaviour

- Progress in iterations, `extra.gaussians` is Brush's splat count. The ETA prior is G1's measured
  30k time (laptop 844 s, desktop 461 s) with the cost curve of G1's runs (training slows as
  Gaussians grow), so the controller's calibration starts from the right shape.
- Checkpoints every 5000 iterations: Brush's raw parameters (fork `exportCheckpoint`, unbaked PLY
  with the quaternions as trained, not normalised) converted to WSSP `params.bin` with
  `state.json` (iteration, Gaussians). The worker takes the snapshot between two `trainSteps`
  batches, so the parameters are exactly those of the iteration they are labelled with
  (`test/stage6/worker_snapshot_test.mjs`; the previous version labelled iteration 5000's
  checkpoint while the next batch was running, as codex found). Resume (after Stop or
  reload) rebuilds the Brush input, writes the checkpoint as `init.ply` and starts Brush at the
  checkpoint's iteration: the LR schedule and growth window continue. v1 resumes from parameters
  only: Adam moments, refine statistics and data order restart. The 3D-filter floor is not in the
  checkpoint; a resumed run recomputes it from the restored positions at its start (fork
  `restore_min_scale`, unit test), since refine, which otherwise sets it, stops at 15000.
- Stop: the runtime's stop, then worker termination (the nested Brush worker dies with it).
- Preview: every 2000 iterations a held-out hot view is rendered by readback (`renderEval`), halved
  and written as `preview.png`; the stage list shows the latest one while the stage is active
  (`test/stage6/preview_test.mjs`). No canvas presentation (the laptop's
  NVIDIA cannot present one).
- Failures: G1's worker contract. Any GPU validation/OOM error, shader error, uncaptured error or
  device loss makes the run invalid and fails the stage; no partial result is published.
- Outputs (CONTRACTS.md 6): `train.json`, `splat.ply` (Brush export with train.py's
  `apcg_splat` header), `eval/NAME.f32` (13 held-out renders), plus `preview.png`.

## Choosing Brush's own controls (native brush-cli, laptop)

Everything else as enforced above; one 30k run each, G1 metrics on the f16 ground truth against
both train.py baselines (rescored on it). Criterion 2 = hot PSNR (worst view vs baseline >= -2 dB),
3 = hot peak error (median <= 14.5 C).

| run | growth threshold | noise weight | refine | means LR | Gaussians | worst hot view dB | hot peak median C | fails |
|---|---|---|---|---|---|---|---|---|
| A | 0.0004 | 50 | 100 | train.py | 800000 (cap) | -2.38 | 12.8 | 2 |
| B | 0.0008 | 50 | 100 | train.py | 485757 | -1.13 | 24.5 | 3 |
| C | 0.0004 | 3.33 | 100 | train.py | 800000 | -2.36 | 4.9 | 2 |
| D | 0.0006 | 3.33 | 100 | train.py | 672786 | -2.68 | 11.1 | 2 |
| E | 0.0006 | 50 | 100 | train.py | 656897 | -2.15 | 18.2 | 2, 3 |
| F | 0.0004 | 50 | 100 | G1 (relative) | 800000 | -1.48 | 5.3 | none |
| **G** | 0.0004 | 50 | **200** | train.py | 793706 | -1.84 | 4.1 | none |

The worst hot view is 00149 in every run. The hot peak error varies widely from run to run (as
in G1). G keeps every enforced rule, including train.py's absolute means rate, and is the stage
default (`REFINE_EVERY`, `GROWTH_GRAD_THRESHOLD`, `MEAN_NOISE_WEIGHT` in `brush-dataset.js`;
`config.train` can override the last two).

## Stage 6 acceptance rule (recorded 2026-11-08T16:23:07Z, before any page-reconstruction baseline was run)

Criterion 3 is judged against train.py on the same reconstruction as the candidate. Fixed now;
no threshold or run set changes after results.

- Baselines: train.py SH-only with the stage 0 command (`train.py WORK --sparse MODEL --out OUT`,
  so all options as G1's two SH-only baselines: 30k iterations, holdout 20, hot holdout 8, hot
  weight 20, hot delta 10, k 8, no lobe or source gain), run twice natively on the laptop GPU.
  WORK is the reference work directory (its frames are pixel-identical to the page's select output,
  checked by test/harness/thermal.mjs); MODEL is the page's own reconstruction (`map/final` of the
  laptop page job). All 8 page runs produced a bit-identical 5b dataset (points.f32, targets.f16),
  so one model and one pair of baselines serve both machines.
- Holdouts: the 13 frozen views (cold 00015 00035 00066 00086 00115 00135 00167 00187, hot 00009
  00050 00096 00140 00149). The page dataset's test split is exactly these. If train.py's own
  holdouts on MODEL differ, the rule is not applied and this is reported instead.
- Scoring: `test/g1/metrics.py` on the page dataset (`g1_view.py`, ground truth its float16
  targets); baseline renders via `render_ply.py` from each baseline's splat.ply at the page
  dataset's poses.
- Bounds: notes/g1.md tolerances v2 (`metrics.criteria`) against the worse of the two new
  baselines; criterion 9 as G1 (min PSNR >= 45 dB, max peak difference <= 1 C).
- Decision: the G1 repeat policy per machine: exactly 5 valid runs; each of criteria 1-6 and 9
  passes in at least 4 of 5, and the median over the 5 runs of each statistic is within its bound.
  A run is valid if the train stage completes with all its outputs (no GPU, shader or device
  error); stage 7's SPZ refusal after training does not invalidate the training run. Invalid runs
  are replaced, at most 3 per machine.
- Run sets (config G): laptop = the 5 laptop runs 14-35-09, 15-14-10, 15-31-05, 15-47-52,
  16-04-00. win11 = 14-34-41, stop10000-15-13-55 and 3 new full runs. The win11 run 14-06-41 used
  config A (refine 100), so it is reported but not counted; 5 valid config-G runs therefore need 3
  new runs, not 2.
- For information only (no decision): train.py once on the reference reconstruction on the same
  machine, and G1's existing reference baselines, to show how much of view 00140's overshoot comes
  from the reconstruction.

## Results through the page (test/stage6/e2e.mjs, config G)

Each run: MKV -> ingest .. prepare (the page's own reconstruction), train, export; G1 metrics on the
run's held-out renders with its own float16 targets as ground truth, against G1's two train.py
baselines (reference reconstruction).

| run | train s | Gaussians | worst hot view dB | hot peak median C | failed criteria | export |
|---|---|---|---|---|---|---|
| win11 (config A, before G) | 461 | 800000 | -1.04 | 21.6 | 3 | done |
| win11 | 428 | 789793 | -0.96 | 21.6 | 3 | done |
| win11 stop at 10000, reload, resume | 447 (333 after resume) | 793774 | -0.20 | 17.4 | 3 | SPZ check (stage 7) |
| laptop | 785 | 789347 | -0.78 | 15.3 | 3 (borderline) | SPZ check (stage 7) |
| laptop rep 2 | 785 | | -0.85 | 21.6 | 3 | done |
| laptop rep 3 | 784 | | -1.19 | 19.7 | 3 | done |
| laptop rep 4 | 768 | | -0.86 | 7.8 | none | done |
| laptop rep 5 | 769 | | -1.06 | 21.6 | 3 | done |

The laptop's first run was borderline on criterion 3, so G1's repeat policy was applied (5 valid runs,
4 must pass): criterion 3 passed 1 of 5, **FAIL**; every other criterion passed 5 of 5. All five
runs train 30k iterations on the 8 GB laptop GPU in 768-785 s (G1: 844 s).

Every page run passes criteria 1, 2, 4, 5, 6 and 9 (exported PLY vs trainer renders: >= 73.8 dB,
peak <= 0.49 C), the contract checks, the Brush input check and has no console errors. GPU peak
2.7 GB (laptop), 2.8-3.7 GB (win11). The ETA, recorded every 10 s on win11, stays within -11% to
+12% of the actual remaining time from the first iterations to the end.

Criterion 3 is set by view 00140: Brush overshoots its peak to the top of the temperature scale
(126.4 C, true 104.8 C) in both win11 runs and laptop reps 2 and 5 (median 21.616 C each). Native
brush-cli with config G on the page dataset does the same (125.1 C), and on 5b's reference-model
dataset reaches 105.5 C, so browser and native Brush agree on the same data. train.py does not
overshoot on either reconstruction (see the acceptance results below): the overshoot is Brush's
on the page reconstruction, not the reconstruction alone.

Stop/resume: Stop paused training in 285 ms. After the reload the job reopened paused with the
10000 checkpoint, and Resume continued at iteration 10250 (`resumed_from: [10000]`); the
Gaussian count and ETA continued. Its quality is in the table (one run, so no conclusion that
resuming from parameters only costs quality).

Export: stage 7 refused the SPZ in 2 of 4 runs ("the SPZ does not decode within its bounds",
scales 1 and 2 of ~790k Gaussians; log scales lie in [-4.8, 2.4], well inside SPZ's range). The
trained splats reproduce it: `$WEBSPLAT_DATA/stage6/runs/laptop-2026-11-08T14-35-09/train/splat.ply`
and `.../win11-stop10000-2026-11-08T15-13-55/train/splat.ply`.

## Acceptance results (rule above, scored after it was recorded)

Baselines on the page reconstruction (laptop, train.py stage 0 command, its holdouts the frozen
13): page_b1 hot PSNR 37.86 dB, hot peak median 13.5 C, max 31.7 C; page_b2 37.78 dB, 18.1 C,
33.0 C. Bounds therefore: hot peak median <= 20.07 C, max <= 38.00 C (worse of the two + 2 / 5).
`test/stage6/score_policy.py` (output in `$WEBSPLAT_DATA/stage6/policy-{laptop,win11}.txt`):

| machine | criterion 3 runs passing | criterion 3 medians | other criteria (1, 2, 4, 5, 6, 9) | decision |
|---|---|---|---|---|
| laptop | 3/5 (fail: 15-14-10, 16-04-00, view 00140 at 21.6 C) | 19.70 C (<= 20.07), max 28.51 (<= 38.00) | 5/5 each, medians within bound | **FAIL** |
| win11 | 3/5 (fail: 14-34-41, 16-34-57, view 00140 at 21.6 C) | 19.96 C (<= 20.07), max 29.68 (<= 38.00) | 5/5 each, medians within bound | **FAIL** |

Both fail by one run. Laptop hot IoU passes narrowly (median 0.25, bound 0.24). The win11
config-A run, scored for information, fails criterion 3 (21.6 C).

View 00140 per trainer and reconstruction (true peak 104.8 C):

| trainer | reconstruction | rendered peak C | hot peak median C |
|---|---|---|---|
| train.py page_b1, page_b2 | page | 91.3, 86.7 | 13.5, 18.1 |
| train.py ref_b (this machine) | reference | 87.2 | 17.6 |
| train.py G1 baselines 1, 2 | reference | 92.3, 92.6 | 12.5, 12.2 |
| Brush native, config G | reference | 105.5 | 4.1 |
| Brush native, config G | page | 125.1 | 18.7 |
| Brush page runs (10, config G) | page | 126.4 in 4 runs; error 7.8-20.0 C in the others | 7.8-21.6 |

train.py undershoots 00140 by 12-18 C on both reconstructions, so the reconstruction does not
change train.py's result there. Brush overshoots it on the page reconstruction, and the four
saturated runs are exactly the ones failing criterion 3.

## Refine every 100 (train.py's rule, bounds pruning then also every 100)

| run | dataset | worst hot view vs baselines dB | hot peak median C | criteria failed |
|---|---|---|---|---|
| browser, laptop (`--train-config '{"refineEvery":100}'`) | page | -1.06 (page baselines) | 8.5 | none (page baselines; none vs G1's either) |
| native brush-cli (config A) | page | -0.60 (page baselines) | 16.3 | none |
| native A, C, D, E (above) | reference | -2.15 to -2.68 (G1 baselines) | 4.9-18.2 | 2 (view 00149) |

The 2.2-2.7 dB loss is not specific to native Brush. It belongs to the reference
reconstruction's view 00149 and does not appear on the page reconstruction in either trainer
(one run each). Stage 6 trains only on the page's own reconstruction, so the reason for 200 does not
apply there. Current default: still **200**, a deviation from train.py's 100 (bounds pruning every
200, not 100). Choosing 100 means a new policy series per machine (the rule above fixed config G's
runs), so the default is unchanged pending that decision.

## View 00140: root cause and fix

The failing runs' 126.4 C is the scale top (value 1.0): Brush's eval render is clamped to [0, 1]
(brush-train eval.rs), as G1's metrics clamp it. Not the 5b ceiling, the target encoding (target
0.9297, exact float16) or patch 0013's pruning: every Gaussian at the hot spot lies inside the
bounds. `hotspot_gaussians.py` decomposes pixel (271, 425) of 00140 from the PLY (it reproduces
the trainer's render, 0.9567, exactly): unclamped values 1.008-1.061 in the failing runs, 0.943-0.982
in passing ones. Brush made the hot spot from view-dependent SH (DC-only about 0.5, SH adding
about 0.5, opacity 0.5-0.76); train.py from DC (DC-only 0.71-0.75, SH adding 0.13, opacity 1.0).
Extrapolated to the held-out view, Brush's large SH term overshot in about 4 of 10 runs.

Native runs on the page dataset (config G otherwise), against the page baselines:

| run | change | 00140 unclamped (target 0.930) | hot peak median C | criteria failed |
|---|---|---|---|---|
| X1 | `--opac-decay 0` | 0.929 | 6.2 | 5 (hot IoU 0.217) |
| X2 | train.py SH schedule (`--sh-degree-interval 1000`) | 0.928 | 2.7 | none |

| X2 rep 2 | same as X2 | 0.948 | | none |
| X2 rep 3 | same as X2 | 0.895 | | none |
| X2 + refine 100 | SH schedule and `--refine-every 100` | 1.059 | 21.6 | 3 |

For comparison, the 11 config-G Brush runs on the page dataset put 00140 at 0.914-1.065 (mean about
1.0; 4 of 11 above 0.996, where criterion 3 starts failing), train.py at 0.853 and 0.875
(`$WEBSPLAT_DATA/stage6/hotspot/00140_values.txt`). X2 is a parity fix (a train.py rule Brush did
not follow) and is now the stage default. X1 is not used. Refine stays at 200: with the SH schedule
the one refine-100 run overshot, and the three refine-200 runs did not.

Note (not a criterion): near the top of the temperature scale a small value error is a large
error in C. The log1p scale is compressed there: value 0.930 is 104.8 C, 0.981 is 120.1 C and
1.0 is 126.4 C. An overshoot of 0.07 in value becomes 21.6 C at view 00140.

## Series 2 (pre-registered 2026-11-08T18:52:17Z, before any run of it)

Configuration (the stage defaults from this point): config G plus train.py's SH degree schedule
(`--sh-degree-interval 1000`, patch 0014), i.e. every rule in the table above with refine every 200
(the documented deviation), growth threshold 0.0004, means noise weight 50. Build: the app with the
Brush package from patches 0001-0014 (wasm sha256 prefix 942a0fded8373dfb).

Rule and bounds: unchanged from "Stage 6 acceptance rule": the same two page-reconstruction
train.py baselines (page_b1, page_b2) and their bounds (hot peak median <= 20.07 C, max <= 38.00 C;
all other bounds as scored there), G1's repeat policy per machine (exactly 5 valid runs; every
criterion 1-6 and 9 passes in >= 4 and its median over the 5 runs is within bound), validity as
defined there (the train stage completes with all outputs and no GPU, shader or device error;
stage 7's SPZ refusal does not invalidate; at most 3 invalid runs replaced per machine). The page
dataset must be bit-identical to the one the baselines were trained on (points.f32 sha256 prefix
837495bcc8f2, targets.f16 4a10944cbdcd); a run whose dataset differs is invalid.

Runs: the next 5 valid full page runs (`test/stage6/e2e.mjs`, no Stop) on the laptop and the next 5 on
the win11 machine, all started after this section was written; none of the earlier runs count.
Scored with `test/stage6/score_policy.py` against page_b1 and page_b2. Nothing here changes after
the first run starts.

## Series 2 results (scored after all 10 runs, as registered)

All 10 runs valid: dataset bit-identical to the baselines' (points.f32 837495bcc8f2, targets.f16
4a10944cbdcd), `sh-degree-interval` 1000 in every train.json, export done, no console errors.
Output: `$WEBSPLAT_DATA/stage6/s2-policy-{laptop,win11}.txt`.

| machine | runs | criterion 3 | criterion 5 | others | decision | train s |
|---|---|---|---|---|---|---|
| laptop | 18-52-53, 19-09-20, 19-25-48, 19-42-15, 19-58-08 | 5/5, median 6.01 C (<= 20.07) | 5/5, median 0.26 (>= 0.24) | 5/5 each | **PASS** | 758-785 |
| win11 | 18-52-53, 19-03-51, 19-14-48, 19-25-51, 19-36-49 | 4/5 (19-03-51: 00140 20.3 C), median 12.83 C | 4/5 (19-14-48), median 0.26 | 5/5 each | **PASS** | 434-443 |

View 00140 peak errors: laptop 0.1-17.2 C, win11 4.0-20.3 C (series 1: 15.3-21.6 C, 4 of 10 at
the 21.6 C clamp). Stage 6 meets the acceptance rule on both machines with train.py's SH schedule
enforced. The remaining deviations from train.py are listed in the rules table (refine and bounds
pruning every 200, Brush's own densification and opacity decay, initial quaternions from our RNG).

## Codex round 1 fixes and native A/B (page dataset, laptop, against page_b1/page_b2)

Fixes (`REVIEW-stage6.md` round 1): (1) SH schedule by sliced render input, inactive bands and
moments bit-zero (unit test); (2) checkpoints taken at batch boundaries (`worker_snapshot_test.mjs`,
which fails on the old worker: label 50, parameters 78); (3) raw quaternions in checkpoints (unit
test); (4) bounds pruning every 100 steps to the end, without replacement (`prune_bounds`, unit
test; `check_bounds_growth.py` in e2e); (5) all refinement stops at 15000, refinement starts after
500 as gsplat; (6) `score_policy.py` checks validity, distinctness, dataset, build and configuration;
(7) the preview stays while the stage is active (`preview_test.mjs`). Patch 0015, fork 8dcc0ad.

Why 0014 still fixed 00140: it zeroed the inactive bands and their moments at the start of every
step, so every forward pass rendered the degree-limited model; its defects were between steps (one
step's Adam update in inactive bands, wiped at the next step; the per-splat SH second moment
including inactive gradients; bands starting from one step's update when enabled).

| run | build | 00140 unclamped (DC only) | hot peak median C | hot IoU (bound 0.243) | outside bounds (> 1 cm) | failed |
|---|---|---|---|---|---|---|
| X2, rep 2, rep 3 | series 2 | 0.928, 0.948, 0.895 (0.56, 0.59, 0.55) | 2.7, , | 0.273, 0.286, 0.263 | | none |
| F3a | fixed | 0.887 (0.665) | 10.5 | 0.240 | 2 (0) | 5 |
| F3b | fixed | 0.911 (0.715) | 4.8 | 0.214 | 0 | 5 |
| F3c | fixed | 0.904 (0.632) | 7.1 | 0.240 | 1 (0) | 5 |
| H1 | fixed, `--refine-stop-iter 0` | | 21.6 | 0.274 | | 3 |
| H2 | fixed, `--prune-bounds-every 0` | | 12.0 | 0.264 | | none |

The fixed build makes the hot spot from DC much as train.py does (DC-only 0.63-0.72; train.py
0.71-0.75) and removes the overshoot, but hot IoU fell below its bound in all three runs. Hot IoU
rests on two views with tiny hot regions: 00149 has 6 true hot pixels, and the fixed runs mark 3 of
them (IoU 0.5) where series 2 marked 4-5 (0.667-0.714). The missing pixels lie 0.005-0.04 below
v_hot (0.479). H1 and H2 (one run each) recover IoU, H1 by bringing the overshoot back. Neither is
used: H1 undoes finding 5's parity, H2 reintroduces finding 4. train.py's own finals also leave 1-2
Gaussians a few mm outside the bounds (99 steps after the last prune at 29900); the check allows 1 cm.
Series 3 tests the corrected build under the unchanged rule; criterion 5 is at risk.

## Series 3 (pre-registered 2026-11-08T21:56:34Z, before any run of it)

Configuration (the stage defaults from this point): the codex round 1 build: patches 0001-0015,
app build id bea773269f541ce6 (`e2e.mjs` report `build.id`: sha256 of the Brush wasm
9985e91f4d41359d..., the train stage worker and the Brush worker assets). Brush options from
`brushConfig`: every rule in the table above, SH schedule `--sh-degree-interval 1000`, refinement
from 501 to 15000 (`--growth-start-iter 501`, `--growth-stop-iter 15000`, `--refine-stop-iter
15000`), refine every 200 (the documented deviation), bounds pruning `--prune-bounds-every 100` to
the end, growth threshold 0.0004, means noise weight 50.

Rule and bounds: unchanged from "Stage 6 acceptance rule" and series 2: baselines page_b1 and
page_b2, their bounds (hot peak median <= 20.07 C, max <= 38.00 C, hot IoU >= 0.243, all others as
scored there), G1's repeat policy per machine (exactly 5 valid runs; every criterion 1-6 and 9 passes
in >= 4 and its median over the 5 runs is within bound). Validity as before (train stage complete
with all outputs, no GPU, shader or device error; stage 7's SPZ refusal does not invalidate; dataset
bit-identical to the baselines': points.f32 837495bcc8f2, targets.f16 4a10944cbdcd; at most 3
invalid attempts replaced per machine), plus: build id bea773269f541ce6, one trainer configuration
across the series, distinct runs, and on the laptop no other compute process on the GPU at the
start of a run (report `gpuBefore`). Scored with `test/stage6/score_policy.py --build
bea773269f541ce6 --prepare` (the baselines' dataset) over each machine's whole ledger.

Runs: the next 5 valid full page runs (`test/stage6/e2e.mjs`, no Stop) on the laptop and the next 5 on
the win11 machine, all started after this section was written; no earlier run counts. The e2e
bounds/growth check is reported per run, not a decision criterion. Nothing here changes after the
first run starts.

### Series 3, laptop run 5: external GPU load (decided 2026-11-08T23:17:35Z, before its results were seen)

Run laptop-2026-11-08T23-06-55 started at 23:06:55Z with the GPU idle (its start check passed). From
about 23:10Z to 23:16:50Z another Chrome window on the laptop rendered a 530k-Gaussian splat on the
same GPU, overlapping the run's map, prepare and early training. The registered validity rule
invalidates a laptop run for another compute process at its start, or for a GPU, shader or device
error; it has no provision for load that begins mid-run, and timing is not a criterion. The run is
therefore kept (replacing it would be an unregistered change to the run set), unless it ends with
one of those errors. Its timings are excluded from the timing figures reported below.

## Series 3 results (scored after all 10 runs, as registered)

All 10 runs valid under the rule: build bea773269f541ce6, one configuration, distinct runs, dataset
bit-identical to the baselines', laptop GPU idle at each start, train stage complete, export done,
no console errors (laptop run 5 kept, see above). Output: `$WEBSPLAT_DATA/stage6/s3-policy-{laptop,win11}.txt`.

| machine | criterion 3 (hot peak) | criterion 5 (hot IoU) | others (1, 2, 4, 6, 9) | decision |
|---|---|---|---|---|
| laptop | 4/5 (23-06-55 fails: 00140 30.3 C), median 10.63 C | **3/5** (22-31-58, 23-06-55), median 0.25 (>= 0.24) | 5/5 each | **FAIL** |
| win11 | **3/5** (21-56-50, 22-49-01: 00140 24.8, 20.8 C), median 15.00 C | **3/5** (22-08-28, 22-49-01), median 0.25 | 5/5 each | **FAIL** |

Stage 6 with the codex round 1 fixes does not meet the acceptance rule on either machine; every
median is within its bound and the failures are single runs over the 4-of-5 line. Both failing
criteria come from the same two views with a few hot pixels: 00140's peak (an overshoot in some
runs, undershoot in others) and 00149's 6-pixel hot region.

Refinement rules on the results (`check_bounds_growth.py`, reported, not a criterion): no run grows
after 15000 (0 rises in 10; the count only falls, by bounds pruning, from about 799.9k to 798.5k);
9 of 10 have nothing further than 1 cm outside the bounds. win11 22-08-28 keeps one near-transparent
Gaussian (opacity 0.007) 4.3 cm across a cell edge into a cell whose ceiling is 1.1 m lower, moved
by Brush's means noise in the 99 steps after the last prune (train.py has no such noise). The other
metre-scale vertical excesses are 0.5-3.3 mm moves across cell edges where the ceiling steps.

Training time (train stage): laptop 838-857 s (runs 1-4; series 2: 758-785 s), win11 476-513 s with
one run at 623 s (series 2: 434-443 s).

## Codex round 2 fixes and native A/B

Fixes (`REVIEW-stage6.md` round 2; patch 0016, fork 3b90167): (1, P1) `prune_bounds` took the
optimiser and the refine record together and, right after a refine (no record), returned having
dropped the optimiser: Adam restarted every 200 steps below 15000 and that prune was skipped. Series
3 trained with this bug. Now the optimiser is always kept and the record subset only if present
(unit test step -> refine -> prune: Adam's moments and step counter continue). (2) A resumed run
recomputes the 3D-filter floor at its start (unit test after 15000). (3) `--refine-start-iter 500`
gates all refinement to 500 < step, as gsplat. (4) A prune that would remove every Gaussian fails
training (unit test). (5) `score_policy.py` counts unreadable attempt metadata as an invalid attempt
(`score_policy_test.py`). (6) `e2e.mjs` records the nvidia-smi query status; a laptop run without a
successful query (gpuBefore null or missing) is invalid. The SH row above now states the second
moment accurately (one shared per splat; the test checks values and first moments).

Native A/B, page dataset, laptop, new build (against page_b1/page_b2):

| run | 00140 unclamped (DC only) | hot peak median C | hot IoU (00140, 00149) | outside > 1 cm | failed | wall s |
|---|---|---|---|---|---|---|
| F4a | 0.891 (0.490) | 8.7 | 0.236 (0.75, 0.429) | 0 | 5 | 1481 (two in parallel) |
| F4b | 0.896 (0.561) | 7.5 | 0.293 (0.75, 0.714) | 0 | none | 1480 (two in parallel) |
| F4c | 0.908 (0.565) | 5.2 | 0.293 (0.75, 0.714) | 0 | none | 684 (alone) |

(Round 1 build, F3a-c: hot IoU 0.214-0.240 in all three; series 3 laptop training 838-857 s.)

## Series 4 (pre-registered 2026-11-09T00:19:31Z, before any run of it)

Configuration (the stage defaults from this point): the codex round 2 build: patches 0001-0016,
app build id cd449d1b2ede4dc3 (`e2e.mjs` report `build.id`: sha256 of the Brush wasm
6a9b5f9dab7c6edc..., the train stage worker and the Brush worker assets). Brush options from
`brushConfig`: every rule in the table above, SH schedule `--sh-degree-interval 1000`, refinement
only while 500 < step < 15000 (`--refine-start-iter 500`, `--refine-stop-iter 15000`,
`--growth-start-iter 501`, `--growth-stop-iter 15000`), refine every 200 (the documented deviation),
bounds pruning `--prune-bounds-every 100` to the end, growth threshold 0.0004, means noise weight 50.

Rule and bounds: unchanged from "Stage 6 acceptance rule" and series 2-3: baselines page_b1 and
page_b2, their bounds (hot peak median <= 20.07 C, max <= 38.00 C, hot IoU >= 0.243, all others as
scored there), G1's repeat policy per machine (exactly 5 valid runs; every criterion 1-6 and 9 passes
in >= 4 and its median over the 5 runs is within bound). Validity as for series 3 (train stage
complete with all outputs, no GPU, shader or device error, stage 7's SPZ refusal does not
invalidate, dataset bit-identical to the baselines', build id cd449d1b2ede4dc3, one configuration,
distinct runs, at most 3 invalid attempts replaced per machine), with the laptop GPU evidence as
fixed in round 2: a successful nvidia-smi query listing no other compute process at the start of
each laptop run. Scored with `test/stage6/score_policy.py --build cd449d1b2ede4dc3 --prepare` (the
baselines' dataset) over each machine's whole ledger.

Runs: the next 5 valid full page runs (`test/stage6/e2e.mjs`, no Stop) on the laptop and the next 5 on
the win11 machine, all started after this section was written; no earlier run counts. The e2e
bounds/growth check is reported per run, not a decision criterion. Nothing here changes after the
first run starts.

## Series 4 results (scored after all 10 runs, as registered)

All 10 runs valid: build cd449d1b2ede4dc3, one configuration, distinct runs, dataset bit-identical
to the baselines', laptop GPU idle at each start by a successful nvidia-smi query (gpuBefore [],
status 0), train stage complete, export done, no console errors. Output:
`$WEBSPLAT_DATA/stage6/s4-policy-{laptop,win11}.txt`.

| machine | criterion 3 (hot peak) | criterion 5 (hot IoU) | others (1, 2, 4, 6, 9) | decision | train s |
|---|---|---|---|---|---|
| laptop | 5/5, median 4.47 C (<= 20.07), max median 30.75 | 5/5, median 0.27 (>= 0.24) | 5/5 each | **PASS** | 738-757 |
| win11 | 5/5, median 10.00 C, max median 30.08 | 4/5 (00-34-50), median 0.26 | 5/5 each | **PASS** | 638-767 |

View 00140 peak errors: laptop 1.0-16.5 C, win11 0.7-11.1 C (no run near the 21.6 C clamp).
Refinement rules on the results: no growth after 15000 in any run; nothing further than 1 cm outside
the bounds in any run (largest 2.8 mm). Stage 6 meets the acceptance rule on both machines with the
codex round 2 build.

Timing: laptop training 738-757 s, below series 2 and 3 (758-857 s; series 3 also restarted Adam
every 200 steps). win11 training 638-767 s, above its earlier 434-513 s: splat training ran concurrently
in that machine's WSL VM during the series (its GPU load shows in nvidia-smi's utilisation, 98-99%,
but not in its process list; corrected 2026-11-09, an earlier version of this note blamed the
interactive session). win11 runs are exempt from the idle check by the rule, so this affects timing
only and the result stands; the win11 ETA prior is not revised from these runs.
