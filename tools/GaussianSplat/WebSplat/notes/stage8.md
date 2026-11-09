# Stage 8: integration, recovery, release

PLAN.md "8. Integration, recovery, release": full runs from the MKV on both machines with timings
in PROGRESS.md and calibrated estimates; recovery tests; a user README; a deployable build and
deploy script (not deployed: that needs the user's OK).

## Pass criteria (pre-registered 2026-11-09T01:53:44Z, before any run they judge)

Recorded at the time given in the section heading below and not changed afterwards. "The page"
is the app's thermal set in a harness Chrome (`test/stage8/`), from `$WEBSPLAT_DATA/input/orbit.mkv`.

### Full runs and estimates

- **Calibration data.** Laptop priors are calibrated from the 5 series 4 laptop runs (notes/stage6.md,
  per-stage medians), before any stage 8 run. win11 priors are calibrated from stage 8 win11 run
  1 (taken only with the win11 GPU idle: no other process above 5% GPU utilisation in the minute
  before it starts, checked with nvidia-smi over ssh), before win11 run 2.
- **Full runs.** Two per machine (laptop runs 1-2; win11 runs 1-2), each from choosing the MKV to the
  result view. A full run passes if all 8 stages finish, the app's own `verify()` passes (manifest
  checksums, output hashes), export's downloads exist, and no page or console error is logged. The
  stage 6 quality criteria are reported, not judged (stage 6 is accepted by its series 4).
- **Estimates (judged on validation runs: laptop runs 1 and 2, win11 run 2).** (E1) For each stage
  that takes at least 30 s, the prior the page holds for it when the stage starts is within +-25%
  of the stage's measured duration. (E2) During training, from 10% of its iterations on, every
  remaining-time value the page shows (sampled every 10 s) is within +-15% of the actual remaining
  training time. (E3) The whole-job remaining time the page shows when training starts is within
  +-20% of the actual remaining wall time to the end of export. Each validation run must pass
  E1-E3 for the estimates to pass on that machine.

### Recovery (laptop; judged per scenario)

Recovery runs use the full pipeline with training shortened to 6000 iterations (job config
`prepare.iterations`; one checkpoint at 5000) unless the scenario needs more. Each scenario
passes only if (a) the page shows a message a user can act on (the stage's error or the log names
the cause: storage full, another tab, device lost, out of memory, no telemetry, another app
version, ...), (b) the app's `verify()` passes on the job afterwards (no corrupted state), and
(c) where stated, the job then completes.

- **R1 Stop/resume at every stage.** One job; Stop once while each of the 8 stages runs (after
  some progress where the stage lasts long enough). Each Stop pauses within 3.5 s; each Resume
  continues; checkpointed stages (map, train) resume from their checkpoint, not from zero; the job
  completes.
- **R2 Reload during checkpoint writes.** Reload the page while training writes a checkpoint, and
  while map writes a round checkpoint. The job reopens with only fully published checkpoints; Resume
  completes.
- **R3 Quota exhaustion.** An injected QuotaExceededError (`?testFault={"quota":...}`) on (i)
  prepare's outputs and (ii) a training checkpoint. The stage fails with "browser storage is full";
  after the fault is removed (reload without it), Resume completes.
- **R4 Stale app versions.** (i) A manifest from a newer app version is refused with a message and
  the job's files are kept. (ii) A job whose stage records come from an older stage version: that
  stage and the ones after it are redone, earlier ones kept, and the job completes.
- **R5 Two tabs.** A second tab on the running job is refused with "open in another tab"; the first
  continues and completes.
- **R6 device.lost during training.** Injected device loss at iteration 2000
  (`testFault.train.deviceLostAt`): the train stage fails naming the device loss, no partial
  output is published, Resume (without the fault) completes.
- **R7 Allocation failure.** An injected allocation the GPU refuses during training
  (`testFault.train.allocAt`): the stage fails naming the GPU error; Resume completes.
- **R8 MKV with missing telemetry.** `test/stage8/strip_telemetry.mjs` removes the telemetry from
  every frame (radiometry kept). The page reports the missing telemetry before any GPU stage and does
  not proceed to mapping or training without it; the job state is valid.

### Laptop run 1: concurrent CPU load (recorded 2026-11-09T01:59:39Z, before its results were seen)

Laptop full run 1 started 01:54Z. From about 01:57Z to 01:59Z I ran the deploy script's builds
(prepare-assets, vite) on the same machine, overlapping the run's match and map stages, which are
CPU-heavy. The criteria above have no provision for this; run 1 is judged as registered and its
match and map timings are marked as possibly slowed. No other work runs on this machine during the
remaining timing runs.

## Laptop full runs 1-2 (judged as registered)

