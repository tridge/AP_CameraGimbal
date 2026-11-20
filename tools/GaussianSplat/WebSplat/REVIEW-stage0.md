# Codex review of stage 0 (reference, contracts, skeleton, harness), round 1, 2026-11-08

**Stage 0 needs fixes before acceptance, chiefly around crash recovery and job ownership.** The reference evidence checks out, but the 22 passing checks leave several failure paths untested.

1. **P1 — Manifest saves can publish an invalid checksum.** [manifest.js:49](app/src/storage/manifest.js:49) hashes the live manifest, awaits WebCrypto, then serializes that same mutable object. Progress updates during the await change the written content. I reproduced a successfully written sequence 2 failing checksum validation and reload falling back to sequence 1. Hash and write an immutable snapshot.

2. **P1 — Unreadable or unsupported manifests cause destructive recovery.** [job.js:23](app/src/storage/job.js:23) treats “neither slot is usable” as a new job. Recovery then deletes existing generations absent from the empty manifest. **Browser reproduction:** replacing both manifests with version 2 caused an existing output to disappear. Distinguish missing jobs from corrupt or unsupported jobs; preserve their data.

3. **P1 — Final outputs and completion state are published separately.** [controller.js:244](app/src/pipeline/controller.js:244) publishes outputs before setting `done`. Publication also clears and deletes the checkpoint. **Browser reproduction:** reopening at that boundary produced `interrupted`, with final output present but no checkpoint. Training consequently restarts. Publish `state: 'done'` in the same manifest transaction.

4. **P1 — Cancelled checkpoint handlers can mutate a subsequent run.** [host.js:60](app/src/workers/host.js:60) awaits checkpoint publication independently of worker settlement. Forced cancellation terminates the worker and rejects the run, but leaves that handler executing. I reproduced checkpoint side effects after the run had rejected. Use a run identifier, reject stale publications, and drain or invalidate pending handlers before allowing another run.

5. **P1 — Reset releases ownership before the controller has stopped.** [main.js:294](app/src/main.js:294) calls `stop()` without awaiting termination, then destroys the job and releases its lock. **Browser reproduction:** another owner acquired the lock, after which the old controller wrote its paused manifest. Await controller shutdown and all publications before deletion and lock release. Ordinary second-tab refusal worked correctly.

6. **P2 — Manifest quota failures bypass stage error handling.** [controller.js:163](app/src/pipeline/controller.js:163) saves before entering the protected execution block; periodic saves are also unobserved promises. Injecting `QuotaExceededError` left the pipeline stopped while its stage remained `running`, with no recorded error and a live clock. Handle save failures throughout the lifecycle and release unpublished storage before retrying.

7. **P2 — The verified-geometry contract contradicts the current implementation.** [CONTRACTS.md:178](CONTRACTS.md:178) specifies WSMT inliers plus an optional, separate model-only WSTV file. Current [format.js:93](app/src/stages/match/format.js:93) and the native exporter instead use one WSTV container containing pair tables, inliers, and models. A reader following the contract rejects or misreads the supplied geometry. Align the contract, exporter, and version.

8. **P2 — Several contract details remain ambiguous or incomplete.** [CONTRACTS.md:238](CONTRACTS.md:238) onward needs clarification:
   - Native cold selection budgets `max_images − hot_count`, rather than selecting up to `max_images` cold images.
   - Required dataset values such as `extent_m`, `v_hot`, filtering rules, and option defaults lack complete definitions.
   - Training checkpoints are placed under `train/gen-N`, contradicting the later `ckpt-N` layout.
   - Manifest/key hashes use sorted-key canonical JSON in code, but that serialization is unspecified.

9. **P2 — Real-stage phases defeat throughput measurement.** [select.worker.js:33](app/src/stages/select/select.worker.js:33) labels actual processing `select` or `output`; ingest similarly uses `index`, `hash`, and `propose`. `StageClock` counts only `work`. Saved thermal reports therefore classify most processing as overhead and calibrate from startup/source-verification time. Separate phase labels from whether elapsed time contributes to throughput.

