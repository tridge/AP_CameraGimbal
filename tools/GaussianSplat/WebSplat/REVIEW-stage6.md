# Codex review: stage 6 (with the final stage 7 SPZ change)

## Round 1 (2026-11-09)

**Request changes: six P2 findings and one P3.** The recorded series 2 passes its numerical acceptance rule, but the claimed SH and resume semantics have correctness gaps.

1. **P2 — Inactive SH bands still receive gradients and Adam updates.** [Patch 0014:58](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0014-brush-train-train.py-s-SH-degree-schedule-sh-degree-.patch:58) masks through `t.inner()` and then creates a new gradient leaf. The mask therefore does not constrain backward propagation; rendering still uses the full SH degree. Adam repopulates inactive coefficients and moments after they were cleared. These values can reach refinement and checkpoints, and survive when a band becomes enabled. The integer schedule matches `min(step // 1000, 3)`, but this implementation does not hold unused coefficients and moments at zero. The reduced SH second-moment calculation also includes inactive gradients.

2. **P2 — Checkpoint parameters and iteration labels can disagree.** [brush-worker.js:279](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-worker.js:279) labels snapshots with the last reported iteration while the next `trainSteps(50)` is already running. Rust exports the latest splat slot, which may have advanced within that batch. An asynchronous mock running the actual worker JavaScript reproduced **label 5000, parameters from iteration 5001**. Resume consequently repeats already-applied training steps and misaligns LR, SH and refinement schedules. Capture parameters and their iteration together at a training boundary.

3. **P2 — “Raw” checkpoints normalize trainable quaternions.** [Patch 0013:176](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0013-WebSplat-stage-6-bounds-pruning-absolute-means-LR-st.patch:176) routes checkpoint export through the ordinary PLY serializer. Its `read_splat_data()` normalizes quaternions. Although orientation initially remains equivalent, quaternion magnitude affects subsequent angular updates, so resume changes more than the documented optimizer/statistics reset. Preserve raw quaternion components in the checkpoint path.

4. **P2 — Spatial pruning stops before training finishes.** [Patch 0013:289](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0013-WebSplat-stage-6-bounds-pruning-absolute-means-LR-st.patch:289) performs bounds pruning only inside Brush’s refinement hook. The existing stream disables that hook after 95% progress: with 30,000 iterations and period 200, the last check is at step 28,400. This does not implement the documented every-200 spatial checks through training. Inspected series 2 final splats contain **24–32 Gaussians outside the floor/ceiling bounds**. Run spatial pruning independently of densification’s cutoff.

5. **P2 — Growth does not actually stop at 15,000.** [brush-dataset.js:268](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-dataset.js:268) maps the claimed cutoff to `growth-stop-iter`, but Brush applies that flag only to gradient-selected growth. Forced screen-size splitting continues. The first laptop series 2 log grows from **797,784 Gaussians at 15,250 to 798,150 at 28,750**. Gate the remaining splitting paths, or explicitly document and evaluate this additional deviation.

6. **P2 — The policy scorer does not enforce run validity or distinctness.** [score_policy.py:50](tools/GaussianSplat/WebSplat/test/stage6/score_policy.py:50) scores supplied render paths without checking distinct attempts, completed training output, dataset hashes, configuration, or the invalid-attempt allowance. Supplying the same passing directory five times produces **PASS, 5/5**. The reported series contains distinct valid runs, so this does not overturn its result, but the scorer can incorrectly certify later series.

7. **P3 — The training preview disappears on ordinary progress.** [stage-list.js:38](tools/GaussianSplat/WebSplat/app/src/ui/stage-list.js:38) removes the image whenever `extra.preview` is absent. Regular training progress supplies only `gaussians`, so the next message removes the preview. Reproduced with the actual preview function. Retain the last preview while training remains active.

The acceptance evidence checks out with one chronology qualification: the original rule was recorded at **16:23:07Z**, before the new baseline runs began at **16:24:08Z**, but after earlier candidate results existed. Series 2 was prospectively registered at **18:52:17Z**, before its first runs at **18:52:53Z**. Both registration sections remain unchanged from their recorded edits. Rescoring all ten artifacts reproduces the notes: laptop every criterion 5/5; Windows criteria 3 and 5 each 4/5; all medians within bounds. Dataset hashes and complete outputs match.

The final **SPZ bound change passes focused review**. All three regression splats reproduce the old failures—1, 2 and 1 scale values—and pass the revised bound. Another **131,070 boundary probes** passed; deliberate scale corruption remained rejected. I found no additional blocker in the asset-preparation changes or the stage 6 → 7 file layout.

Stage 6 unit checks passed. OPFS checkpoint publication ordering appears sound by inspection. Live GPU-loss, crash/reload and Windows browser tests were not rerun. No files were modified.

## Round 2 (2026-11-09)

**Request changes. One P1 and five P2 findings remain.** Series 3’s reported **FAIL is computed correctly**.

1. **P1 — Bounds pruning drops all Adam state after every refinement.** [Patch 0015:242](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0015-WebSplat-stage-6-SH-schedule-by-render-slice-refine-.patch:242) calls both `Option::take()` operations before matching them. Immediately after `refine()`, `refine_record` is `None`, so this branch returns after removing—and dropping—the optimizer. With the default periods, this happens every 200 steps below 15,000. The bounds check is skipped, and the next training step recreates Adam with empty moments and time counters. The supplied pruning test never exercises **step → refine → prune_bounds**.

2. **P2 — Resuming after 15,000 permanently loses the 3D-filter floor.** [train.stage.worker.js:73](tools/GaussianSplat/WebSplat/app/src/stages/train/train.stage.worker.js:73) restores the unbaked parameters through PLY. Neither that checkpoint nor WSSP stores `min_scale`; loading initializes it to `None`. The floor is restored only during refinement, which now stops at 15,000. Consequently, later resumes train and export different scales/opacities from the saved model, beyond the documented optimizer/statistics reset. Preserve or explicitly restore the filter state.

