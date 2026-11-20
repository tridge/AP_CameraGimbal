# WebSplat progress

Plan: PLAN.md. Reviews: REVIEW-*.md. Newest entries at the top of each section.

## Status

| stage | state | notes |
|---|---|---|
| plan | v2 done | v1 reviewed by codex (REVIEW-plan.md), v2 written 2026-11-08 |
| 0 reference, contracts, skeleton, harness | accepted (codex round 3), committed | reference reproduced byte-identical; CONTRACTS.md v1.3; dummy pipeline 30/30 checks on laptop NVIDIA, Intel and win11; crash-safe manifest and checkpoints; notes/stage0.md, REVIEW-stage0.md |
| G1 thermal training in Brush (gate) | passed under the repeat policy, accepted (codex round 3), committed | Brush + CubeCL forks: float targets, hot-weighted L1, frozen holdouts, apcg_splat PLY, buffer lifetime fix; runs with any GPU error are invalid; repeat policy (5 valid runs, each criterion in >= 4/5 with median in bound) registered before the runs: laptop Chrome PASS (criterion 3 4/5, median 12.70 C vs 14.51), about 14 min, 1.7-2.3 GB GPU (one 4.3 GB peak); win11 Chrome series 4 PASS (criterion 3 4/5, median 12.41 C), 7.7-8.1 min; win11 series 3 failed (no WebGPU adapter at launch, recorded); notes/g1.md, REVIEW-g1.md |
| G2 telemetry-seeded mapping in wasm (gate) | passed, accepted (codex round 3), committed | colmap-wasm fork: verification matches native seed 0 on 2698 pairs (E not reproduced in 32 pairs, equal-consensus solutions); mapping 0.020 m / 0.009 deg median from native geometry, 0.132 m / 0.047 deg from browser matching of native features; Node, laptop and win11 Chrome byte-identical; verification 70 s laptop / 36 s win11, mapping 74 s / 70 s; notes/g2.md, REVIEW-g2.md |
| 1-2 ingest, frame selection | accepted (codex round 3), committed | all acceptance checks pass vs native (raw pixels, poses, CLAHE pixel-identical, option variants, blank/incomplete telemetry, storage faults); run through the page on laptop and win11; notes/stage12.md, REVIEW-stage12.md |
| 3 features + 3-5 integration | accepted (codex round 3), committed | fork SIFT exact vs native CPU SIFT (201/201 counts, keypoints within 0.01 px); browser chain features -> WebGPU match -> verify -> map: 106%/110% of native verified pairs/inliers, 0.250 m / 0.081 deg median (closes G2); features, match, map run as page stages with per-round map checkpoints (resume byte-identical) and Stop; pipeline to sparse model 207 s laptop / 175 s win11; notes/stage3.md |
| 4 pairs and matching | accepted (codex round 3), committed | pairs identical; matches agree with native within the fp16 grid (35 of 510,656 at decision boundaries, all feasible within 0.51 ulp); 27.5 s laptop NVIDIA, 7.9 s win11, 317 s laptop Intel; verification waits for G2; notes/stage4.md, REVIEW-stage4.md |
| 5 mapping | integrated with stage 3 (above) | |
| 5b scene preparation | accepted (codex round 3), committed | `prepare` stage after map: train.py's scene loading bit-exact vs train.py's own (dump_scene.py) on the native and the browser model and 6 option variants: same views/frozen holdouts, identical undistorted Celsius and f16 targets, scale, bounds, ceiling, initial points, lr/densification settings, hot spot within 1e-5 m; in the page 8.1 s laptop / 3.9 s win11; Brush hand-off proposals in notes/stage5b.md |
| 6 training | accepted (codex round 3), committed | G1's trainer as a page stage from 5b's dataset with train.py's rules: SH degree schedule (render-sliced), bounds pruning every 100 to the end, refinement only 500 < step < 15000, Adam state kept across pruning, checkpoints every 5000 with Stop/resume (3D-filter floor restored); series 4 (pre-registered 2026-11-09T00:19:31Z, same rule/baselines/bounds as registered 2026-11-08) PASS: laptop every criterion 5/5, win11 hot IoU 4/5 and the rest 5/5; training 738-757 s laptop, 638-767 s win11 (its GPU was shared with splat training in its WSL VM); series 2 and 3 superseded by review findings (recorded); found gsplat 1.5.3's opacity reset never fires; notes/stage6.md, REVIEW-stage6.md |
| 7a export round trip | accepted (codex round 3), committed | tiny known model (10 Gaussians, SH3): PLY, SPZ, meta.json byte-identical to native; quaternion, opacity, scale, SH, axes checked against gsplat renders with negative controls; SPZ error <= 0.56 quantisation steps; viewer embedded via postMessage, readouts within 0.42 C; run13 exports in 4.9 s; open: viewer antialiasing lowers hot peaks; notes/stage7a.md |
| 7 result | accepted (codex round 6), committed | export stage streams PLY (64k chunks) and SPZ (131k chunks, byte-identical to whole-file Spark) to OPFS with Stop/resume; result view with embedded viewer, downloads and summary; laptop page MKV to downloads 184 s (training stand-in), export 6.9 s; 2M Gaussians in 22-26 s at +422-548 MiB; open: SPZ clamps Brush hot sources (up to 24.5 C), start camera barely shows the fire; notes/stage7.md |
| 8 integration, recovery, release | waiting | |

