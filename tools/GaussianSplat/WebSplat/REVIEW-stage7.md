# Codex review: stage 7 result

## Round 1

**Verdict: request changes. No P1 found; six P2 and two P3 findings.** Tracked files were unchanged. Line numbers below refer to the uncommitted tree; GitHub links use `pr-splat-web2`.

1. **P2 — Stop/reload can leave export files locked and prevent recovery.** [stream.js:121] runs the PLY loop synchronously, so its per-chunk stop checks cannot receive the stop message. At 2M Gaussians, forced termination left a partial generation locked; immediate Resume failed with `NoModificationAllowedError`. A retry after two seconds succeeded. Reloading after an SPZ chunk also failed during recovery cleanup, leaving the controller uninitialized. Yield between PLY chunks and make cleanup tolerate delayed handle release. Reproduction log ($WEBSPLAT_DATA/codex-stage7/stop-probes.log), reload log ($WEBSPLAT_DATA/codex-stage7/browser-probes.log).

2. **P2 — Recovery deletes the user-selected training input.** [main.js:159] stores it under `job/inputs/`, which recovery treats as an unpublished stage directory. Reload before the train stand-in finishes removes the file but preserves its configuration URL. Resume then fails with `NotFoundError`, without prompting for reselection. Store these inputs in a durable, recognized location.

3. **P2 — Failed SPZ validation still publishes a successful result.** [stage.js:51] only warns when `spz.over` is nonempty. Fault injection confirmed that out-of-bounds positions and `nonFinite` errors still return successfully, allowing publication and downloads. Reject invalid decoded output; expected format clamping is already accounted for separately.

4. **P2 — Large thermal results automatically select a potentially severely clamped SPZ.** [result.js:109] chooses SPZ solely from PLY size, regardless of radiometric clamping. The supplied evaluation shows **24.54 °C peak loss from clamping alone**, before quantisation. Prefer PLY whenever DC/SH values clamp, with SPZ as an explicit choice. Preserving Spark byte identity does not preserve Brush temperatures; adjusting SPZ would require a separately validated encoding/metadata change.

5. **P2 — Final download comparisons do not fail closed.** [run_tests.sh:87] uses `cmp … && echo …`. Bash’s `set -e` does not exit for a failed left-hand command here, so mismatched downloads can reach `ALL PASS`. The Windows loop has the same pattern. Use an explicit failing branch. Also, run13’s training→PLY error is reported without an acceptance bound.

6. **P2 — The planned COLMAP download is missing.** [result.js:41] exposes only PLY, SPZ and metadata. PLAN.md stage 7 explicitly includes the COLMAP model. Wire the mapping outputs into a downloadable model archive.

7. **P3 — SPZ verification still allocates the entire compressed file.** [stream.js:208] reads all SPZ bytes and copies them into a Blob. The measured 2M budget passes, but the “one chunk” memory claim is inaccurate: verification also grows with compressed output size. Feed the decompressor from bounded file reads.

8. **P3 — The inherited start-camera policy remains a poor result view.** [stream.js:107] preserves the native middle-hot-view policy. The Brush result opened at an ambient-looking **21.2 °C peak**, reproducing the reported issue. Choose a view with demonstrated hot-source visibility, or provide a hotspot-focused home view.

The core serialization checks were strong: whole-file comparisons passed for tiny/G1, and additional SH-degree 0–3 probes passed around both chunk boundaries, including a one-Gaussian final chunk. Source counts are known before writing headers; ordinary successful publication excludes scratch files and partial generations.

The stage 6 contract is feasible for Brush’s exported PLY with unchanged dataset poses. The stand-in is visibly labelled, but it is still registered in the normal thermal pipeline; this is fixture-assisted execution, not real training. Replacing it must invalidate existing stand-in outputs.

Viewer File/Blob handling passed alongside legacy ArrayBuffer/Uint8Array handling. Cross-origin parents and same-origin sibling frames were rejected. I found no new message-origin vulnerability or object-URL leak.

Node export peaked at **482 MiB RSS** for 2M Gaussians. Separate current-source browser checks passed using software WebGL, at **+392 MiB** during export; downloaded files matched Node outputs. Missing, duplicate and non-finite render controls were rejected. The supplied full runner stopped at evaluation because CUDA is unavailable, so the thermal metrics above are from the supplied cached evaluation. No Windows tests were run.

Review status and evidence index ($WEBSPLAT_DATA/codex-stage7/review-status.txt).

## Round 2

**Verdict: request changes. No P1; two P2 and one P3 remain.** Most round-1 fixes pass verification, but the claimed start-view improvement does not reach the actual viewer.

