# Stage 4: pairs and matching (verification hand-off only)

2026-11-08. Pair selection and the WebGPU matcher are done and pass their acceptance tests
on the laptop (RTX PRO 1000 and Intel Xe-LPG) and win11 (RTX 4070). Verification is G2's
(colmap-wasm fork); the hand-off is defined below. Round 2 fixes the 7 findings of
REVIEW-stage4.md (section "Review round 1 fixes" at the end).

## Files

`app/src/stages/match/` (ES modules, no build step, no dependencies)
- `pairs.js`: `selectPairs(frames, target, opts)`, a port of sfm.py `select_pairs()`.
- `quantize.js`: sfm.py's descriptor preparation, bit-exact (uint8 → f32 / max(norm, 1e-6) → fp16).
- `matcher.js`: `createDevice()`, `matchPairs(device, pairs, counts, descriptors, opts)`.
- `format.js`: WSMT matches and WSTV geometry containers (CONTRACTS.md 4), validating decoders.
- `worker.js`: module worker, one run at a time, every message tagged with the caller's run
  `id`: `run` / `cancel`; posts `pairs` (with `ambiguous`), `progress`, `done`, `error`
  (`Busy` for an overlapping run).

`test/stage4/`
- `export_native.py`: native database → `$WEBSPLAT_DATA/stage4/native/` (descriptors in frame
  order, `matches.bin` (WSMT), `geometry.bin` (WSTV with F/E/H and pose), fp16 reference values).
- `native_rematch.py`: re-runs sfm.py's own `match_pairs()` on CUDA (stub database, no
  verification) → `native_raw.bin` + `native_raw.json` (backend settings, GEMM error).
- `select_pairs_native.py`: sfm.py's `select_pairs()` on JSON input, for test_pairs.mjs.
- `test_pairs.mjs`, `test_quantize.mjs`, `test_format.mjs`: Node tests.
- `match_test.html` + `run_match.mjs`: browser harness (local headless Chrome, or `--cdp PORT`
  for a remote Chrome); `mode=edge` runs synthetic edge cases, the default runs all reference pairs.
- `compare_matches.py`: the float-tolerance acceptance of a browser container against native
  (`--selftest` corrupts the browser data and requires the corruption to be caught).
- `win_chrome.ps1`: start/stop/list a private headless Chrome on the Windows machine.
- `run_tests.sh`: everything above in order (`set -euo pipefail`, fresh output required).

## Pair selection

Exact port: same direction/range geometry, `cos(25°)` as `math.radians` computes it, scale
test, `j != i`, view neighbours by descending cosine, plus 5 sequential neighbours, sorted
unique (i, j). Result: **3175 pairs, identical to `ref/work/colmap/pairs.txt`**. 5 ms in Node.