## Shared data (not in git)

`$WEBSPLAT_DATA/`
- `input/orbit.mkv`: canonical input, `2025-12-18_log80_thermal_orbit_1202-1207UTC.mkv` (31.8 MB,
  a cut of log80_thermal.mkv covering 12:02-12:07 UTC; FFV1 16-bit + per-frame telemetry).
- `ref/work/`: native `extract_frames.py` output for it (frames.json, images/ CLAHE PNGs,
  thermal/ raw 16-bit, colmap/ database.db, pairs.txt, sparse/{telemetry,work,final}).
- `ref/extract.log`, `ref/sfm_prior.log`, `ref/train_run13.log`: native logs. Extraction
  command (recovered, reproduced byte-identical): `extract_frames.py MKV WORK --lat -35.5802022
  --lon 148.9065438 --rotation 0`. Full record: `ref/REFERENCE.md`, `ref/metrics_tables.md`.
- `ref/reproduce/`: native reruns. `ref/baseline_sh/`, `ref/baseline_sh_rep2/`: two SH-only
  train.py baselines (run13's options without lobe/source gain; same 13 frozen holdouts). Hot
  views PSNR 37.88 dB, abs peak error median 12.5 C / max 33.8 C, IoU 0.29; cold PSNR 41.07 dB.
- `ref/splat_run13/`: train.py output (splat.pt, scale.json with all options, metrics.json,
  preview images). Native timings on the desktop (RTX 4070): features 4 s, matching 17 s,
  verification 13 s, mapping 34 s, training 525 s (30k iterations, 591k Gaussians).
- Research: `$WEBSPLAT_RESEARCH/REPORT.md` and its prototypes.

## Measurements

Brush (WebGPU trainer, github.com/ArthurBrussee/brush main 2026-10) on the orbit, 30k
iterations, as a training-speed reference:
- Chrome on win11 RTX 4070: about 17.5 min (headless, timed from GPU activity).
- native (Vulkan) on the laptop RTX PRO 1000: 19.2 min, 1.02M Gaussians.
- Chrome on the laptop RTX PRO 1000: does not train (see open issues).
- Randy RGB set (977 x 1080p): native laptop 45 min (1.97M Gaussians); Chrome on win11 fought
  the 12 GB limit for 49 min at 1920 px then lost the device; at 1280 px it panicked after
  3 min. Large RGB sets are out of scope for release 1.

WebSplat full runs through the page (stage 8, notes/stage8.md; the orbit MKV, 201 frames, 30k
iterations; per-stage medians, seconds):

| | ingest | select | features | match | map | prepare | train | export | total |
|---|---|---|---|---|---|---|---|---|---|
| laptop (RTX PRO 1000, runs 3-6) | 0.6 | 3.5 | 17.3 | 65 | 72 | 5.9 | 741 | 9.7 | 923 |
| win11 (RTX 4070-class, runs 1-3, GPU idle) | 1.2 | 6.1 | 21.2 | 45 | 76 | 3.9 | 421 | 12.4 | 605 |

The page's estimates are calibrated from these (train/stage.js `SECONDS_30K`, cost curve; map 19.5 s
per round); estimates v3 pass on the laptop and miss on win11 by one early training sample.

## Decisions

- 2026-11-08: training targets are float16 as train.py trains on (stage 5b output); stage 6 builds Brush's init.ply from 5b's points (kNN scales, random quaternions). G1's float32-based PNG transport is a gate-only path.

## Open issues

- Stage 4 follow-ups (non-blocking, codex round 3): compare_matches.py should check byte-length
  divisibility before np.fromfile, and restrict or generalise --ulps beyond the configured 0.51.

- Chrome 155 on Linux (Vulkan, both laptop adapters): a WGSL `var` declared without an
  initializer inside a loop is not re-zeroed each iteration (win11 D3D12 is correct). All
  WebSplat WGSL (including the Brush fork) must initialise explicitly. Repro in notes/stage4.md.
  Buffer-based compute on the laptop NVIDIA GPU otherwise works (stage 4 matcher); Brush's
  failure there looks texture-specific.

- Emscripten: the system package (emcc 3.1.69, /usr/bin) is installed and used; there is no
  emsdk checkout, so colmap-wasm's build script is adapted to the system toolchain.

- Laptop NVIDIA in Chrome (Vulkan): presenting a WebGPU canvas fails (Dawn's Vulkan memory import)
  and repeated failures lose the device; compute and texture readback work. WebSplat trains
  without a canvas and previews by readback; repro in test/g1/probe/. Stock Brush's "every
  pipeline invalid" was separate: generated WGSL used subgroup builtins without
  `enable subgroups;` (the worker adds it).