# Codex review of G2 (telemetry-seeded mapping in wasm), round 1, 2026-11-08

The numerical G2 gate is demonstrated. I found one P1 and six P2 issues to address; the browser-feature claim also needs narrowing.

1. **P1 — Mapping cancellation never reaches worker termination.** [pool.js:73](app/src/stages/map/pool.js:73) ignores `opts.signal`, forwards it into the message, and terminates only after the RPC settles. [map.worker.js:63](app/src/stages/map/map.worker.js:63) runs all rounds synchronously, returns `false` from every stop callback, and never checks cancellation during mapping. Aborting left the promise pending and the worker unterminated in my probe. Wire the signal to termination and rejection with `AbortError`, including already-aborted signals. The Node callback tests do not establish the browser’s three-second Stop contract.

2. **P2 — Verification acceptance does not validate exported models.** [run_tests.sh:33](test/g2/run_tests.sh:33) never supplies `--model-tol`; [compare_geometry.py:86](test/g2/compare_geometry.py:86) therefore ignores F/E/H and pose values when deciding PASS. Zeroing every model still passed. Current E differences also exceed “last bits”: calibrated pair `(31,169)` has approximately **0.226** normalized Frobenius difference after accounting for scale/sign. Add model validity/equivalence checks and qualify [notes/g2.md:108](notes/g2.md:108). Mapping consumes configurations and inliers, so its successful comparison does not validate those matrices.