Tie policy: neighbours are ordered by descending cosine, then ascending frame index.
numpy's argsort is unstable, so its choice inside a tie is not reproducible; every decision
within 1e-12 of a tie (neighbour cut-off inside a run of equal cosines, a cosine on the angle
threshold, a range ratio on the scale threshold) is returned as `ambiguous` and posted by the
worker in `pairs` and `done`. Parity with sfm.py is claimed only when it is empty; the app
should warn otherwise (repeated telemetry positions are the realistic cause). Tests: the
reference has none; four jittered synthetic orbits (1355-3665 pairs) are identical to
sfm.py's `select_pairs()` run natively with none; 80 frames alternating between two positions
report 80 ambiguous cut-offs and differ from numpy in 56 pairs (codex's case), all under a report.

## Matching rule

The reference ran on CUDA (`matching 3175 pairs on cuda`), so sfm.py's rule there is:
fp16 normalised descriptors, `sim = fp16(a @ b.T)`, top-2 per row, `dist = sqrt(clamp(2 - 2 sim, 0))`
in f32, keep `d0 < 0.8 d1`, reverse `argmax(dim=0)` (first index on ties) must return the row;
pairs with fewer than 2 descriptors on either side are skipped (no matches row).
A numpy check on 11 sampled pairs: fp16 emulation reproduces the native matches exactly;
plain f32 sims differ by 1-5 matches per pair. The browser therefore emulates fp16:
- Descriptors are normalised and rounded to fp16 on the CPU, bit-exact with numpy (squared
  norms are exact integers in f32; test over 4.1M values), uploaded as packed fp16 and
  unpacked in WGSL (`unpack2x16float` is exact).
- Products of fp16 values are exact in f32; sums are f32, then rounded to fp16 with explicit
  round-to-nearest-even in WGSL (WGSL leaves f32→f16 rounding direction open).
- One GEMM per pair. A workgroup owns 64 rows of A and walks B in 64-column tiles (16x16
  threads, 4x4 outputs each); row top-2 is kept as fp16 bits in registers; columns get one
  `atomicMax` per tile of `fp16bits << 16 | (0xffff - row)`, which yields the column max with
  the smallest row on ties, exactly torch's argmax. Up to 65534 descriptors per image.
- Ties: equal top-2 values fail the strict ratio test, so the forward index under a tie never
  matters; reverse ties go to the first row as in torch.
- The ratio test runs on the CPU from the fp16 top-2 through f32 tables, so it is f32-exact
  (correctly rounded sqrt and multiply, as on CUDA).
- Edge-case test (`mode=edge`): random, permuted and duplicated descriptors (forward and
  column ties), sizes not multiples of 64, 1 and 0 descriptors (skipped), i > j, against a
  CPU reference of the rule: all identical. Cancellation (AbortSignal, between batches,
  returns `AbortError` with the pairs done) and `device.lost` (`DeviceLost` with the pairs
  done) are tested, also through real worker messages (run, progress, cancel by id) and an
  overlapping run (rejected `Busy`, the first completes). Setup failures: an injected bad
  kernel (`opts.wgsl`) gives `Shader` with every created buffer destroyed, balanced error
  scopes and a reusable device; a device lost before setup gives `DeviceLost`, buffers
  destroyed. The out-of-memory path (pool halved and retried) is written but not exercised.

Batching and resources: descriptors live in a pool of fixed slots (one image each, 10112 x 256
bytes here) in one storage buffer of at most `poolBytes`, default 128 MiB = 51 slots, so no
limits beyond the WebGPU defaults are requested (a larger pool requests them). Slots are LRU;
a batch is up to 32 pairs whose images fit in the pool together; 2 submits in flight, each
with its own staging buffer. GPU memory: ~135 MiB. Host: the uint8 descriptors (186 MB for the
reference, transferred to the worker, not copied) plus results. 473 image uploads for the 3175
pairs (quantisation 5.8 ms per 8192 descriptors, overlapped with the GPU).

## Acceptance against native

The database's `matches` table is not entirely raw: COLMAP's verification empties the stored
matches of every pair with fewer than `min_num_inliers` (15) matches (shown by writing 12
matches for (19, 165) into a copy of the database and running `pycolmap.verify_matches`: 0 rows
left; a 22-match pair keeps its 22). So the raw native matches were recomputed:
`native_rematch.py` runs sfm.py's own `match_pairs()` (stub database, no verification) on
CUDA. On an RTX 4070 (the reference GPU model; torch 2.14.1+cu130, CUDA 13.0,
`allow_fp16_reduced_precision_reduction=True`, TF32 off, the same settings as sfm.py) it
reproduces the database exactly on all 3151 pairs verification kept (0 differences) and gives
212 matches in the 24 emptied pairs, at most 14 per pair: this is `native_raw.bin`, the
reference. The browser's matches in those pairs are identical to it. The same re-run on the
laptop's Blackwell GPU differs from the database in 14 matches, so the GEMM rounding is
GPU-specific.

Measured GEMM error, on 40 pairs sampled evenly from the 3175 (2.1e9 similarities, against
the exact float64 sums of the fp16 products): at most 0.5047 fp16 ulp on the RTX 4070 and
0.5024 on Blackwell. This is an empirical bound for that sample, not a guarantee for every
similarity or input; the 0.51-ulp tolerance below rests on it. The browser's fp32 sum then
RNE is within 0.5 ulp plus a few fp32 ulps.

Acceptance (`compare_matches.py`):
- every container (database, raw re-run, browser) is validated before comparison, with no
  dict/set collapsing: contiguous offsets, totals, statuses, unique pairs with i < j,
  feature indices in range, one-to-one matches within each pair;
- pair list (in order) and statuses identical; the raw reference is validated against the
  database as above;
- each browser difference must be producible by sfm.py's rule when every similarity takes
  an fp16 value within 0.51 ulps of its exact value (interval endpoints quantised to fp16),
  evaluated as torch does: `float32(sqrt(max(float32(2 - 2 s), 0)))`, strict
  `d0 < float32(float32(0.8) d1)`, forward argmax, reverse argmax with the first row on
  ties. A reported match needs all predicates to pass for some permitted values; a reported
  non-match needs one to fail.

Result, all 3175 pairs: 510868 raw native matches, 35 differ (6.9e-5), all producible; the
largest tolerance any needs is 0.5002 ulps (that is, the neighbouring fp16 value of a
near-midpoint sum). `--selftest` must reject codex's counterexample (extra (295, 5733) in pair
(0, 1): at its best fp16 values d0 = 0.40864676 > float32(0.8 d1) = 0.40850338), a dropped
robust match, a bogus extra match, and six container corruptions (duplicate pair, duplicate
match, duplicate row, duplicate column, bad offset, index out of range). The laptop NVIDIA
and win11 outputs are byte-identical; the Intel adapter differs from them in 4 matches and
passes the same check.