| run | stages | verify() | downloads | wall s | train s | E1 match / map / train | E2 worst | E3 |
|---|---|---|---|---|---|---|---|---|
| laptop-2026-11-09T01-56-20 | 8/8 done | pass (41 files) | pass | 923 | 741 | -34.8% FAIL / -18.2% / -1.8% | +73.6% at 29500 (18 s vs 10 s) FAIL | +4.7..+5.0% (end within 2 s after the last sample) |
| laptop-2026-11-09T02-12-06 | 8/8 done | pass (41 files) | pass | 943 | 755 | -35.3% FAIL / -15.6% / -4.5% | +146% at 29750 (10 s vs 4 s) FAIL | +2.4% |

Full runs: **PASS** (both). Estimates: **FAIL** on the laptop. Causes: (E1) the raw match prior
was right (65.7 s against 65 s), but the page scales waiting priors by a calibration factor
learned from the finished stages, and the first three (21 s in all) took it to about 0.65; (E2)
every sample passed until the last 1-2 s of training, where a relative bound cannot hold as the
remaining time goes to zero (a flaw in the criterion as written). E3 passed in both. Fix: the
calibration factor is shrunk towards 1 by 60 s of evidence (`pipeline/eta.js` `EVIDENCE_SEC`).

## Estimates v2 (pre-registered 2026-11-09T02:29:50Z, after v1 failed, before any run it judges)

A revision made after seeing the v1 results above; v1 stays recorded as failed. Judged on new
validation runs with the damped calibration factor: laptop runs 3 and 4, win11 run 2 (win11 run 1
calibrates, GPU idle as above). E1 and E3 unchanged. E2 revised: from 10% of the iterations on,
every remaining time shown is within +-15% or +-10 s (whichever is larger) of the actual. Each
validation run must pass E1, E2 and E3. Full-run criteria unchanged; laptop runs 3 and 4 also
count as full runs.

## Estimates v2 validation (judged as registered) and win11 calibration

win11 run 1 (calibration, GPU idle: 0% over the minute before): full run passes, 603 s wall, train
stage 417 s (training phase 407 s); desktop training prior set to 407 s.

| run | full run | E1 match / map / train | E2 (+-15% or +-10 s) | E3 | estimates |
|---|---|---|---|---|---|
| laptop-2026-11-09T02-30-55 (run 3) | pass | +0.7% / +5.8% / -1.4% | FAIL: 12 samples, 3500-9500 it, shown 453-559 s vs 586-696 s | +4.9% | FAIL |
| laptop-2026-11-09T02-46-38 (run 4) | pass | -3.0% / -16.7% / +9.4% | FAIL: 11 samples, 4250-9500 it | +3.8% | FAIL |
| win11-2026-11-09T02-44-14 (run 2) | pass | +5.6% / +5.7% / +1.0% | FAIL: 8 samples, 4750-10000 it, shown 277-308 s vs 336-396 s | +1.2% | FAIL |

E1 and E3 now pass on both machines (the damped factor fixed E1). E2 fails the same way on
both: from 10% to about a third of training the remaining time is 25-35% low. The training cost
curve was G1's; this build grows Gaussians only between 500 and 15000, so early iterations are
relatively cheaper. Fix: the curve refitted from the calibration runs (laptop runs 1-2, win11 run
1; the validation runs are not used): 0.117 of the time at 6000 iterations instead of 0.144.
win11 run 2 also failed G1 criterion 3 against G1's baselines (reported only; stage 6's rule uses
the page baselines).

## Estimates v3 (pre-registered 2026-11-09T03:04:11Z, after v2 failed, before any run it judges)

The only change from v2 is the training cost curve (above). E1, E2 (+-15% or +-10 s) and E3 as in v2.
Judged on new validation runs: laptop runs 5 and 6 and win11 run 3 (win11 GPU idle as before),
each of which must pass E1-E3; they also count as full runs.

## Estimates v3 validation (judged as registered)

| run | full run | E1 match / map / train | E2 (+-15% or +-10 s) | E3 | estimates |
|---|---|---|---|---|---|
| laptop-2026-11-09T03-04-40 (run 5) | pass | +1.0% / +3.2% / -1.0% | 0 of 70 outside | +5.2% | **PASS** |
| laptop-2026-11-09T03-20-21 (run 6) | pass | +2.1% / +4.0% / -1.9% | 0 of 70 outside | +4.6% | **PASS** |
| win11-2026-11-09T03-05-32 (run 3) | pass | +5.4% / +8.3% / +5.9% | 1 of 39 outside: 3500 it, shown 485 s vs 386 s (+25.6%) | +5.7% | **FAIL** |

Estimates v3: **PASS on the laptop, FAIL on win11** by one sample at the start of the judged
window (3500 of 30000 iterations), where the remaining time still leans on the prior while the
measured rate settles; every later win11 sample is within the bound. Not tuned further here.

