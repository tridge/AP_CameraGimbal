# Codex review of stage 4 (pairs and matching), round 1, 2026-11-08

The matching implementation looks credible for the canonical input, but the acceptance tests can conceal real failures. I would fix the following before treating stage 4 as accepted. No project files were modified.

1. **P1 — The tolerance checker can excuse matches that cannot pass the native rule.** [compare_matches.py:102](test/stage4/compare_matches.py:102) accepts a difference when **any** of the ratio, forward or reverse margins is within tolerance. A reverse tie therefore excuses an incorrect match even when its ratio test fails by about 37 ulps. Acceptance requires all predicates to permit the reported result under bounded perturbations. Additionally, [line 83](test/stage4/compare_matches.py:83) exempts arbitrary browser matches below 15 whenever native stores zero; those 212 matches are unvalidated. Recompute the missing native raw matches rather than exempting them.

2. **P1 — The test runner hides failed tests and can reuse stale results.** [run_tests.sh:11](test/stage4/run_tests.sh:11) uses pipelines with `set -e` but no `pipefail`. Edge-test failure is hidden by `tail`; full-run failure can be hidden by `grep`; comparator failure is hidden by the final `tail`. A failed browser run can leave an old `accept.bin` for comparison. Propagate every command’s failure and require a fresh output artifact.

3. **P2 — The G2 verified fixture contradicts the agreed geometry format.** [export_native.py:85](test/stage4/export_native.py:85) exports verified matches using raw `WSMT` magic, with configuration replacing status and no geometric models. [CONTRACTS.md:150](CONTRACTS.md:150) requires `WSTV` with F/E/H and relative pose. A conforming consumer rejects this fixture; a raw decoder silently misinterprets its statuses. Align the export and [handoff description](notes/stage4.md:133) with the contract.

4. **P2 — Pair-selection parity fails at tied neighbour cutoffs, without informing the worker’s caller.** [pairs.js:34](app/src/stages/match/pairs.js:34) uses index ordering instead of NumPy’s unstable ordering. This limitation is acknowledged, but [worker.js:21](app/src/stages/match/worker.js:21) discards `ties`. With default options and 80 frames alternating between two repeated positions, I reproduced **56 differing pairs**. Establish an explicit tie policy and expose ambiguity rather than claiming unconditional native parity.

5. **P2 — Shader-initialisation failure escapes resource cleanup.** [matcher.js:195](app/src/stages/match/matcher.js:195) precedes the cleanup `try/finally`. An injected pipeline rejection left all seven allocated buffers undestroyed and the validation scope open. Device loss during setup also bypasses the later `DeviceLost` handling. The worker eventually destroys its device, limiting the leak there, but direct callers retain resources. Cover allocation and pipeline setup with cleanup and consistent error classification.

6. **P2 — Concurrent worker runs share cancellation state.** [worker.js:18](app/src/stages/match/worker.js:18) overwrites the global controller while an earlier run can still await device creation. Both then read the newer controller at line 32. I reproduced two runs sharing one signal and producing indistinguishable completion messages. Reject overlapping runs or give each run its own controller and identifier.

7. **P2 — The container decoder silently accepts truncated data.** [format.js:26](app/src/stages/match/format.js:26) validates only magic/version. A header declaring a pair without its table is accepted; a pair declaring 12 absent matches decodes as empty. Validate lengths, totals, offsets and statuses before G2 consumes the data.

The default mathematical port otherwise checks out: inclusive angle/scale thresholds, sequential neighbours, normalised descriptors, strict forward L2 ratio test, reverse first-index ties, and skipping either image below two descriptors. Equal forward maxima fail the default ratio test, so Torch’s unspecified forward tie index does not affect acceptance.

The WGSL indexing, uniform early return, barriers and atomic ordering appear sound for valid inputs. Workgroup storage is 12,544 bytes, within default limits; the feature-count guard protects the packed reverse key. I found no evident kernel race or bounds defect. Cross-adapter numerical differences remain possible.