1. **P2 — The viewer still clips hot values, including PLY values.** [viewer.js:116] leaves Spark’s `accumExtSplats` disabled. Extended storage in `SplatMesh` does not prevent the packed accumulator from clipping evaluated colour before blending.

   At the new camera, the reviewed G1 PLY’s exhaustive browser peak was **23.36 °C**. Enabling extended accumulation in a review-only build raised it to **101.19 °C**, with identical input and camera. The supplied laptop download is byte-identical and its report also shows an ambient peak. Preserve thermal values through accumulation and add actual viewer acceptance: [page_check.mjs:365] currently accepts any numeric peak string; the 111.1 °C check uses gsplat.

2. **P2 — The SPZ loss label promises an unsupported upper bound.** [result.js:89] says “up to X °C lower”, but [stream.js:190] excludes Gaussians below 0.5 opacity and samples only 32 directions. A valid 0.49-opacity control reported **0.0 °C loss**, despite **3.84 °C directional emission loss**. Describe this as a sampled estimate with limited coverage, rather than an error bound.

3. **P3 — “Peak in view” is a sampled maximum.** [viewer.js:179] samples pixels for both percentiles and peak. In the extended-accumulator probe, the covered-pixel maximum was **101.19 °C**, but the UI reported **76.4 °C**. Scan all relevant pixels for the maximum; retain sampling for percentile ranges.

Verified fixes:

- Stop during 2M PLY export paused in **231–316 ms**; immediate Resume passed. Reload after SPZ chunks recovered cleanly, with resumed files matching Node outputs. Held-handle removal retries worked.
- Picked files survived reload under `source/inputs/`; missing files prompted reselection.
- SPZ bounds/non-finite injection durably failed the stage without publishing outputs. Deliberate `cmp` mismatch exited 1. The run13 gate passed cached reference renders and rejected deliberately corrupted renders.
- Clamped large results selected PLY; PLY remained the first download. Replaced object URLs were revoked.
- Downloaded COLMAP archives passed standard Python tar reading and byte comparisons. Ustar checksums, sizes, padding and terminators passed boundary tests. Names ≥100 bytes are explicitly rejected; there is no prefix/PAX support, which is sufficient for the fixed COLMAP filenames.
- SPZ verification used reads ≤1 MiB. Changing `standin-1` to numeric version `1` invalidated train/export generations through the manifest, and that invalidation survived reload.

Node 2M export peaked at **489 MiB RSS**. Browser export passed its memory check; software-WebGL screenshot capture timed out for 2M. CUDA evaluation was unavailable. Stage 6 was isolated, no Windows tests ran, and tracked files were unchanged.

Detailed evidence and logs ($WEBSPLAT_DATA/codex-stage7/round2/review-status.txt).

## Round 3

**Verdict: request changes. No P1; two P2 and one P3.** Tracked files were unchanged.

1. **P2 — The widened bound masks a fixable footprint distortion.** [viewer.js:131] inherits Spark’s `maxPixelRadius=512`. This squeezes broad, cool foreground Gaussians, reducing their occlusion and making hot sources read higher.

   My G1 start-view result was **118.90 °C versus gsplat’s 111.09 °C**, failing even the widened bound. Changing **only** the radius cap to 4096 produced **110.83 °C**.

   The diagnosis is supported by an independent pixel calculation matching gsplat within 0.00002 °C. Spark’s decoded parameters, colours and ordering predict 110.75 °C; alpha-cap controls had no effect. Eighteen contributing Gaussians exceed the 512-pixel cap. Fix the thermal footprint policy, then tighten and justify [viewer_compare.mjs:12], rather than widening around this error.

2. **P2 — Redeploying the viewer alone does not restore deployed assets’ trained rendering mode.** [viewer.js:370] deliberately keeps unmarked metadata on the antialiased path. That contradicts the unqualified deployment claim in [stage7.md:152].

   For run13 view `00140`, the new viewer reads:

   | Input/mode | Peak |
   |---|---:|
   | PLY, marked classic | 100.27 °C |
   | Same PLY, unmarked | 74.70 °C |
   | SPZ, unmarked legacy metadata | 73.94 °C |
   | Same SPZ bytes, sidecar marked classic | 99.11 °C |
   | gsplat classic reference | 100.67 °C |

   Viewer redeployment removes accumulator clipping, but legacy thermal metadata also needs migration. Site SPZ bytes can remain unchanged; standalone PLYs need their header updated or re-exported.

3. **P3 — Sparse scenes can still lose the peak readout.** [viewer.js:214] returns when no percentile sample finds coverage, before publishing the independently computed maximum. A valid one-Gaussian control covered one unsampled pixel at **92.02 °C**: the peak text stayed blank while the pointer reported **92.0 °C**. Publish the peak independently of percentile sampling.

