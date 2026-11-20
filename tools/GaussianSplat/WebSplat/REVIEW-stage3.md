# Codex review: stage 3 features and stage 3-5 integration

## Round 1

**Verdict: accept after fixes.** No P1 findings.

1. **P2 — SIFT worker startup errors can leave extraction hanging.** [features/pool.js:24–32] creates workers, then awaits image reads before `call()` installs error handlers. A module-load error during that gap is lost. I reproduced this in Chrome with a nested worker: extraction remained pending until aborted. Install error handling immediately on worker creation and reject/terminate the whole pool on failure.

2. **P2 — The SIFT “exact” acceptance gate is substantially weaker than the claimed equivalence.** [compare_features.py:69–91] allows 1% missing keypoints and descriptor differences of two, without asserting equal counts, one-to-one correspondence or orientation agreement. A synthetic result with reordered rows, an extra keypoint, every orientation rotated 90°, and every descriptor byte increased by two still returned `PASS`. Enforce the documented count, keypoint and descriptor tolerances explicitly.

3. **P2 — Byte-identical resume is reported but never asserted.** [test_resume.mjs:60–73] calculates `bytesEqual`, then asserts only quaternion, translation and focal tolerances. Its assertion block returns `PASS` with `filesIdentical:false`. Assert equality of all five model binaries and exercise every published round checkpoint. The browser harness additionally covers same-tab Stop/resume, but does not test reload during a real mapping checkpoint write.

4. **P3 — Harness errors can leave the test process running indefinitely.** [thermal.mjs:100] starts a referenced interval that is cleared only on the successful path at line 146. An exception or timeout before that point closes the browser but leaves the interval alive. Hoist the timer handle and clear it in `finally`.

5. **P3 — Mapping progress omits the planned iteration display and uncertainty.** [map.stage.worker.js:55–56] sends iteration information as `extra.done/total`, which the stage-list renderer does not display. [map/stage.js:19–24] also leaves `uncertain` unset. Users see fractional round progress, without the BA iteration detail and uncertainty required by PLAN.md.

The current results themselves are supported:

- Stage 3 passed. Independent row-by-row comparison found identical counts across all 201 images, 1,501,042 keypoints, affine-component differences below `1e-6`, and 517 descriptor rows differing by at most one.
- Saved browser geometry contains **2858 pairs / 426211 inliers**, versus **2702 / 388488** in the actual native reference database. The harness uses the correct reference and enforces both 90% thresholds.
- Re-running model comparison passed the G2 limits: position median/p95 **0.250/0.510 m**, rotation **0.081/0.162°**.
- Fresh resumes after rounds 0 and 2 produced identical compared model binaries. Saved laptop, win11 and stopped/resumed browser runs also match across geometry and all five model binaries.
- PRNG restoration, per-round reconstruction rebuild, closed-file writes and manifest acknowledgement appear sound. Parent termination stopped nested workers in Chrome.
- Memory is substantial but bounded under the default pool: about **1.2 GB combined SIFT wasm heaps**, approximately **228 MB feature arrays**, and **375 MB mapping wasm**. Tests record memory rather than enforce an aggregate ceiling.

I ran the stage 3 suite and targeted G2 allocation, pool, cancellation and resume tests; I did not repeat the complete browser chain. No tracked files were modified.

GitHub anchors use reviewed worktree line numbers; uncommitted files may not resolve online yet.
tokens used

## Round 2

**Verdict: accept after fixes.** No P1 findings; two remaining issues:

1. **P2 — Exact SIFT comparison still accepts NaNs.** [compare_features.py:52–60]: NaN makes the maximum difference NaN, and the threshold comparison evaluates false. A two-image synthetic container with one NaN affine component passes `--mode exact --selftest`, reporting zero affine difference. Reject non-finite keypoints explicitly and add a NaN corruption test.

2. **P3 — BA iteration display has an off-by-one error.** [stage-list.js:22–23]: the wasm counter includes Ceres’ initial solver state. A real run with `maxIterations: 2` emits counters 1, 2, 3 against total 2; the renderer labels the last value “iteration 3 of at most 2.” Normalize the counter before reporting/displaying it.

The round-1 fixes otherwise verified:

- Both **stage 3 and complete local G2 suites passed**.
- Previous synthetic SIFT corruptions now fail.
- Previous delayed-read worker hang now rejects in direct and nested Chrome workers.
- Resume from checkpoints after rounds 0, 1 and 2 asserts and passes equality of all five binaries.
- Active-ticker fault injection exits with code 2.
- Triangulation/BA details render, and mapping estimates set `uncertain`.
- All five Chrome model binaries match Node byte-for-byte.
- Native CPU-SIFT inputs and model comparisons agree with the new notes.


## Round 3

**Verdict: accept after fixes.** Both requested fixes work; one boundary regression remains.

- **P3 — Zero BA iterations now produces NaN progress.** [map.stage.worker.js:57]: `maxIterations: 0` is accepted by the mapper, but the normalized counter becomes `0/0`. The emitted NaN causes Chrome’s progress-bar renderer to throw `TypeError`. Handle zero explicitly or reject that configuration before mapping.

Verified:

- NaN and infinity in either comparison input are rejected; the finite control passes.
- `maxIterations: 2` reports iterations **0, 1, 2**, with finite, monotonic progress and correct UI labels.
- The complete stage 3 suite passed, including all corruption self-tests.


No tracked files were modified.

Resolution: the P3 is fixed in map.stage.worker.js (progress fraction 1 when the iteration total is 0; the stage list already omits the iteration text then). Accepted.