10. **P2 — Unknown work can become a misleading zero ETA.** [eta.js:67](app/src/pipeline/eta.js:67) counts an unknown-total stage down to zero once its prior expires; `uncertain` is unused. Overall estimation also treats a null current estimate as zero. Preserve uncertainty instead of displaying “under a minute” for continuing, unbounded work.

11. **P2 — Dummy acceptance can pass without validating the claimed evidence.** [dummy.mjs:317](test/harness/dummy.mjs:317) skips short or unsampled stages, then initializes worst ETA error to zero. Empty coverage passes. The saved speed-30 reports validate per-stage ETA only for training; the speed-5 report covers seven stages, excluding export. Its final manifest verifier also selects the highest parseable sequence without validating its checksum. Assert required coverage and use the production manifest validator.

12. **P2 — Thermal acceptance does not establish image or pose parity.** [thermal.mjs:116](test/harness/thermal.mjs:116) compares frame count and camera positions. It does not compare rotations, frame identities, radiometry, thermal pixels, or CLAHE pixels; missing frames metadata skips those comparisons entirely. Require those artifacts and comparisons before claiming full pose/extraction parity.

13. **P2 — Worker startup cloning failures leak the worker.** [host.js:80](app/src/workers/host.js:80) can throw synchronously from `postMessage`. Promise rejection then bypasses `finish()`, leaving the worker and abort listener alive. I reproduced this with an uncloneable message. Route startup errors through the same cleanup path.

14. **P2 — Fresh development startup omits required assets.** [package.json:7](app/package.json:7) starts Vite without preparing the ignored FFmpeg assets. A clean `npm ci && npm run dev` therefore lacks the decoder files. Add asset preparation to development startup.

Validation: the build passed into `/tmp`; container tests passed; reference metrics reproduced; all **402 saved extraction PNGs** matched. Saved reports contain the claimed 22 passes on both machines. Fresh Chrome exposed only SwiftShader here, which the device check correctly rejected, so hardware smoke tests were not rerun.

Packaging coverage is sound: `dist/`, `node_modules/`, and `public/vendor/` are ignored. WebSplat is currently untracked; no tracked generated blobs were found, and the largest nonignored file is about 26 KB. Formatting checks fail on four stage-4 files. No project files were changed.

# Round 2, 2026-11-08

**Stage 0 is not acceptable to commit yet.** Two reproducible data-loss paths remain, alongside three P2 issues.

1. **P1 — Transaction rollback can delete a durably published checkpoint.** [job.js:67](app/src/storage/job.js:67) mutates the shared manifest before its queued save executes. An earlier queued save can therefore publish that mutation. If the transaction’s own write fails, rollback and cleanup use the older generation.

   **Browser reproduction:** checkpoint 2 was published by a queued save; its own publication then hit an injected quota error. Memory reverted to checkpoint 1, and cleanup deleted checkpoint 2, leaving the latest valid manifest referencing a missing file. Serialize the whole transaction and prevent ordinary saves from observing uncommitted changes.

2. **P1 — Mixed manifest versions still cause destructive recovery.** [manifest.js:51](app/src/storage/manifest.js:51) returns a supported slot before considering an unsupported slot.

   **Browser reproduction:** with an older valid v1 slot and a newer, checksummed v2 slot, opening the job selected v1 and deleted v2’s output generation. The new harness tests only the case where **both** slots are unsupported. Preserve newer-version data and refuse incompatible recovery.

3. **P2 — Completion is still displayed before it is durable.** [controller.js:352](app/src/pipeline/controller.js:352) reads live state and generation, while only checkpoints use durable records.

   Blocking the manifest close reproduced live `done` alongside durable `running`. The checkpoint-display fix works, but completion needs the same treatment. [dummy.mjs:221](test/harness/dummy.mjs:221) can consequently end its run before publication finishes; require the controller to stop and verify persisted completion states.

4. **P2 — Single-pass selection now reports twice the actual work.** [select.worker.js:41](app/src/stages/select/select.worker.js:41) doubles the total unconditionally, although `single` mode has no second output pass.

   **Fresh canonical run:** its final worker progress was **201/402**; cache and re-decode correctly reached 402/402. This leaves the default mode at 50% until controller completion fills the counter, distorting progress and ETA. Make accounting mode-dependent.

