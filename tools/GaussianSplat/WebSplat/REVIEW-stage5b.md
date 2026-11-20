# Codex review: stage 5b scene preparation

## Round 1

**Verdict: accept after fixes.** No P1 findings. The scene-preparation core matched the native loader in the exercised cases; the main gaps are comparator coverage and harness isolation.

1. **P2 — The comparator accepts materially invalid datasets.**  
   [compare_dataset.py:54], [compare_dataset.py:147]  
   Each of these independent corruptions still returned **30/30 PASS**: invalid `frame`/`image_id`, invalid non-test splits, wrong dtype/shape, deleted scene/training options, and wrong initialization, near-plane, pruning-frequency and training hot-threshold settings. Relabelling all 165 elevation/observation rejections as budget exclusions also passed. Numeric array tolerances are strict, but the metadata checks are incomplete. Validate required fields, enum values and complete option keys, and compare the omitted training settings and rejection reasons.

2. **P2 — Windows Stop can kill another profile’s Chrome.**  
   [stop.ps1:3]  
   Escaping `$name` protects regex syntax but still performs substring matching. Stopping `websplat-harness-profile` also matches `websplat-harness-profile-g1`. Match the complete `--user-data-dir` argument, including its boundary, before terminating processes.

3. **P3 — The native dump incorrectly rounds PINHOLE intrinsics.**  
   [dump_scene.py:148]  
   SIMPLE_RADIAL reconstructs `K` from double-precision camera parameters; PINHOLE instead uses the float32 tensor. An added unequal-focal-length PINHOLE case produced identical targets and initial points, but failed the exact `K` comparison solely because of this rounding. Construct both reference matrices from `cam.params`.

4. **P3 — Relative application-build overrides can serve only 404s.**  
   [env.mjs:7]  
   `WEBSPLAT_APP_DIST=./app/dist` reaches the server unnormalized; its normalized filenames fail the root-prefix check. I reproduced HTTP 404 for `/`. Resolve the override to an absolute path.

All eight supplied cases and five negative controls passed. An additional single-view/no-hot/no-holdout case also passed. I verified that `dump_scene.py` invokes the actual `train.main()` scene-loading, initialization, optimizer and strategy setup, with CPU substitution.

Isolated Linux browser checks passed Stop during both passes, cleanup, reload during target writing, restart, publication and completed-stage resume. All four published files matched Node output byte-for-byte. The 201-frame run peaked at approximately **231 MiB Node RSS**. Registration, invalidation, streaming writes and weighted progress/ETA showed no further blocker. Windows behavior was assessed statically.

**Keep float16 targets and build `init.ply` in stage 6.** That follows the contract and native target precision. Validate PNG16 transport against the float16 source and rerun G1 metrics using it. Stage 6 must implement the recorded initialization and translate or enforce the learning-rate, densification, cap, cylinder and canopy rules; recording them does not make stock Brush apply them. Point order and RNG differences also preclude claiming identical initialized Gaussians.

No tracked files were modified. Probe artifacts are under `$WEBSPLAT_DATA/stage5b/codex-review/`.

## Round 2

**Verdict: accept after fixes.** No P1 findings. Two round-1 fixes remain incomplete.

1. **P2 — Windows Stop still matches some other profiles.**  
   [stop.ps1:5]  
   The optional quotes allow whitespace *inside* a quoted path to satisfy the argument boundary. Stopping `websplat-harness-profile` still matches:
   ```
   --user-data-dir="C:\...\websplat-harness-profile g1"
   ```
   I confirmed this using the actual PowerShell predicate on Win11, without stopping processes. The old `-g1` suffix case is correctly excluded. Use separate quoted/unquoted alternatives with matching quotes and boundaries around the complete argument.

2. **P2 — Comparator metadata coverage remains incomplete.**  
   [compare_dataset.py:215], [compare_dataset.py:76]  
   Each new corruption independently returned **45/45 PASS**:
   - Set `training.init.seed` to `999`, or delete it.
   - Replace `training.sh_degree_schedule` with `sh_degree`, or delete it.
   - Replace the undistortion method with `nearest neighbour, BORDER_REFLECT`.

   The first two can change trainer initialization or behavior; the third falsely describes the preparation algorithm. Capture and check the seed and SH schedule, and validate the complete undistortion method.

Verification completed:

- `run_tests.sh` exited **0**: all nine cases, including unequal-focal PINHOLE, and all **23 negative controls** passed.
- Every earlier corruption was independently retried and now rejected, including relabelling the 165 rejected views.
- Extra/changed options and incorrect ceiling dimensions were rejected.
- PINHOLE `K` now correctly uses `cam.params`.
- Default, relative, normalized-relative and absolute `WEBSPLAT_APP_DIST` paths all served HTTP 200 with the expected content.
- No scene-output regression was observed.

No tracked files were modified. Logs and probes are under `$WEBSPLAT_DATA/stage5b/codex-review/round2/`; the suite log is `codex-review/round2-suite.log`.

## Round 3

**Verdict: accept.** No remaining P1/P2/P3 findings.

Verified:

- `run_tests.sh` exited **0**: all nine dataset cases and all **28 negative controls** passed.
- Independent retries rejected every round-2 seed, SH-schedule and undistortion corruption, including deleted fields.
- Win11’s actual `stop.ps1 -test` passed **18 predicate cases**. It excludes quoted `profile g1` paths and matches all supported quoting forms.
- Source-extraction probes detected changed seeds/schedules and rejected altered undistortion calls.
- The additional single-view/no-hot case passed; application-build path checks also passed.

No regressions observed within scope. No tracked files were modified.

Evidence: [suite log ($WEBSPLAT_DATA/stage5b/codex-review/round3/suite.log), [independent comparator probes ($WEBSPLAT_DATA/stage5b/codex-review/round3/comparator-probes.log), [Windows predicate results ($WEBSPLAT_DATA/stage5b/codex-review/round3/profile-probes.json).
