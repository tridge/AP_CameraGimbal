# Codex review of PLAN.md v1 (2026-11-08)

Reviewed by codex (read-only) against the repo, the native reference in $WEBSPLAT_DATA/ref and the research report. Its recommendations were folded into PLAN.md v2.

The thermal-orbit pipeline looks feasible, but the plan is not ready to implement unchanged. The biggest risks are thermal training semantics, the colmap-wasm integration, and recovery from browser memory/device failures. The existing Brush timing demonstrates that browser training can run; it does not yet demonstrate parity with `train.py`.

I read the requested files, the supporting implementations, and the saved native reference under `$WEBSPLAT_DATA/ref`. No files were modified.

Ranked recommendations:

1. **P0 — Prove Brush’s thermal data path before building the full pipeline.**

   [Stage 6](../../../tools/GaussianSplat/WebSplat/PLAN.md:97) treats 16-bit input and thermal loss as later adaptations. The inspected Brush implementation packs ground truth into RGBA8, converting other formats—including 16-bit images—to that representation. Simply supplying 16-bit PNGs will therefore lose precision. See [Brush’s image packing](https://github.com/ArthurBrussee/brush/blob/1388f74c6fe0236f68ee4915564bf00e9d2e3747/crates/brush-dataset/src/scene.rs).

   First demonstrate a worker-hosted Brush build accepting numeric scaled-temperature targets, applying the hot-weighted loss, evaluating held-out views, and exporting correct thermal values. Preserve float/half-float targets and test that no gamma conversion occurs. Keeping RGB-shaped rendering initially is reasonable; rewriting everything for scalar rendering is a separate optimization.

   Remove “or write our own WGSL trainer” as a routine fallback. Forward/backward rasterization, sorting, optimizer state, densification and numerical validation constitute another substantial project.

2. **P0 — Make the colmap-wasm fork an early feasibility gate; the prebuilt fallback does not provide the proposed interfaces.**

   [Stages 4–5 and the fallback](../../../tools/GaussianSplat/WebSplat/PLAN.md:83) assume separable verification and prior mapping. The inspected [C API]($WEBSPLAT_RESEARCH/colmap-wasm/native/entry/web.cc:98) instead hard-codes a guessed-focal `SIMPLE_PINHOLE` camera and combines verification, calibration and global mapping in `cw_map_run()`. Verification is not exposed independently for the proposed worker pool.

   The fork needs explicit camera parameters/model, telemetry poses and position covariance, separate verification input/output, prior-mapping orchestration, complete reconstruction export, and progress/cancellation boundaries. Its [browser build disables the filesystem]($WEBSPLAT_RESEARCH/colmap-wasm/native/entry/CMakeLists.txt:50), so copying Python’s file-oriented orchestration verbatim is insufficient.

   The `ba.c` fallback is particularly underqualified: its [measurements used final native tracks and final intrinsics]($WEBSPLAT_RESEARCH/REPORT.md:63). That validates optimization under favorable inputs, not reconstruction from noisy verified matches. It still needs track construction, conflict handling, triangulation, completion/merging and re-triangulation. Prove those from raw matches before calling it a fallback.

3. **P0 — Add an explicit scene-preparation stage between mapping and training.**

   Passing “posed frames” to Brush omits substantial native behavior:

   - Reject weak/misregistered views and reproduce hot/cold training selection and holdouts.
   - Undistort radiometric images with the refined camera.
   - Compute the global temperature scale from those images.
   - Filter sparse points spatially, build the canopy ceiling, and initialize their values from thermal observations.
   - Re-locate the hotspot using refined poses.
   - Configure scene-relative learning rates, thermal densification and spatial pruning.

   These operations live in [scene loading](../../../tools/GaussianSplat/train.py:112), [initialization](../../../tools/GaussianSplat/train.py:289), and [training setup/pruning](../../../tools/GaussianSplat/train.py:415).

   This stage must explicitly choose native-equivalent undistorted pinhole training or Brush’s distorted-camera path and validate the difference. Also replace SfM point colors: CLAHE intensities are not scaled temperatures.

4. **P1 — Do not substitute colmap-wasm’s matcher without changing its rules.**

   The two matchers are materially different. Native [matching](../../../tools/GaussianSplat/sfm.py:51) normalizes descriptors, applies an L2 ratio test in the forward direction, and checks reverse nearest-neighbor identity. The [upstream WebGPU matcher]($WEBSPLAT_RESEARCH/colmap-wasm/src/sift-matcher.ts:106) uses angle distances, a maximum-distance cutoff, and ratio acceptance in both directions.

   “Identical to COLMAP’s matcher” does not establish equivalence to this repository’s custom matcher. Port the native rule explicitly, including ties, empty descriptors and numerical tolerance. Validate matches using identical descriptor inputs before testing feature-extractor differences.

   Likewise, CPU VLFeat SIFT versus native GPU SIFT is an equivalence question, not a guarantee. Feature counts alone cannot answer it.

5. **P1 — Replace “check GPU memory” with explicit resource budgets and recovery.**

   [Stage 0 and the memory risk](../../../tools/GaussianSplat/WebSplat/PLAN.md:59) conflate total VRAM, available memory and WebGPU allocation limits. There is no portable free-VRAM query. Storage bindings default to 128 MiB and buffers to 256 MiB; higher limits must be requested when supported. See the [WebGPU limits and allocation model](https://gpuweb.github.io/gpuweb/#limits).

   Useful upper bounds for this dataset:

   | Data | Size |
   |---|---:|
   | 201 raw uint16 thermal images | 125.6 MiB |
   | Same images as float32 scalar targets | 251.3 MiB |
   | 201 × 8192 uint8 SIFT descriptors | 201 MiB |

   The upstream matcher uploads all descriptors into one buffer. It therefore needs either sufficiently large requested limits or descriptor batching.

   Budget JS arrays, each wasm heap, transfer copies, optimizer/gradient buffers, densification peaks, tile intersections, preview and export simultaneously. Limit CPU worker count by measured heap use. Release completed stages’ devices/heaps and cap preview resolution.

   “Keep frames on GPU only as needed” does not control Gaussian growth or rasterizer scratch memory. Require successful full runs on the actual 8 GB target, plus handled OOM and `device.lost` recovery.

6. **P1 — Strengthen acceptance criteria and resolve the run13 mismatch.**

   [Stage 6’s criterion](../../../tools/GaussianSplat/WebSplat/PLAN.md:103) defers the lobe while comparing with run13, which used `--lobe --source-gain --gain-scope scene`. Those options are recorded in [run13’s configuration]($WEBSPLAT_DATA/ref/splat_run13/scale.json:45).

   Either reproduce those capabilities or establish a separate native SH-only baseline for the first milestone.

   PSNR within 3 dB is insufficient for thermal acceptance. In the saved [run13 metrics]($WEBSPLAT_DATA/ref/splat_run13/metrics.json:85), one hot view scores 43.55 dB while predicting a 57.42°C peak as 29.63°C. Across its five hot holdouts, peak MAE is approximately **12.55°C**, despite a reported signed median error of −5.73°C.

   Freeze exact holdout filenames and preprocessing. Require separate hot/cold PSNR, absolute peak-error distribution, hotspot localization, hot-region overlap and false detections. State numeric tolerances before adaptation begins.

   Other acceptance fixes:

   - Ingest: require **raw pixel equality**, not just matching telemetry/timestamps.
   - Features: compare locations/descriptors or downstream geometry, not saturated feature counts.
   - Mapping: “201 registered” is weak because every pose starts registered. Check observations per image, track lengths, triangulation angles, camera-error tails and intrinsics.
   - At this camera’s focal length, 0.1° corresponds to roughly 2.6 pixels: median pose agreement alone cannot establish small-hotspot fidelity.

7. **P1 — Specify actual stage contracts and distinguish restart from training resume.**

   [The architecture](../../../tools/GaussianSplat/WebSplat/PLAN.md:34) promises interfaces but only names OPFS and `job.json`.

   Define versioned schemas covering frame identity/order, source timestamps, raw encoding/endian, per-frame radiometry, rotation, camera model, world-to-camera versus camera-to-world transforms, ENU datum, keypoint indices, matches/tracks, dataset splits and temperature scale. Include configuration/dependency hashes and downstream invalidation rules.

   OPFS stage outputs do not automatically provide resumable training. Brush’s inspected [periodic export path is excluded from wasm](https://github.com/ArthurBrussee/brush/blob/1388f74c6fe0236f68ee4915564bf00e9d2e3747/crates/brush-process/src/train_stream.rs). A PLY also omits Adam moments, refinement statistics, schedules and sampling state.

   Decide whether v1 promises exact continuation or restarting training from saved splat parameters. Test reload during checkpoint writes, quota exhaustion, stale versions and two tabs opening one job. Publish completed checkpoint generations only after writing their data, and request persistent storage while handling denial. [Storage persistence remains permission-dependent](https://storage.spec.whatwg.org/#persistence).

   Source-file recovery also needs a policy: retain a copy, persist an appropriate handle, or request reselection and verify its identity.

8. **P1 — Rework ingest/selection around the native multi-pass dependency and missing user inputs.**

   [Stages 1–2](../../../tools/GaussianSplat/WebSplat/PLAN.md:66) look sequential, but selection needs global information. Native extraction scans metadata, locates a hotspot, selects views, computes selected-frame temperature statistics, rejects drift, then generates retained images. See [the extraction flow](../../../tools/GaussianSplat/extract_frames.py:112).

   Choose between bounded repeated decoding and caching candidate frames in OPFS; include both costs in timing/storage estimates.

   Existing decoder reuse is promising: it already returns checked gray16 data. However, its [API decodes one requested frame at a time and cancels the previous request](../../../tools/viewer/src/media/software-decoder.js:275). It is designed for seeking, not batch extraction. Benchmark it before assuming a 30–60 second ingest; a bounded batch interface may be needed.

   The “choose an MKV” UX also lacks target latitude/longitude, fallback target altitude, rotation override and HFOV override. [Native extraction requires the target coordinates](../../../tools/GaussianSplat/extract_frames.py:79). Provide automatic proposals with a target/frame preview and editable overrides, plus explicit handling of absent or inconsistent telemetry.

9. **P2 — Move an export/viewer round trip earlier.**

   [Stage 7](../../../tools/GaussianSplat/WebSplat/PLAN.md:106) is more than serialization. Validate SH ordering/direction, quaternion layout, opacity/log-scale encoding, axes, units, temperature metadata and starting camera using a tiny known model before training integration.

   The existing [viewer is a page-level WebGL application](../../../tools/GaussianSplat/viewer/src/viewer.js:101), not an embeddable component. Choose an embedding boundary early and account for simultaneous WebGPU/WebGL resources.

   SPZ encoding can reuse [Spark’s existing encoder](../../../tools/GaussianSplat/viewer/ply2spz.mjs:1), but thermal scale metadata needs a defined delivery mechanism. Evaluate training output, exported PLY and SPZ separately: native lobe projection and SPZ quantization both introduce additional error.

10. **P2 — Treat timings as hypotheses; simplify the first ETA implementation.**

    The [research report explicitly says colmap-wasm was not run]($WEBSPLAT_RESEARCH/REPORT.md:35). Its SIFT timings are estimates, desktop matcher timings are extrapolated, and prototype BA timing excludes discovering tracks from matches. Carry those labels into the plan.

    Use **17.5 minutes as the measured desktop browser training baseline** for the tested configuration. The 19-minute laptop result is native Brush, so laptop browser time remains unmeasured. Neither result measures the proposed thermal adaptations, checkpointing or final export.

    For UX, show indexing bytes before frame totals exist; frames/pairs afterward; BA rounds and iterations with uncertain remaining work; and training iterations plus Gaussian count. Start with a broad overall range, then refine from completed GPU work. Separate shader compilation, loading, evaluation and checkpoint time from training throughput.

    Delay extensive per-kernel startup benchmarking until real pipeline measurements show it improves estimates. Add “paused”, “interrupted” and “checkpointing” states. Screen Wake Lock is released when the page becomes hidden; it is not a background-execution guarantee. [Wake Lock specification](https://www.w3.org/TR/screen-wake-lock/#handling-document-loss-of-visibility)

I would change the implementation order to: **freeze reference artifacts/contracts → prove thermal Brush adaptation and colmap-wasm mapping independently → validate a thermal export round trip → implement ingest/selection and scene preparation → integrate → add robust recovery and calibrated ETAs**.

Keep the first release scoped to thermal orbits on the two specified Chrome/dGPU machines. Large RGB datasets, CPU fallback, GPU SIFT and a custom trainer should each require a separate justification. The research’s 3–5 day fork and 1–2 week integration estimates do not cover all the thermal adaptation and recovery work identified above; a credible completion estimate should follow the two feasibility gates.
