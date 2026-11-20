# Codex review of stages 1-2 (ingest, frame selection), round 1, 2026-11-08

**The canonical extraction path is well supported, but I would fix the proposal and input-validation bugs before accepting stages 1–2.** I found no mismatch in the saved canonical pixels, CLAHE images, rotations, ENU positions, or drift-compaction results.

Ranked findings:

1. **P1 — The fallback target proposal discards its estimated altitude.**  
   [select.js:232](app/src/stages/select/select.js:232), [select.js:271](app/src/stages/select/select.js:271)

   When hotspot triangulation is unavailable, `propose()` uses the optical-axis intersection for latitude/longitude but leaves `target_alt: null`. Selection consequently uses height zero above takeoff.

   **Reproduced:** disabling hot-frame eligibility on the canonical metadata gives an optical-axis target height of **44.21 m**. Selecting with the returned proposal keeps **77 frames**; preserving that height keeps **201**.

   Set `target_alt` to the fallback intersection’s ENU height. Add an acceptance test that passes the complete proposal into selection without a hotspot.

2. **P1 — Incomplete telemetry can silently become plausible but incorrect geometry.**  
   [select.js:88](app/src/stages/select/select.js:88), [select.js:136](app/src/stages/select/select.js:136), [geometry.js:80](app/src/stages/select/geometry.js:80)

   Object-presence checks do not validate their numeric fields. JavaScript coercion then differs materially from Python:

   - `vehicle_attitude.yaw_rad: null` produces a finite pose using zero vehicle yaw.
   - An AMSL value of `null` participates in `homeAltitude()` as zero.

   **Reproduced:** one valid frame with takeoff altitude 90 m plus one frame with null AMSL yields a takeoff median of **40 m**. Native code would raise rather than silently generate that datum.

   Validate required coordinates, altitude and heading as finite numbers before geometry/statistics. Explicitly skip or reject incomplete records and report their indices. Preserve native defaults only for genuinely optional fields.

3. **P1 — Stage 2’s source-match check does not establish source identity.**  
   [select.worker.js:22](app/src/stages/select/select.worker.js:22)

   The check compares file size, packet count, offsets and packet sizes, but never compares the stored source hash. A same-size recording with changed metadata or compressed payload can pass and produce stage-2 outputs attributed to the old ingest input.

   Require a size/hash-verified source binding before selection, either explicitly supplied by the controller or checked here. Also validate the ingest format/version. Packet-layout agreement is a useful additional check, but cannot replace identity verification.

4. **P2 — Hotspot sampling retains every decoded hot frame.**  
   [select.js:157](app/src/stages/select/select.js:157)

   Each `samples` entry stores a `hottest()` closure capturing the complete `Uint16Array`. Memory therefore grows with all eligible hot frames, despite the batch decoder’s bounded queue. The canonical recording has only 15 such frames; lowering the threshold or processing a hotter orbit exposes the larger footprint.

   Store the four hottest-pixel coordinates and blank flag immediately, then release the raw frame. That is all subsequent triangulation needs.

5. **P2 — Non-keyframe dependency chains bypass the advertised batch limits.**  
   [frame-decoder.js:29](app/src/stages/ingest/frame-decoder.js:29)

   On overflow, `planBatches()` creates a new batch containing the entire keyframe-to-requested-frame chain, without rechecking its size. Consequently `batchFrames` and `batchBytes` are not hard bounds.

   **Reproduced:** requesting frame 199 from a 200-frame GOP creates a **200-packet batch** with the default 16-frame setting. Large chains are rejected only later, after reading/building the compressed group, or can consume much more memory than expected.

   Either preserve decoder state across bounded chunks or reject oversized dependency chains before reading their payloads. Add both planner tests and an actual non-keyframe FFV1 decoding fixture. The notes correctly acknowledge that this path remains untested.

6. **P2 — A failed optional optical-axis check can destroy an otherwise valid hotspot proposal.**  
   [select.js:243](app/src/stages/select/select.js:243)

   Optical-axis triangulation runs unconditionally, including after a usable hotspot has been found. Singular geometry throws through the entire proposal operation.

   **Reproduced:** converging hotspot rays with parallel optical axes produce `singular matrix`, losing the valid hotspot result.

   Isolate optional triangulations, reject ill-conditioned/non-finite solutions, and retain an independently valid hotspot proposal. Individual failed rotation trials should likewise not abort all trials.