5. **P2 — The training contract still omits native initialization behavior.** [CONTRACTS.md:277](CONTRACTS.md:277) drops points without observations in chosen images. Native [train.py:192](tools/GaussianSplat/train.py:192) first projects such points into chosen images and samples up to four visible views. Filtering or subsampling images can therefore produce different initial clouds. Document that fallback, or explicitly record an intentional deviation.

Against the original 14 findings:

| Original finding | Verification |
|---|---|
| 1: Checksum race | Fixed; mutation during hashing preserved a valid snapshot. |
| 2: Unreadable manifests | Both-unsupported case fixed; mixed-version case remains above. |
| 3: Atomic completion | Fixed on disk; premature completion display remains above. |
| 4: Late checkpoint handlers | Pending handlers now drain before settlement; cancellation probe passed. |
| 5: Reset ownership | Shutdown precedes destruction; fresh Reset probe passed. |
| 6: Quota handling | Initial-save failure now records `failed`, identifies quota, and removes the clock; transaction rollback remains unsafe. |
| 7: Geometry layout | WSTV contract matches implementation; container tests passed. |
| 8: Contract completeness | Most requested details added; native point fallback remains missing. |
| 9: Throughput phases | Work/overhead distinction fixed; single-pass progress regressed. |
| 10: Unknown ETA | Expired priors and overall unknown duration now remain unknown. |
| 11: Dummy assertions | Coverage is non-vacuous and hashes use production validation; completion race remains. |
| 12: Thermal comparisons | Required artifacts and per-frame metadata/pixel comparisons added. |
| 13: Worker startup cleanup | Uncloneable-message probe confirmed worker termination. |
| 14: Development assets | `dev` now prepares FFmpeg assets. |

The additional checkpoint-display fix passed: while publication was blocked, the snapshot retained checkpoint 1; after commit, it showed checkpoint 2.

Fresh selection runs in **all three modes** matched canonical per-frame metadata and thermal/CLAHE pixels exactly. I found no selection-output change from the worker edit; the regression is in progress accounting.

Build, formatting, container tests, full dummy completion/reload, and Reset checks passed. Saved hardware reports show **26/26 dummy checks** and **12/12 thermal checks**; speed-30 ETA coverage remains explicitly training-only. Hardware smoke tests were not rerun. No project files were modified.

# Round 3, 2026-11-08

**Stage 0 is acceptable to commit within the reviewed scope.** All five fixes are verified; neither round-2 data-loss path reproduces.

| Fix | Verification |
|---|---|
| Queued transactions | [job.js:84](app/src/storage/job.js:84) applies changes to a copy inside the queue. Injecting failure at the actual OPFS stream close, among six queued saves, preserved checkpoint 1 through cleanup and reopen. |
| Mixed versions | [manifest.js:53](app/src/storage/manifest.js:53) refuses either-slot version mismatches. Both generations’ bytes remained intact in the repeated probe. |
| Durable completion | Delaying the manifest close kept live and durable states at `running`; both became `done` afterward. The dummy harness now waits for controller stop and checks persisted completion. |
| Selection progress | [select.worker.js:40](app/src/stages/select/select.worker.js:40) produced **201/201** for single and **402/402** for re-decode/cache. Fresh canonical runs in all three modes preserved metadata and thermal/CLAHE pixels exactly. |
| Projection fallback | [CONTRACTS.md:280](CONTRACTS.md:280) now agrees with native projection, visibility, sampling, and point-count behavior. |

The new harness assertions cover the reported cases. Its quota hook fails before writing; my independent stream-close fault additionally tested failure during publication. Saved reports show **30/30 dummy checks** on all three adapters and **14/14 thermal checks** for the recorded modes.

Build, container tests, and scoped stage-0 formatting checks passed. Full-package formatting flags unrelated map/train files. No remaining blocking stage-0 issues or new regressions were found. Hardware smoke tests were not rerun; no project files were modified.
