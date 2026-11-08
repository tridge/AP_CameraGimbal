# Stages 1-2: ingest and frame selection

2026-11-08. Code: `app/src/stages/ingest/`, `app/src/stages/select/`. Tests: `test/stage1/`,
`test/stage2/`. Bulk test data under `$WEBSPLAT_DATA/stage12/` (native dumps, native
extract_frames.py runs per option set, port outputs, Chrome profile under
`$WEBSPLAT_DATA/chrome-profiles/`).

## Round 1 review fixes (REVIEW-stage12.md)

1. Fallback proposal (no usable hot spot) sets `target_alt` to the optical-axis point's height;
   the complete proposal is passed into selection in `proposal-no-hot` / `proposal-failed-hot`
   (201 frames, equal to native with the same options; with height 0 it kept 77).
2. `checkTelemetry()` validates numeric fields where native reads them (takeoff altitude over all
   frames; poses, `maximum_c`, radiometry over start..end) and rejects with the fields and frame
   indices; absent optional gimbal keys keep native's 0 default. Cases run against native with
   the same metadata patch (`native_injected.py`): null yaw, null AMSL, string latitude, null
   `maximum_c` fail on both sides; missing gimbal pitch, empty gimbal record and bad data outside
   start..end succeed identically.
3. The select worker verifies the ingest index (`websplat.ingest` v1), source size and `sha256c`,
   then packet layout and pts (`verifySource()`); a same-size file with one changed byte is
   rejected in Chrome.
4. Hot samples keep only the blank flag and the hottest pixel for each of the four rotations
   (`hottestAll()`, equal to argmax of the rotated frame including ties, 300 random cases).
5. `planBatches()` bounds are hard and checked before reading: `batchFrames` for keyframe-only
   batches, `maxChainFrames` (64) for batches holding a dependency chain, `batchBytes` and the
   decoded size; a longer chain is rejected (`BatchLimitError`). Planner tests (fixed + 2000
   random layouts) and a real GOP-8 FFV1 fixture (`make_gop_fixture.py`, non-key frames that
   fail to decode alone) decoded in Chrome with small limits equal PyAV's in-order decode.
6. Rotation trials and the optical-axis estimate each fail independently; proposals use a
   conditioning check (`rcond`) and reject non-finite results; parallel optical axes keep a valid
   hot spot (synthetic test). Selection keeps native's semantics (fails only on an exactly
   singular system, like `np.linalg.solve`).
7. Ingest is keyed on `config.proposal` (`PROPOSAL_KEYS`: start, end, hotspot_min_c,
   hotspot_max_error, hfov) and only those reach `propose()`; the proposal reports the effective
   HFOV and records its options.
8. Output handles are acquired inside the protected scope and closed independently; on any
   failure the stage removes its partial outputs (frames.json is written last) and the cache;
   cancellation terminates running FFmpeg workers. Tested in Node (quota mid-write in all modes,
   decoder failure, cancellation, a throwing close: no open handles, no files, store reusable)
   and in Chrome (test write fault as QuotaExceededError, cancellation, forced termination with
   no grace period while FFmpeg runs; then a rerun into the same OPFS directory equals native).
   Unpublished generation directories after a crash remain the controller's cleanup (stage 0).
9. Suite: blank hot frames (proposal and selection, against native with the same injection),
   CLAHE fixtures generated in `test_clahe.mjs` (34 cases incl. 1x1, 8x3, 3x8, 640x7, 5x512),
   native caches keyed by input hash, native code revision, options and injection, stage 1
   Chrome test requires exactly one hash per frame, no-hot/failed-hot proposals, incomplete
   telemetry, degenerate rays.

## Round 2 review fixes

- Cleanup covers the whole run through the frames.json write; a failed `createWritable` is
  aborted. Tests: frames.json write failure (single, cache), no usable frames.
- Validation follows the executed path: `maximum_c` only when locating the hot spot, radiometry
  only for frames actually decoded (`checkDecoded`), size from frame 0; native-equal cases for
  null `maximum_c` without hot spot search, null scale on an unselected and a selected frame.
- The proposal's provisional origin is the first validated pose in start..end; a bad or null
  frame 0 latitude with start 17 leaves the proposal unchanged.
- A frame larger than the decoded-output limit is rejected at planning.

## Acceptance

| check | result |
|---|---|
| 1: metadata, timestamps, pixels, every frame (Node index; Chrome stage worker + FFmpeg-wasm) | 201/201 identical to `splat_common` (PyAV): meta deep-equal, pts exact, sha256 of gray16le equal |
| 1: source identity `sha256c` | Node = Chrome = independent Python |
| 2: reference options (lat -35.5802022, lon 148.9065438, rotation 0) vs `$WEBSPLAT_DATA/ref/work` | same 201 frames, hot spot 4e-14 m, positions identical, rotations within 2.2e-16, CLAHE and raw frames pixel-identical, temperatures equal |
| 2: option variants, fallback proposals, injected blank frames and incomplete telemetry vs native with the same options/injection (Node, native-decoded frames; 34 cases) | all pass: drift rejection (3 modes), step/start/end, margin, max range, target alt, no hot spot, recorded rotation 180, rotation 90 and 270 with HFOV, hot spot rejected (strict error, too far), radius mode, failures with native's message |
| 2: blank frames (injected; the orbit has none) | removed and listed as native would |
| 2: NumPy statistics (12 synthetic + real frames, incl. the 0.5 C blank threshold, odd sizes, float scale/offset) | bit-exact |
| 2: CLAHE vs cv2 5.0 (34 cases: real, rotated, random, smooth, flat, tiny, one/no dimension divisible by 8) | bit-exact; no tolerance needed |
| 2 in Chrome: stage worker, three decode modes and drift compaction in OPFS | identical to native |
| 2 in Chrome: changed source, quota failure, cancellation, forced termination, rerun | rejected/cleaned up; rerun identical to native |