## Measurements

Full reference run (3175 pairs, 201 images of up to 10099 descriptors, 13.5 GFLOP/pair), timed
inside the worker from device ready to last pair; matching only, excluding loading the data
and starting the worker (under 0.5 s locally, about 3 s over the tunnel to win11).

| machine | Chrome | ms/pair | pairs/s | 3175 pairs | effective |
|---|---|---:|---:|---:|---:|
| laptop RTX PRO 1000 (Vulkan) | 155.0.8059.39 Linux, `--use-angle=gl --enable-features=Vulkan` | 8.68 | 115 | 27.5 s | 1.55 TFLOPS |
| laptop Intel Xe-LPG (Vulkan, `low-power`) | same | 99.8 | 10 | 317 s | 0.14 TFLOPS |
| win11 RTX 4070 (D3D12) | 156.0.8078.82 Windows, `--enable-unsafe-webgpu --force_high_performance_gpu` | 2.49 | 401 | 7.9 s | 5.4 TFLOPS |

Laptop figures are from six runs (8.66-8.75 ms/pair) started with the GPU idle; the round 2
`run_tests.sh` runs gave 8.84 (laptop) and 2.48 ms/pair (win11); one run gave 12.45 while
another agent's Brush training shared the GPU.
The Intel figure is a full run (a first-100-pair subset gave 90.6 ms/pair).

Native sfm.py matching on the RTX 4070 took 17 s (including database writes), so the browser
is about 2x faster there. Batch 16/32/64, depth 2/3/4 and a 256 MiB pool all ran within 1%:
both GPUs are compute-bound. The 1 to 1.5 min / 30 s estimate in PLAN.md for stage 4
(matching plus verification) stands for matching at 28 s / 8 s; verification is G2's.

## Chrome/Dawn bug found (Linux Vulkan)