I reproduced **3,175 identical reference pairs**, **4,097,280 identical quantised values**, the reported **35 differences**, and byte equality between the saved NVIDIA outputs. All 35 differences also passed a stronger, optimistic all-predicate feasibility check under independent ±1-ulp perturbations. That supports numerical sensitivity, but does **not** establish the claimed cuBLAS error bound or prove the checker sound. PyTorch permits reduced-precision intermediate reductions; capture the native similarities and backend settings to establish the actual cause. [PyTorch numerical-accuracy documentation](https://docs.pytorch.org/docs/main/notes/numerical_accuracy.html#reduced-precision-reduction-for-fp16-and-bf16-gemms)

Fresh GPU execution was unavailable here: Chrome reported no adapter. The OOM retry remains untested, worker cancellation is not exercised through real messages, and the claimed Dawn bug has no executable regression in the submitted tests. Explicit initialisation is sensible and agrees with [WGSL’s required initial values](https://www.w3.org/TR/WGSL/#initial-values). Verification parity and the ≥90% inlier acceptance criterion also remain outstanding.

# Round 2, 2026-11-08

**Not yet acceptable to commit as an accepted stage 4.** Six original fixes are addressed, but the tolerance checker still admits impossible matches. Two validation issues remain blocking.

1. **P1 — Feasibility ignores the fp16 output grid.** [compare_matches.py:50](test/stage4/compare_matches.py:50) permits similarities to vary continuously. Native and browser similarities must be representable fp16 values.

   I injected an extra match `(295, 5733)` into reference pair `(0, 1)` **in memory**. The checker returned **PASS, zero unexplained differences**. Within its 0.51-ulp bound, the best permissible fp16 scores give:

   ```
   d0             = 0.40864676
   float32(.8*d1) = 0.40850338
   ```

   The strict ratio test necessarily fails. Quantise the permitted interval endpoints to representable fp16 values, then evaluate the actual clamped, float32 distance rule and tie handling. Add this case to `--selftest`.

2. **P2 — Set comparison conceals duplicate matches and duplicate pair records.** [compare_matches.py:32](test/stage4/compare_matches.py:32) collapses pair records into a dictionary; [line 96](test/stage4/compare_matches.py:96) collapses matches into sets. I constructed a length-consistent container containing an additional duplicate match: loading succeeded and comparison saw identical match sets. Such output cannot come from the native matching rule. Validate pair uniqueness, offsets, indices and per-pair one-to-one correspondence before comparison; include duplicate corruption in `--selftest`.

The seven original findings now stand as follows:

| Finding | Round 2 result |
|---|---|
| Tolerance checker | Improved substantially; still blocked by finding 1 above |
| Runner failure propagation/stale output | Fixed: `pipefail`, output removal and fresh-file requirement |
| G2 format | Fixed: WSTV, models and pose; fixture/container checks pass |
| Pair-selection ambiguity | Fixed: explicit policy and worker reporting; synthetic tests pass |
| Setup cleanup/classification | Fixed; injected OOM retry and pipeline failure destroyed all eight buffers and balanced scopes |
| Overlapping worker runs | Fixed by inspection: per-run controller, IDs and Busy rejection; real-message tests added |
| Truncated container decoding | Fixed in the JavaScript decoders; corruption tests pass |

The numerical evidence is considerably stronger now. I reproduced **510,868 raw native matches**, zero differences against retained database matches, the recovered **212 small-pair matches**, and the reported **35 browser differences**. All 35 also pass my stricter discrete-fp16 interval check. I found no new matcher-kernel regression.

The submitted `--selftest` passes and catches its two injected corruptions, but does not exercise the counterexamples above. The recorded **0.5047336-ulp** CUDA maximum is valid evidence for the sampled 40 pairs, not a universal bound.

No project files were modified. GPU execution remains unavailable here, so I could inspect—but not independently rerun—the browser cancellation, loop-variable probe or CUDA measurement. G2 verification acceptance remains explicitly deferred.

# Round 3, 2026-11-08

**Yes—stage 4 is acceptable to commit for the agreed pair-selection and matcher scope. No remaining blocking issues.** Verification acceptance remains deferred to G2.

Both round-2 fixes are verified:

- The checker rejects `(295, 5733)` using fp16-permitted values and the float32 ratio rule.
- Validation catches duplicate pairs, matches, rows and columns, invalid offsets and out-of-range indices before dictionary/set conversion.
- The expanded `--selftest` passes.
- The reference comparison passes: **35 differences, zero unexplained**.
- At the configured **0.51-ulp tolerance**, feasibility agreed with exhaustive enumeration across **4,800 small-matrix checks**.
- The notes correctly describe the measured CUDA bound as empirical.

Two nonblocking follow-ups:

- [compare_matches.py:29](test/stage4/compare_matches.py:29): `np.fromfile` silently ignores trailing 1–3 bytes. Check byte-length divisibility before reading.
- [compare_matches.py:79](test/stage4/compare_matches.py:79): arbitrary `--ulps` values are not fully supported. Small tolerances can return a nearest value outside the bound; large tolerances inspect too few neighbours. Restrict the supported range or generalise the interval calculation.

Neither affects the configured acceptance run. No files were modified.