Run (each self-contained; runs the native side if its output is missing):
`node test/stage1/test_planner.mjs`, `node test/stage1/test_ingest.mjs`,
`node test/stage1/test_gop_chrome.mjs`, `node test/stage2/test_clahe.mjs`,
`node test/stage2/test_numeric.mjs`, `node test/stage2/test_robust.mjs`,
`node test/stage2/test_select_node.mjs` (37 cases), `node test/stage2/test_select_chrome.mjs`,
`node test/stage1/bench.mjs` (decoder benchmark). Python: `tools/GaussianSplat/.venv`.

## Timings (laptop, headless Chrome)

The machine was under heavy load from parallel Rust builds (load 6-10) for most runs; quiet
numbers in brackets.

- Index 32 MB MKV: 60-270 ms (Node 30-110 ms). Source `sha256c`: about 50 ms.
- FFV1 decode, 640x512 gray16: viewer `SoftwareDecoder` (one frame per request) 29 fps; batch
  decoder 48 fps with 1 worker, 85 with 2, 100 with 3 (16-frame batches), 112 with 4 x 8; about
  19 ms per frame in a worker. Default 3 workers: 105 fps quiet, 50 fps loaded.
- Ingest stage total (index, hash, proposal incl. wasm load and 15 hot frames): 0.8-1.0 s.
- Select, 201 frames: single 4.4 s [3.3], redecode 6.1 s [5.0], cache 4.3 s [3.1] (+131 MB
  temporary OPFS). Statistics + CLAHE about 7 ms per frame. OPFS writes of 198 MB about 70 ms
  with sync access handles (5 s with a `createWritable` per file, abandoned).
- Native extract_frames.py on the same input: 6.5 s.

## Decisions

- Batch decoder (`ffv1-batch.worker.js` + `frame-decoder.js`): the viewer's single-frame decoder
  is 3.6x slower than a 3-worker batch pool. Each batch becomes one small Matroska (the viewer's
  `createMiniMatroska`) decoded by one FFmpeg exec; every output frame is checked against its
  packet's timestamp (showinfo) and the batch size. The worker is a sibling of the viewer's
  (same pinned core and load code), not a change to it. Dependency chains longer than the
  bounds are rejected rather than split (FFmpeg exec cannot carry decoder state between
  batches); APCG FFV1 frames are all keyframes.
- Decode strategy: **single pass** (default). Drift rejection is only a filter, so outputs for
  every non-blank selected frame are written provisionally and drifted frames' slots compacted
  away afterwards: one decode, no extra storage. Redecode (native's flow) and an OPFS raw cache
  are kept as options (`config.selectMode`) and produce identical outputs.
- Proposals (`propose()`, written by the ingest stage to `proposal.json`): rotation = the one of
  0/90/180/270 whose hot-pixel rays triangulate most consistently, keeping the recorded rotation
  unless another is better by 1.5x (orbit: 0 at 10 px vs the recorded 180 at 29 px, as the
  reference used); target lat/lon = the hot spot triangulated in a provisional frame; target
  altitude unset (= hot spot); HFOV from metadata. Without hot frames: where the optical axes
  converge. Notes explain each choice and telemetry gaps.
- NumPy semantics are emulated exactly (float32 Celsius, percentile dtype rules, medians) from a
  raw-value histogram: Celsius is monotonic in the raw value.

## Deviations from native

- `source` is the file name; frames.json adds `format`, `version`, `selection` (plus
  `selection.mode`). Pixels are `features.u8`/`thermal.u16` `[N, H, W]` (CONTRACTS.md), not PNGs.
- The proposal (lat -35.5799111, lon 148.9066524) differs from the reference's inputs; it gives
  the same target within 0.2 mm, but poses differ from `ref/work` by up to 1.8e-4 m because
  native's re-centring depends slightly on the given position. Exact with the reference inputs.
- All frames blank or drifted: a clear error (native crashes in `np.max([])`).
- Log wording: "check rotation" instead of "check --rotation".

## Proposed for stage 0 / CONTRACTS.md

- The controller's `ctx` needs the source `File` (`ctx.sourceFile`); the app must serve the
  viewer's FFmpeg core at `vendor/ffmpeg/<version>/` (as the viewer's `prepare-assets.mjs` does)
  and allow imports from `tools/viewer/src` (vite `server.fs.allow` in dev).
- Register `ingestStage` (`stages/ingest/stage.js`) and `selectStage` (`stages/select/stage.js`);
  selection options are `config.select` (CONTRACTS names), filled by the UI from `proposal.json`.
- Add `ingest/proposal.json` (`websplat.proposal` v1) and `selection.mode` to CONTRACTS.md, and
  define `selection.considered` as `end - start + 1`.
- The UI keeps `config.proposal` equal to the corresponding `config.select` settings
  (`PROPOSAL_KEYS`), so ingest re-runs only when those change.
- On a stage failure the select worker deletes its partial files; a crash or forced termination
  can still leave files in an unpublished `gen-*`, which the controller deletes at startup and
  should also delete before a retry.
- The select worker re-indexes the MKV (60-270 ms) because the index lacks the codec private
  data the decoder needs; adding `codecPrivate` to `track` would allow skipping it.

## Open issues

- Not run on win11. Timings mostly under load; to be repeated quiet.
- The proposal's 15 hot frames are decoded again by the select stage (about 0.5 s), and the
  select stage hashes the source again to verify it (about 50 ms for 32 MB, linear in size).
- The viewer's media code is imported by relative paths across the repo (no package boundary).