The extended accumulator, kind-switch blur ordering, and 0.49-opacity sampling fixes work. RGB screenshots were pixel-identical to the old viewer, including after thermal/RGB switches; repeated software-WebGL timings showed no clear regression.

Both requested runners were invoked but stopped at CUDA-dependent checks because CUDA is unavailable here. Separately, stage 7a’s browser comparisons against cached gsplat references and full-size run13 Node checks passed. Stage 7 Node/export/recovery checks passed; its viewer and page checks failed the G1 temperature bound above. No Windows tests ran; hardware performance remains unverified.

Detailed evidence and logs ($WEBSPLAT_DATA/codex-stage7/round3/review-status.txt).

## Round 4

**Verdict: request changes. No P1; five P2 findings.** Tracked files were unchanged.

1. **P2 — Half-float fallback silently exceeds the accuracy bound.** [viewer.js:246] falls back without indicating reduced precision. A valid 2,000-Gaussian blending control reads **92.1 °C with Float32, but 79.9 °C with half-float**, against a 92.13 °C compositing reference and 1.31 °C bound. Existing tests do not force this branch. Gate accurate thermal rendering appropriately or define and expose a tested degraded mode.

2. **P2 — Float32 selection omits texture-filtering support.** [viewer.js:250] retains linear filtering without checking `OES_texture_float_linear`. Masking that extension produces an **entirely black image**, although the framebuffer and peak remain correct. Select supported filtering, such as nearest filtering, or handle this capability explicitly.

3. **P2 — A failed migration cannot resume correctly.** [migrate-classic.mjs:127] marks metadata before processing its files. A corrupt SPZ leaves metadata marked classic; after repairing the SPZ, retry reports success with zero changes, leaving the PLY unmarked and SPZ flag unchanged. Individual renames are atomic, but completion is unsafe. Preflight the files and publish the metadata marker last.

4. **P2 — Named sidecars can modify an unrelated RGB asset.** [migrate-classic.mjs:131] prefers `splat.spz` over `NAME.spz`. With `thermal.json/thermal.spz` beside an RGB `splat.json/splat.spz`, migrating `thermal.json` changes the **RGB SPZ** and leaves the intended thermal SPZ unchanged. Match paired names or refuse ambiguity.

5. **P2 — Unknown headers are rewritten instead of refused.** [spz-flags.mjs:6] only gunzips and accesses byte 14. Bad magic, version 999, a 15-byte payload and truncated data all returned success and were rewritten. The [PLY check] also accepted an invalid preamble/format. Validate supported headers and lengths before modifying anything.

The normal Float32 fixes pass: **G1 111.0 versus 111.09 °C**, run13 `00140` **100.5 versus 100.67 °C**, and the one-pixel control publishes **92.0 °C**. RGB screenshots remain pixel-identical, including after kind switches. Happy-path migration preserves PLY bodies and SPZ decoded bytes except the flag; dry runs, repeat runs and ordinary RGB/already-marked skips pass.

Stage 7 browser/page/recovery checks and stage 7a browser/full-size Node checks passed. Both full runners stopped at CUDA-dependent steps; browser comparisons used cached gsplat references and local software WebGL. Hardware performance and Windows behaviour were not independently verified.

Detailed evidence and logs ($WEBSPLAT_DATA/codex-stage7/round4/review-status.txt).

## Round 5

**Verdict: accept after fixes. One P2; no P1/P3 findings.**

- **P2 — Migration permits conflicting rendering modes.** [migrate-classic.mjs:124] skips an explicitly antialiased PLY, but [line 178] continues to clear the SPZ flag and mark its unmarked sidecar classic. The reproduced thin-disc readout becomes **49.4 °C with the sidecar versus 42.4 °C from the PLY header**. Refuse the whole run before writes when markers conflict; only an already-classic PLY should count as a completed resume step.

The requested round-4 repros now pass: half-float warning and ≈ marks, masked filtering, corrupt-SPZ recovery, correct name pairing, and invalid-header rejection. PLY bodies and other decoded SPZ bytes remain unchanged. RGB screenshots are pixel-identical.

Both runners stopped at unavailable CUDA. Node, CPU and separate browser checks passed against cached references, including recovery. The 2M screenshot required a longer timeout under SwiftShader; export memory peaked at **+487 MiB**. No Windows tests or tracked-file edits.

Full findings and evidence ($WEBSPLAT_DATA/codex-stage7/round5/REVIEW.md).

## Round 6

**Verdict: accept.** No remaining P1/P2/P3 findings in this focused review.

The original conflict repro now refuses before writing and leaves all files unchanged. All **32 supplied migration assertions** and **36 independent marker combinations** pass, covering conflicts, resume states, consistent antialiased skips, idempotence and byte preservation.

No tracked files modified.

Review and evidence ($WEBSPLAT_DATA/codex-stage7/round6/REVIEW.md).