Full runs (all 9 stage 8 full runs: laptop 1-6 and win11 1-3, run 1 the calibration run): every stage
done, verify() passes (41 files), downloads present, no page or console error. Totals: laptop
918-957 s (runs 3-6; 923 s median), win11 603-625 s with its GPU idle. Reported only: two runs fail
G1 criterion 3 against G1's reference-model baselines (win11 run 2, laptop run 6), and laptop run 5
kept one Gaussian more than 1 cm outside the bounds.

### R1 test mechanism (changed after the first recovery run, criterion unchanged)

The first recovery run polled for each stage to be running and never saw ingest or select (1-4 s),
waiting 30 min each. R1 now issues Stop from inside the controller's 'update' event in which a
short stage becomes running (through the page's test API, `api.testing.controller`, which only
exposes the existing controller); after codex's review, at the first progress report before the stage
completes instead, with its worker checked to be running; match, map and train stop after progress. Each wait's
timeout is twice the page's expected time to that point (at least 30 s), so a miss fails fast.

## Recovery results (laptop, as registered)

Final run (release 20261109T063219Z, `test/stage8/recovery.mjs`, all scenarios from a fresh job,
training 6000 iterations; `$WEBSPLAT_DATA/stage8/recovery-2026-11-09T06-32-29`): **all eight
pass**. R1 was rerun after codex's review on release 20261109T072810Z (Stop after the stage's first
progress, with a check that its worker is running; the rebuilt, path-remapped wasm):
`$WEBSPLAT_DATA/stage8/recovery-2026-11-09T07-28-52`, **pass**. The rows below are from those
two report files (codex found the first version of this table quoting an earlier run's R1 timings
and misdescribing R2's map checkpoint).

| scenario | message the page shows | verify() | completes |
|---|---|---|---|
| R1 Stop in every stage (rerun `recovery-2026-11-09T07-28-52`, see below) | each paused within 3.5 s (ingest 96 ms, select 374 ms, features 99 ms, match 0.8 s, map 3.1 s, prepare 14 ms, train 0.3 s, export 184 ms); the short stages were stopped mid-work with their worker running (ingest at 5.0 of 63.5 MB, select and features at frame 1 of 201, prepare at 0.3 of 201, export at 65536 of 363422); map resumed from round 1, train from 5250 | pass | yes |
| R2 reload during checkpoint writes | map and train each reopened interrupted, without the checkpoint that was being written (unpublished, so discarded; earlier published ones kept), and resumed | pass | yes |
| R3 storage full (prepare outputs; training checkpoint) | "browser storage is full (quota exceeded)" | pass | yes, after the fault is removed |
| R4 stale versions | older stage version: prepare and later redone, earlier kept; newer manifest: "this job was written by another app version (manifest version 2). Its data has been left as it is." with Delete | pass | yes (older stage version) |
| R5 two tabs | "This job is open in another tab or window. Close it there (or use that tab) and reload." | pass | yes (first tab) |
| R6 device lost | "training failed: device lost (destroyed): Device was destroyed." | pass | yes, Resume |
| R7 allocation refused | "training failed: GPU error at iteration 2000: Buffer size (...) exceeds the max buffer size limit" | pass | yes, Resume |
| R8 no telemetry | "this recording has no vehicle telemetry (position and attitude) in its 201 frames, so the cameras cannot be placed; record with the autopilot connected to the gimbal"; no later stage started | pass | (not expected) |

Found and fixed on the way (app): ingest now refuses a recording without telemetry (it went on and
would have failed in mapping); a failed training run's message named a refused preview snapshot
or the wasm panic that follows a lost device, not the cause (the Brush worker now announces the
failure cause at once, `failure` message; the stage ignores snapshot refusals from a failed run);
the ETA calibration factor (above). Harness: R1 stops short stages from the controller's events;
the harness waits for the page to offer Resume after a reload.

Not a recovery fault, for stage 7: in the previous full run (release 20261109T055323Z), R3's resumed
job failed at export with "the SPZ does not decode within its bounds: {"alpha":1}" (one Gaussian's
opacity, a 6000-iteration splat). The splat was not kept (the next scenario cleared storage); the
harness now keeps the trained splat whenever export fails. It did not recur in the final run.

Builds: the estimate runs used release 20261109T030424Z; the later changes (failure messages,
ingest's telemetry check, the test API) do not touch training or the estimates.

## Codex stage 8 review: fixes

1. `deploy.sh` built the release suffix with a command substitution that failed (and aborted the
   script under `set -e`) on a clean tree: now an `if`. Tested on a clean worktree copy (release
   `...-77fd82ecc065`, dirty_files 0) and on the working tree (`-dirty`).
2. Path scrubbing removed. The wasm builds remap their source paths: COLMAP with
   `-ffile-prefix-map` (colmap-wasm patch 0005, clean rebuild), Brush with `--remap-path-prefix`
   (build command in test/g1/README). `deploy.sh` now only checks: every file for the data
   directory and `$HOME` (byte search), every `.wasm` with `WebAssembly.validate`; either fails the
   release. The final release passes both and R1 ran on it.
3. Upload: rsync into `releases/<release>.partial/` on the target (unchanged files hard-linked
   from `current` by checksum), renamed when complete, then `current` switched with a symlink
   rename; interrupted uploads' partial directories and all but the newest 3 releases removed.
   Tested against a local target: repeated publishes, hard links, pruning, and an interrupted
   transfer (a failing rsync) leaving `current` on the previous release.
4. R1 stops a short stage at its first progress before completion and checks that a worker was
   constructed for the stage and is alive (a page-level Worker count); rerun above.
5. The notes' R1 and R2 rows corrected from the report files.

## Codex stage 8 review round 2: deploy fixes

1. Pruning keeps the newest 3 releases and never removes current's target or the release it
   replaced (`app/scripts/deploy-remote.sh`).
2. One deploy at a time: an atomic `mkdir` lock (`.deploy-lock` in the target), taken before the
   upload and removed on exit; a held lock refuses the deploy and changes nothing. The temporary
   symlink is per release (`current.<release>`). Under the lock any `.partial` is an interrupted
   upload and is removed.
3. Pages open across a switch: each release lists its own files (`.files`); the next release
   carries the live release's files forward (hard links, never overwriting). The Brush package
   moved to `vendor/brush/<wasm hash>/` (prepare-assets; the page gets the directory at build time
   through a vite `define`), since its URLs are not content-hashed; vite's assets already are.
   One release back is covered.

Tested against a local target from a clean worktree copy: release A published and served over
HTTP; release B (changed source and a different Brush wasm) published; every file an A page loads
lazily (23: hashed assets including 16 workers and wasm, and A's Brush package) fetched through
`current` afterwards, all 200 and byte-identical to A's. A held lock refused a deploy with current
unchanged. Rollback to the oldest of three releases and a new publish kept the rolled-back release
(outside the newest 3); a late upload with the oldest name became current and was kept.

## Deploy, simplified (after codex round 3)

Replaces the round 2 design (carry-forward `.files`, versioned Brush directory, mkdir lock):

- Releases pinned by `<base>`: each release is built normally into `releases/<release>/`; the
  site's `index.html` is that release's page with `<base href="./releases/<release>/">`, so every
  URL a page resolves (scripts, workers via `import.meta.url`, wasm, vendor files through
  `document.baseURI`; `app/src/asset-url.js` for the splat viewer, FFmpeg and Brush URLs) stays
  in its release. A switch replaces only `index.html` (temporary file + rename) and the `current`
  symlink. Old pages keep working as long as their release is kept. (Vite refuses a relative base
  other than `./`, hence `<base>` rather than a per-release vite base.) The versioned Brush
  directory and the vite define are reverted; `e2e.mjs` finds assets in a release root.
- Concurrency: uploads go to their own `releases/<release>.partial/` unlocked; the switch and
  prune run under `flock(1)` on `.deploy.lock`, held by the target-side process
  (`deploy-remote.sh`), released by the kernel if it dies. Partial uploads are removed once a day
  old.
- Pruning: the newest 3 releases, never the live release or the one it replaced, by resolved name
  (`basename "$(readlink -f current)"`).

Tested against a local target from a clean worktree copy: A, B, C published in turn, an A page's
35 lazily loaded files (assets/, vendor/) fetched after C at the URLs its `<base>` gives, all
intact; rollback to the oldest of three releases then a publish kept it; a deploy while another
process held the lock for 15 s waited and then switched; two simultaneous switches ran one after
the other with `index.html` and `current` agreeing; `kill -TERM` during a 51 MB upload left
`index.html` and `current` unchanged and no lock to clear, and the next deploy published.

Deploy target (user's decision): https://plot.ardupilot.org/websplat/, served by an nginx alias of
`/websplat/` to the deploy base directory (the `WEBSPLAT_DEPLOY_TARGET` path, which holds
`index.html`, `current` and `releases/`). The target side needs bash, coreutils (`mv -T`, `ln`,
`cp`, `readlink -f`, `basename`, `sort`, `head`, `rm`), findutils (`find -printf`) and util-linux
(`flock`), all required packages on Debian and Ubuntu, plus rsync, which minimal images may lack.
The final release (`20261109T081442Z`, with the coordinator's UI edits) passed R1 served from its
site root through `<base>` (`recovery-2026-11-09T08-14-52`), and an e2e full run started on it and
reached training before being stopped.