A function-scope `var` without an initializer declared inside a loop is not re-zeroed on each
iteration on Chrome 155 Linux / Vulkan, on both the NVIDIA (driver 595.99.02) and Intel
adapters; win11 D3D12 is correct. WGSL requires zero initialization at each declaration. Minimal
repro: `for (t < 3) { var acc: array<vec4f, 4>; acc[0] += 1 (twice); total += acc[0].x }` gives 12
instead of 6. The matcher initializes explicitly; other WGSL in WebSplat (and the Brush fork)
should not rely on implicit zeroing inside loops. Possibly related to the texture issue in
PROGRESS.md; worth a Dawn bug report. No console warnings or errors were seen in the matcher runs.
Regression tests (`mode=edge`): a lint fails if the kernel declares an uninitialised
function-scope `var`, and the probe runs the repro and reports `loopVarBug` (true on the laptop,
false on win11) while the edge cases verify the matcher is correct on both.

## Hand-off to verification (G2)

Input: `match/matches.bin` (WSMT v1, CONTRACTS.md 4; `format.js decodeMatches` validates
lengths, offsets, totals, statuses and, given counts, frame and feature indices), stage 3's
keypoints (WSFT) for the feature indices, and the camera (SIMPLE_RADIAL from frames.json K,
focal prior). The verifier runs COLMAP `TwoViewGeometryOptions()` defaults as
`pycolmap.verify_matches` does, including emptying pairs below `min_num_inliers`, and writes
`match/geometry.bin` (WSTV v1: inliers, config, F/E/H, `cam_j_from_cam_i`; `encodeGeometry` /
`decodeGeometry` in format.js). Fixtures for G2: `$WEBSPLAT_DATA/stage4/native/geometry.bin`
is the native `two_view_geometries` in WSTV form (config 0: 473, 2: 1101 (120637 inliers),
3: 1016 (19927), 6: 585 (247924), the contract's reference counts; checked by test_format.mjs),
and `$WEBSPLAT_DATA/stage4/out/*.bin` are browser WSMT outputs to feed it. G2's own acceptance
(verified pairs and inliers at least 90% of native with the browser features) is outstanding.

## Proposals for CONTRACTS.md (stage 0 owns it)

- `match/pairs.json` should carry the `ambiguous` list from the tie policy.
- WSMT pair order: the contract says pairs order with i < j, as the matcher produces from
  pairs.js; `decodeMatches` accepts any i != j (the matcher also matches caller-given pairs).
- Record the matcher's parameters (ratio 0.8, fp16 emulation, pair options) in
  `match/matches.json` as the configuration hash input.

## Not done / open

- Stage 0 integration (stage registration, OPFS input/output per CONTRACTS.md, checkpoint
  per batch through `onBatch`): the worker still takes typed arrays.
- sfm.py's CPU (no CUDA) path computes in f32; the matcher reproduces only the CUDA path,
  which produced the reference. An f32 variant would need a two-pass column argmax.
- Out-of-memory recovery is untested (no way found to force it).

## Review round 1 fixes (REVIEW-stage4.md)

1. compare_matches.py: all-predicate feasibility per difference against the recomputed raw
   native matches (native_rematch.py on the reference GPU model), no exemption for small pairs;
   tolerance from the measured GEMM error; self-test for the checker. Round 2: fp16-quantised
   permitted values with the exact float32 distance rule and tie handling, strict container
   validation, codex's counterexample and duplicate corruptions in the self-test.
2. run_tests.sh: `set -euo pipefail`, every step checked, the output deleted before the run
   and required after it; failure propagation checked (unreachable browser: exit 1).
3. geometry.bin in WSTV v1 with models (F/E/H, pose; pairs re-expressed in frame order if
   image ids were ever reversed); hand-off text aligned with CONTRACTS.md.
4. Tie policy and `ambiguous` reporting (pairs.js, worker), tests with synthetic orbits and
   the repeated-position case.
5. matcher.js: all allocations and pipeline setup inside the cleanup scope, balanced error
   scopes, one error classification (DeviceLost whenever the device is lost, setup included).
6. worker.js: per-run id on every message, overlapping runs rejected (`Busy`), cancel by id.
7. format.js: decoders validate everything; truncation/corruption tests in test_format.mjs.
Also: executable loop-var bug regression (lint + probe) and worker cancellation through real
messages.