3. **P2 — Refinement still runs before the claimed start window.** [Patch 0015:23](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0015-WebSplat-stage-6-SH-schedule-by-render-slice-refine-.patch:23) adds the stop gate but no start gate. `growth-start-iter=501` gates gradient-selected growth inside `refine()`, while opacity pruning, replacement, screen-size splitting and decay still execute at steps **200 and 400**. This does not match the documented `500 < step < 15000` window. The stop gate itself correctly prevents refinement from step 15,000 onward.

4. **P2 — Bounds pruning silently retains everything when every Gaussian fails.** [Patch 0015:248](tools/GaussianSplat/WebSplat/app/src/stages/train/brush-patches/brush/0015-WebSplat-stage-6-SH-schedule-by-render-slice-refine-.patch:248) only removes points when `kept > 0`. With zero survivors it returns the complete original model and reports zero removals. An exhausted model should be handled explicitly, including failing training if empty models are unsupported. The current test covers partial removal only.

5. **P2 — Malformed attempts abort scoring instead of consuming the invalid allowance.** [score_policy.py:60](tools/GaussianSplat/WebSplat/test/stage6/score_policy.py:60) parses attempt metadata without catching errors; subsequent required-field accesses are also unguarded. A truncated `report.json` reproduced an uncaught `JSONDecodeError`. Such an interrupted attempt prevents processing its valid replacements instead of being counted among the permitted invalid attempts.

6. **P2 — GPU-idleness validation accepts missing evidence.** [e2e.mjs:54](tools/GaussianSplat/WebSplat/test/stage6/e2e.mjs:54) ignores `nvidia-smi`’s exit status, interpreting empty stdout from a failed query as idle. [score_policy.py:63](tools/GaussianSplat/WebSplat/test/stage6/score_policy.py:63) also accepts a laptop report with absent or null `gpuBefore`; both cases reproduced as valid. Record successful query status and require its evidence for laptop runs.

The other fixes check out: snapshots are serialized at batch boundaries, checkpoint quaternions remain raw, and previews survive ordinary progress. SH slicing now blocks inactive coefficient gradients and keeps their values and first moments zero. Its test passes with the control. One qualification: Brush retains a **shared second moment per splat**; the test checks first moments, so the blanket claim that every inactive band’s Adam moments remain bit-zero overstates what is represented and tested.

All **15 patches applied exactly in memory** from the pinned upstream revision and reproduced the tree of **`8dcc0ad50182f028283ec38fa3c6c00410fcb536`**.

The gsplat opacity-reset claim is correct for the installed **1.5.3** source at [default.py:195](tools/GaussianSplat/.venv/lib/python3.13/site-packages/gsplat/strategy/default.py:195). The expression is a chained comparison:
`(step % reset_every == (0 & step)) and ((0 & step) > 0)`, which is always false.

Series 3’s registration section is unchanged from **21:56:34Z**; launch commands followed at **21:56:44Z/47Z**, with first runs at **21:56:50Z**. The scorer is unchanged from before registration, and the actual saved build hashes reproduce `bea773269f541ce6`. Rescoring both complete ledgers gives:

| Machine | Hot peak | Hot IoU | Other criteria | Decision |
|---|---:|---:|---:|---|
| Laptop | 4/5 | 3/5 | 5/5 each | **FAIL** |
| Windows | 3/5 | 3/5 | 5/5 each | **FAIL** |

Every median is within bounds. The failures correctly arise from the required pass counts. The identified scorer gaps do not alter these ten supplied attempts’ results.

JavaScript tests and existing native SH, pruning and quaternion test binaries passed; native tests used software Vulkan. No live Windows/page training rerun or file edits were performed.

## Round 3 (2026-11-09)

**Accept. No remaining P1/P2/P3 findings in the requested scope.**

All six round-2 fixes check out:

- Bounds pruning preserves Adam state and handles an absent refinement record.
- Resume restores the 3D-filter floor, including after step 15,000.
- All refinement is gated to `500 < step < 15000`.
- Pruning the entire model fails training.
- Unreadable attempt metadata counts as invalid.
- Failed GPU queries produce null evidence; missing/null laptop evidence is rejected.

The revised SH description accurately distinguishes zero inactive coefficients/first moments from Brush’s shared second moment.

Patches **0001–0016**, applied strictly in memory, reproduce the exact tree of **`3b901675f7c347f273a7167aa2e75670a5b5bf61`**.

[Series 4’s registration](tools/GaussianSplat/WebSplat/notes/stage6.md:402) is unchanged from the recorded edit at **00:19:31Z**. Launch commands followed at **00:19:38Z/41Z**, and both first runs started at **00:19:47Z**. The scorer matches its recorded pre-series version. Actual build hashes and all ten reports agree on **`cd449d1b2ede4dc3`**.

Rescoring both complete ledgers reproduces the reported results:

| Machine | Hot peak | Hot IoU | Other criteria | Decision |
|---|---:|---:|---:|---|
| Laptop | 5/5 | 5/5 | 5/5 each | **PASS** |
| Windows | 5/5 | 4/5 | 5/5 each | **PASS** |

All medians satisfy their bounds; both ledgers contain five distinct valid runs and zero invalid attempts.

Five native training regression tests, the raw-quaternion test, JavaScript tests, and the ledger regression passed. Native tests used software Vulkan. I found no remaining correctness issue in the reviewed stage 6 → 7 handoff.

No live browser/Windows training rerun was performed. No files were modified.
