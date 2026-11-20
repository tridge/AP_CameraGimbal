# Codex review: stage 7a export/viewer round trip

## Round 1

**Verdict: accept after fixes.** No P1 finding. The serialization conventions check out, but the error accounting and acceptance tests need tightening.

1. **P2 — Thermal acceptance uses a different rendering mode from training.** [test/stage7a/check_native.py:107] selects gsplat `antialiased`; comparison against native `classic` only prints the discrepancy. Consequently, acceptance passes despite the thin-disc readout being 42.6 °C versus 49.44 °C.

   The proposed `preBlurAmount: 0.3, blurAmount: 0` is correct **for classic-trained splats**. Both settings produce covariance `Σ + 0.3I`; the proposed settings remove the opacity multiplier `sqrt(det(Σ) / det(Σ + 0.3I))`. In a generated viewer copy, the disc readout became **49.4 °C**, with maximum centre-readout error about **0.25 °C** against the cached classic reference. This agrees with [Spark’s documented semantics](https://sparkjs.dev/docs/spark-renderer/).

   Applying it globally would also make existing splats denser/brighter and change overlap and occlusion, particularly for thin or distant Gaussians. Select the rendering mode explicitly for known classic-trained exports and preserve compatibility for other assets. Also address [app/src/stages/export/spz.js:10]: the produced SPZ sets the antialiased flag, which tells other viewers to apply mip-splat antialiasing. Changing this viewer alone leaves that mismatch. The [SPZ flag definition](https://github.com/nianticlabs/spz/blob/main/src/cc/load-spz.h) confirms its meaning. This fix does not remove lobe-projection or SPZ-quantisation error; I could not freshly reproduce the reported run13 peak without CUDA.

2. **P2 — SPZ error accounting can certify missing data.** [app/src/stages/export/spz.js:109] compares only `min(sourceCoefficients, decodedCoefficients)`. I removed every higher-order SH coefficient from the actual tiny SPZ and changed its header to degree 0. Decoding succeeded, and comparison against the degree-3 source returned `over: {}`, with both SH errors zero. Non-finite decoded arrays likewise produce no `over` failures through [spz.js:117]. Assert the expected degree, array lengths and valid numerical values before comparison; add rejecting controls. Clamped fields should also be checked against their expected saturated values instead of excluded entirely.

3. **P2 — The runner can test stale viewer code.** [test/stage7a/run_tests.sh:28] rebuilds only when `dist/viewer.js` lacks the hook’s name. Subsequent broken origin checks or removal of the race fix can therefore leave the tests exercising an older, correct bundle. Build the viewer unconditionally into the test output, or verify its source fingerprint.

4. **P2 — The export API needs restructuring for stage 7 streaming.** [app/src/stages/export/ply.js:140] allocates the whole PLY body, then copies it into another whole-file allocation. [export.js:56] also creates full greyscale SH arrays, and the worker returns complete files. A highly compressible 2M-Gaussian synthetic export succeeded but peaked at **1.84 GiB RSS**, before worker-input and viewer copies. Add chunked PLY output to a worker-owned OPFS sink, calculate greyscale/quaternions per chunk, and sequence SPZ work to release intermediates. **This can remain an explicit stage 7 deferral.**

5. **P3 — Missing temperature readouts silently reduce coverage.** [test/stage7a/compare_views.py:72] uses `zip` without checking lengths. Removing the tenth temperature from every case still yielded “view checks: all pass”. Require the complete case set and exactly one readout per expected centre.

The positive checks are strong: tiny-model PLY, SPZ and metadata match native bytes when supplied the native camera; its independently calculated camera differs by only **4.32 µm**. Property order, channel-major SH, direction, `w,x,y,z` quaternions, logits, log scales, ENU axes/metres and temperature metadata agree. The separate run13 check passed, including geometry/opacity equality and bounded SH/quaternion differences.

The message hook checks both origin and parent source. Browser probes confirmed rejection of a cross-origin parent and same-origin sibling, and acceptance of the same-origin parent. File-picker, existing site-page and `?url=` routes passed; the thermal setup guard addresses the first-load race.

The supplied runner stopped at CUDA rendering because no GPU is exposed here. Node/native checks and the separate run13 check passed; software-WebGL browser checks passed against cached gsplat references, including the existing negative controls. **No tracked files were modified.**

## Round 2

**Verdict: accept after fixes.** One remaining P2 finding; no P1 findings.

- **P2 — Valid metadata can break native SPZ/site export.** [viewer/ply2spz.mjs:20] searches for the substring `end_header`, including inside JSON comments. A title or description containing that text truncates the metadata before `JSON.parse`. I reproduced `export_web.py … --title 'test end_header title'` failing with `SyntaxError: Unterminated string`. Match the complete PLY terminator line and add a regression case. The viewer’s existing [viewer.js:85] has the same delimiter weakness and should use the same correction.

All five round-1 items are otherwise addressed:

- **Rendering:** classic metadata and cleared SPZ flags are consistent across JS and native exports. Current-source browser tests give **49.4 °C** for classic versus **42.6 °C** for unmarked metadata; both reject the opposite reference.
- **SPZ accounting:** the earlier missing-band, NaN, wrong-length and wrong-saturation probes now fail.
- **Fresh builds:** the runner builds and serves the current viewer source unconditionally.
- **Streaming:** its design and required stage-7 work are explicitly deferred in the notes.
- **Coverage:** truncated readouts now produce seven failures; removing a case also fails.

The native writer changes correctly describe the trainers’ existing classic rasterisation. Old PLYs and metadata without the field loaded through file-picker, `?url=` PLY/SPZ and site-page routes without browser errors. Native CPU PLY loading also passed. `view_splat.py` reads checkpoints and `scale.json`; `splat_site.py` discovers those directories and launches it. Their input schemas remain unchanged. Origin/source rejection probes still pass.

Validation: the unmodified runner encountered Vite’s read-only config-cache directory. Retrying with the native config loader passed Node/native comparisons, then stopped because CUDA is unavailable. Separate run13 comparisons passed against the cached native reference; software-WebGL browser comparisons passed against cached gsplat renders.

**No tracked files were modified.**

## Round 3

**Verdict: accept.** The round-2 P2 is fixed; no new P1/P2 findings.

One **P3, pre-existing and non-blocking** limitation remains: [viewer/ply2spz.mjs:14] delegates to Spark, which rejects CRLF PLY headers. Direct viewer loading also fails for these files. I confirmed the unchanged HEAD converter fails identically. The JS PLY reader supports CRLF correctly, and canonical exports use LF, so this does not block stage 7a.

Verified:

- The exact `export_web.py --title 'test end_header title'` probe now succeeds.
- The multiline `tiny model end_header\nend_header` title passes native/JS exports and all seven viewer cases.
- Complete terminator-line matching preserves body offsets; the CRLF JS-reader case preserves every property and metadata field.
- Current-source browser screenshots, temperatures and negative controls pass against cached gsplat references.
- Vite’s native config loader builds successfully without the earlier read-only-cache failure.
- Separate run13 comparisons pass.

The supplied runner passed Node/native checks, then stopped because CUDA is unavailable. Browser checks used software WebGL and cached native renders.

**No tracked files were modified.**
