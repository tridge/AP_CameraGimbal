# WebSplat progress

Plan: PLAN.md. Reviews: REVIEW-*.md. Newest entries at the top of each section.

## Status

| stage | state | notes |
|---|---|---|
| plan | v2 done | v1 reviewed by codex (REVIEW-plan.md), v2 written 2026-11-08 |
| 0 reference, contracts, skeleton, harness | implemented, codex review running | reference reproduced byte-identical; CONTRACTS.md v1.1; app + harness pass 22 checks on laptop (NVIDIA, Intel) and win11; stages 1-2 run through the page; notes/stage0.md |
| G1 thermal training in Brush (gate) | agent working | started 2026-11-08 |
| G2 telemetry-seeded mapping in wasm (gate) | blocked | needs emsdk (see open issues) |
| 1-2 ingest, frame selection | implemented, codex review running | all acceptance checks pass vs native (raw pixels, poses, CLAHE pixel-identical, 21 option variants); notes/stage12.md |
| 3 features | waiting | needs G2's colmap-wasm fork |
| 4 pairs and matching | accepted (codex round 3), committed | pairs identical; matches agree with native within the fp16 grid (35 of 510,656 at decision boundaries, all feasible within 0.51 ulp); 27.5 s laptop NVIDIA, 7.9 s win11, 317 s laptop Intel; verification waits for G2; notes/stage4.md, REVIEW-stage4.md |
| 5 mapping, 5b scene preparation | waiting | |
| 6 training | waiting | after G1 |
| 7a export round trip, 7 result | waiting | |
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

## Decisions

## Open issues

- Stage 4 follow-ups (non-blocking, codex round 3): compare_matches.py should check byte-length
  divisibility before np.fromfile, and restrict or generalise --ulps beyond the configured 0.51.

- Chrome 155 on Linux (Vulkan, both laptop adapters): a WGSL `var` declared without an
  initializer inside a loop is not re-zeroed each iteration (win11 D3D12 is correct). All
  WebSplat WGSL (including the Brush fork) must initialise explicitly. Repro in notes/stage4.md.
  Buffer-based compute on the laptop NVIDIA GPU otherwise works (stage 4 matcher); Brush's
  failure there looks texture-specific.

- emsdk is not installed and installing it (download + run of the external toolchain) was
  blocked by the session's safety check on 2026-11-08. Tridge to install it (e.g. into
  $WEBSPLAT_DATA/emsdk) if the colmap-wasm prior-mapper fork is wanted; until then stage
  5 uses the ba.c route and stages 3-4 use colmap-wasm's prebuilt npm package where possible.

- WebGPU on the laptop's NVIDIA GPU in Chrome 155 (driver 595.99, Vulkan) is not usable for
  Brush yet: with a 1600x1000 viewport, VK_ERROR_OUT_OF_DEVICE_MEMORY on an empty 8 GB GPU;
  at 1280x720, Dawn rejects a texture allocation ("Requested allocation size (3768320) is
  smaller than the image requires (3932160)"), after which every pipeline is invalid and
  nothing trains. Looks like a Dawn/NVIDIA-driver bug (row pitch or alignment). The Intel GPU
  works in Chrome but is slow. To investigate in stage 0/G1: a minimal WebGPU reproduction,
  Dawn toggles, Chrome Canary, other driver versions; release 1 may need the Intel GPU or a
  workaround on this laptop.