7. **P2 — Proposal outputs depend on configuration excluded from the stage key.**  
   [ingest/stage.js:19](app/src/stages/ingest/stage.js:19), [ingest.worker.js:47](app/src/stages/ingest/ingest.worker.js:47)

   Ingest declares `configKeys: []`, yet writes `proposal.json` using selection settings including start/end, hotspot thresholds and HFOV. Changing those settings leaves a completed ingest stage—and its proposal—valid under the same key.

   Include proposal-affecting settings in its dependency key or make proposal generation a separate operation. There is also an inconsistency at [select.js:231](app/src/stages/select/select.js:231): fitting honors an HFOV override, but the returned proposal reports metadata HFOV.

8. **P2 — OPFS acquisition and failure cleanup are incomplete.**  
   [select.js:391](app/src/stages/select/select.js:391), [select.js:433](app/src/stages/select/select.js:433), [opfs-store.js:58](app/src/stages/select/opfs-store.js:58)

   Files are opened before entering the cleanup `try`. Failure opening `thermal.u16` or the cache leaves previously acquired handles outside that cleanup. Within cleanup, one throwing `flush()`/`close()` prevents subsequent handles from closing. Cache removal happens only on success.

   Put acquisition inside the protected scope and close each acquired handle independently. Define cleanup of unpublished scratch generations on failure/retry.

   Fresh controller generations provide useful protection for published outputs, but the stage tests do not exercise quota failures, interrupted writes or cancellation followed by reopening OPFS handles. Active FFmpeg commands also lack cooperative abort handling; test the host’s forced-termination path.

9. **P2 — The acceptance suite proves less than a complete reading of the notes suggests.**  
   [test_select_node.mjs:103](test/stage2/test_select_node.mjs:103), [test_numeric.mjs:33](test/stage2/test_numeric.mjs:33), [runner.mjs:44](test/stage2/runner.mjs:44)

   The main limitations are:

   - All three injected blank frames are outside the canonical hotspot search. This tests blank rejection during selection, **not blank rejection during hotspot triangulation**.
   - `test_numeric.mjs` checks contrast-stretched feature input, not CLAHE itself. The seven saved CLAHE fixtures pass, but their generation/comparison is not included in the listed self-contained tests.
   - Browser acceptance covers three proposal modes plus drift compaction; the broader option matrix runs against native-decoded frames in Node.
   - Native extraction caches are keyed by arguments, without the input hash or native-source revision, allowing stale references after changes.
   - Stage-1 comparison permits hashes to be absent. Its Chrome test should explicitly require exactly one hash per frame.

   Add no-hot/failed-hot proposals, blank hot frames without cached samples, incomplete telemetry, degenerate rays, non-keyframe batches, cancellation and quota-failure cases. Include CLAHE fixtures for tiny dimensions and cases where only one dimension is divisible by eight.

The positive evidence is substantial: I rechecked the saved **201/201 browser metadata/timestamp/pixel comparison**, successful saved option outputs, **12 numeric fixtures**, and **seven CLAHE fixtures**. The ordinary native flow—blank/drift statistics, view/radius selection, rotation, ENU conversion and canonical CLAHE—appears faithfully ported. The single-pass compaction approach is sound for those validated inputs.

This review used read-only comparisons and in-memory probes. I did not rerun Chrome or modify files.

# Round 2, 2026-11-08

**Not yet acceptable to commit.** Most fixes are sound, but three blocking issues remain.

1. **P2 — Failure cleanup still excludes the final metadata write.**  
   [select.js:609](app/src/stages/select/select.js:609), [select.js:657](app/src/stages/select/select.js:657), [opfs-store.js:76](app/src/stages/select/opfs-store.js:76)

   `complete` becomes true and the cleanup scope ends before writing `frames.json`. A quota/write failure there leaves `features.u8` and `thermal.u16` behind.

   **Reproduced in all three modes:** injected failure on the metadata write leaves both pixel files, although handles close correctly.

   The new fault tests miss this: [FaultStore](test/stage2/test_robust.mjs:225) faults only handle writes, while the browser test fails after 20 MiB during pixel output. Extend cleanup through successful metadata completion and abort failed `createWritable()` operations. Add metadata-write/close fault tests.