3. **P2 — The packaged wasm artifacts omit third-party notices.** [build-colmap.sh:26](app/src/stages/map/build-colmap.sh:26) copies only `.mjs` and `.wasm`. The external fork has `THIRD_PARTY_NOTICES.md`, but it is neither copied nor packaged by WebSplat. Carry the upstream license and dependency notices with the artifacts. Also restore the full upstream header on the copied two-view implementation at [0002…patch:4576](app/src/stages/map/colmap-wasm-patches/0002-native-WebSplat-entry-websplat.cc-verification-and-p.patch:4576); its attribution comment omits the conditions and disclaimer retained on the other copied files. [COLMAP’s BSD terms](https://raw.githubusercontent.com/colmap/colmap/3.13.0/COPYING.txt) specify source and binary notice preservation.

4. **P2 — Partial allocation failure leaks wasm memory.** [colmap.js:47](app/src/stages/map/colmap.js:47) allocates all arguments before entering `try/finally`. If a later allocation fails, earlier pointers are never freed; my injected second-allocation failure freed none. String allocations also lack the null check used by `put()`. Allocate incrementally inside the protected block and reject null string pointers before calling C++.

5. **P2 — Worker failures can leave operations pending indefinitely.** [pool.js:8](app/src/stages/map/pool.js:8) handles application `{type:'error'}` messages but installs no worker `error` or `messageerror` handlers. A module-loading failure leaves the promise pending, preventing `finally` from terminating workers. Reject on those events and remove handlers when the call settles.

6. **P2 — Existing checkouts bypass source pinning and patch updates.** [build-colmap.sh:18](app/src/stages/map/build-colmap.sh:18) checks out the pinned commit and applies patches only when `.git` is absent. Subsequent builds silently reuse any revision or edits, including stale implementations after the patch series changes. Validate the existing revision and patch identity, refusing mismatches without overwriting local work. The system compiler and Boost versions also remain environmental inputs rather than enforced reproducibility inputs.

7. **P2 — The claimed browser-feature acceptance uses native features.** [run_tests.sh:43](test/g2/run_tests.sh:43) supplies native WSFT features, and [g2_page.js:56](test/g2/g2_page.js:56) explicitly loads `/g2data/native/features.bin`. These establish browser matching of native descriptors followed by verification/mapping. They do not establish the ≥90% criterion **with browser-extracted features**, as claimed at [notes/g2.md:135](notes/g2.md:135). Keep that acceptance pending until stage 3 supplies its own features.

I found no substantive orchestration discrepancy with `sfm.py`: thresholds, clearing and rebuilding tracks for re-admission, fixed-pose triangulation, completion/merging/filtering, covariance `σ²I`, robust position priors, Cauchy reprojection loss, 50 BA iterations, intrinsics refinement after round zero, and fixed principal point agree. The camera/pose conventions, rounding, image IDs, track indices, WSFT/WSTV layouts and binary exports fit the contracts.

Validation: a fresh four-round Node mapping run reproduced **all five saved model files byte for byte**, taking **65.0 s** with a **374.9 MB wasm heap**. Rechecked model differences were **0.0205 m / 0.0092°** median from native geometry and **0.1323 m / 0.0469°** from browser matches; all configured model tolerances passed. Saved Node/laptop/win11 outputs were identical. Telemetry and callback cancellation tests passed, and the sampler matched libstdc++ across **1.5 million draws**, including rejection cases. Seeded native repeats were identical; unseeded repeats differed as documented.

I changed no project files. I did not perform a clean rebuild or rerun Chrome/Windows.

# Round 2, 2026-11-08

**I would hold the commit for two remaining P2 fixes.** The original P1 cancellation issue is fixed, and the numerical G2 result remains credible.

1. **P2 — `check_models.py` overstates what its comparison proves.** [check_models.py:99](test/g2/check_models.py:99) compares inlier counts, while COLMAP’s support comparator also uses the sum of inlier residuals to break ties. Equal counts therefore do not establish equally good RANSAC solutions.

   On the current artifacts:
   - `(14,156)`: both have 72 E inliers, but wasm’s residual sum is **3.27×** native’s; eight inlier memberships differ.
   - Calibrated `(31,169)`: both have 49, but wasm’s residual sum is **1.29×** native’s.
   - Counts are equal in **30** differing pairs, with **+1 in two**, rather than the 31/one stated in [notes/g2.md:118](notes/g2.md:118).

   The Sampson calculation itself matches the port. The test demonstrates valid matrices with near-equal consensus counts; it does not prove the “equivalent optima” explanation. Either assess residual scores and consensus overlap using justified tolerances, or narrow the test’s and notes’ conclusions. These differences still do not invalidate mapping, which consumes configurations and inliers.

2. **P2 — Worker failure cleanup remains incomplete for synchronous exceptions.**
   - [pool.js:62](app/src/stages/map/pool.js:62) constructs the pool before entering `try`. When the second factory invocation throws, the first worker survives. I reproduced this.
   - [pool.js:46](app/src/stages/map/pool.js:46): a synchronous `postMessage()` failure rejects the promise without removing the installed handlers. My probe retained all three worker listeners; the signal listener also bypasses cleanup.
   - [pool.js:32](app/src/stages/map/pool.js:32): exceptions from progress/round callbacks escape the message handler. A throwing `onRound` produced an uncaught exception, then the RPC subsequently resolved successfully.

   Construct workers inside the cleanup scope and route synchronous send/callback exceptions through the same cleanup-and-reject path.

Verification of the seven original fixes:

| Original finding | Round-two result |
|---|---|
| Abort → termination | **Fixed.** Fresh Chrome Stop test passed, including pre-aborted signals and mapping during BA. |
| Model validation | **Improved, partially resolved.** F/H comparison is enforced; zeroed models now fail. E interpretation needs the correction above. |
| Licensing/notices | **Fixed.** Headers restored, packaged files match their sources, and an in-memory Vite bundle emitted notices and retained the inline licenses. |
| Partial allocation failure | **Fixed.** Incremental allocation and null-string checks are present; mock tests passed. |
| Worker errors | **Partially resolved.** `error`/`messageerror` reject and clean up; synchronous paths remain incomplete. |
| Build identity | **Fixed for the reported issue.** Parent commit, patch IDs, clean tracked tree, packaged notices and wasm hash match `BUILD-INFO.txt`. System tool versions are recorded rather than enforced. |
| Native-feature wording | **Fixed.** Browser-extracted-feature acceptance is explicitly pending stage 3. |

Fresh Chrome cancellation settled in **0.4–0.6 ms**, with zero tracked live workers and no subsequent messages. The updated full geometry/model checks and allocation tests passed. The native fork’s executable source is unchanged from round one; its changes are attribution/notices.

No project files were changed. I did not perform a clean native rebuild or rerun Windows.

# Round 3, 2026-11-08

**G2 is acceptable to commit as the feasibility gate.** Both round-two findings are resolved; I found no new blocking regressions.

- [pool.js:21](app/src/stages/map/pool.js:21) now cleans up and rejects on synchronous send and callback exceptions. Worker construction is inside the cleanup scope, and failures settle pending pool calls. The new `test_pool.mjs` passed, including listener cleanup and preservation of the original errors.
- [check_models.py:102](test/g2/check_models.py:102) reports residual-sum ratios and membership changes. Its narrowed conclusion and [notes/g2.md:117](notes/g2.md:117) match the evidence: 32 differing E matrices, 30 equal consensus counts, two increased by one, residual ratios 0.641–3.274, and up to 16 membership changes.

Validation passed: pool tests, allocation tests, full saved geometry/model checks, shell syntax checks, and a **fresh Chrome Stop test**. Running verification/mapping aborted within **0.5 ms**, with zero live workers or late messages. The wasm hash is unchanged.

Browser-extracted-feature acceptance and stage integration remain later work, as documented. No project files were changed; I did not repeat the clean build or Windows run.