2. **P2 — Telemetry validation introduces a native-compatible-path regression.**  
   [select.js:106](app/src/stages/select/select.js:106), [select.js:419](app/src/stages/select/select.js:419)

   Validation always rejects null `maximum_c`, including with `locate_hotspot: false`. Native extraction does not read that metadata field on this path; it computes output extrema from decoded pixels.

   **Reproduced:** frame 0 with `maximum_c: null`, frames 0–3, and hotspot location disabled:
   - Native extraction succeeds with four frames.
   - Browser selection rejects it, incorrectly claiming native fails too.

   Validate fields where the selected execution path uses them. The new [null-maximum test](test/stage2/test_select_node.mjs:133) covers only hotspot-enabled extraction.

3. **P2 — Proposal generation still uses unchecked telemetry outside the requested range.**  
   [select.js:285](app/src/stages/select/select.js:285), [select.js:287](app/src/stages/select/select.js:287)

   Validation correctly excludes out-of-range pose fields, but `posed` then scans every frame and chooses the first as its provisional origin.

   **Reproduced with `start: 17`:**
   - Setting frame 0’s latitude to `"bad"` destroys the proposal despite valid in-range data.
   - Setting it to null silently shifts the proposed hotspot by approximately 14 m.

   Choose the provisional origin from validated in-range poses. The existing outside-range test modifies a later frame, so it misses this prefix-anchor case.

There is also a smaller, nonblocking limit edge: [frame-decoder.js:33](app/src/stages/ingest/frame-decoder.js:33) clamps `chainLimit` to at least one. A single frame exceeding `maxOutputBytes` therefore passes planning. Reject when no frame fits; the planner tests currently cover oversized multi-frame chains only.

The nine original findings now stand as follows:

| Original finding | Verification |
|---|---|
| 1. Fallback altitude | Fixed; complete fallback proposals are tested against native selection. |
| 2. Incomplete telemetry | Original coercion cases fixed; blocking path/range problems above remain. |
| 3. Source identity | Fixed: format/version, size, content hash and packet layout checked. |
| 4. Retained hot-frame pixels | Fixed: compact coordinates replace closures; ties tested across rotations. |
| 5. Dependency-chain bounds | Original oversized-chain bug fixed; planner tests and real GOP fixture added. Single-frame edge remains. |
| 6. Optional triangulation failures | Fixed: estimates fail independently, with finite/conditioning checks. |
| 7. Proposal configuration key | Fixed through `config.proposal`; effective HFOV is returned correctly. |
| 8. OPFS cleanup/cancellation | Substantially improved; final metadata failure remains uncovered and unfixed. |
| 9. Acceptance coverage | Substantially improved: blank hot frames, standalone CLAHE tests, stronger cache keys and mandatory browser hashes. Missing cases identified above remain. |

Read-only verification passed: the planner’s 2,000 random cases, all 15 robustness scenarios using an in-memory store, 34 saved CLAHE fixtures, 12 numeric fixtures, and the strict saved 201-frame metadata/timestamp/pixel comparison. The saved Node report records 34/34 passing cases. I inspected the new browser/GOP tests but did not rerun Chrome.

No files were modified.

# Round 3, 2026-11-08

**Yes—stages 1–2 are acceptable to commit.** The round-2 blockers and the planner limit edge are fixed. I found no new blocking regressions in this focused review.

- **Cleanup:** completion is recorded only after `frames.json` succeeds; failures remove all outputs. Failed writable writes/closes are aborted. Partial metadata-write cleanup passed in all three modes. [select.js:563](app/src/stages/select/select.js:563), [opfs-store.js:82](app/src/stages/select/opfs-store.js:82)
- **Validation:** `maximum_c` validation now depends on hotspot search; radiometry validation moved to decoded frames. The null-maximum regression case succeeds with hotspot search disabled. [select.js:108](app/src/stages/select/select.js:108), [test_select_node.mjs:169](test/stage2/test_select_node.mjs:169)
- **Proposal origin:** only validated in-range poses supply the provisional origin. Both `"bad"` and null prefix latitudes leave the complete proposal unchanged. [select.js:298](app/src/stages/select/select.js:298), [test_select_node.mjs:75](test/stage2/test_select_node.mjs:75)
- **Planner:** an oversized single frame is rejected before reading. The new regression assertion covers the previous clamp-to-one bug. [frame-decoder.js:33](app/src/stages/ingest/frame-decoder.js:33), [test_planner.mjs:56](test/stage1/test_planner.mjs:56)

Read-only checks passed: planner fixed cases plus 2,000 random layouts, all 18 robustness scenarios with in-memory storage, targeted reproductions, writable-abort probes, and the saved 201-frame pixel/metadata comparison. The saved Node report records 37/37 passing cases.

I did not rerun Chrome or Windows tests. No files were modified.
