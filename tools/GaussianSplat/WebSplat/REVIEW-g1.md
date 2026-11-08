# Codex review of G1 (thermal training in Brush), round 1, 2026-11-08

**G1 should remain open.** The thermal data path is convincing, but buffer lifetime safety and the laptop acceptance failure remain unresolved. Review was read-only.

1. **P1 — Four submits do not establish safe buffer lifetime.**  
   [CubeCL patch:69](app/src/stages/train/brush-patches/cubecl/0001-cubecl-wgpu-destroy-deallocated-buffers-a-few-submit.patch:69) destroys buffers solely by submit age. CubeCL’s prepared tasks retain `WgpuResource` buffer references after consuming the allocation bindings; those references can outlive pool deallocation. There is no invariant that every remaining consumer has submitted within four submits. Increasing the delay reduces observed failures without fixing ownership. Retain allocation ownership through queued consumers and destroy only after their final submission. This is a GPU use-after-destroy risk; the final clean runs do not prove safety.

2. **P1 — The declared pass contradicts criterion 3, and the variance evidence includes an invalid run.**  
   [notes/g1.md:60](notes/g1.md:60), [variance claim:81](notes/g1.md:81). I reproduced the final laptop median **18.51 C**, exceeding the permitted **14.51 C**. Moreover, the earlier passing laptop run records [ten GPU errors]($WEBSPLAT_DATA/g1/browser_laptop/log.jsonl:370), including [submission of a destroyed buffer]($WEBSPLAT_DATA/g1/browser_laptop/log.jsonl:41). Exclude it from the “six of seven pass” argument. Variance is plausible, but a harmless variance explanation is unproven; the current clean laptop build fails the gate. Repeat the unchanged final build and native baseline under a defined repeat policy before closing this.

3. **P1 — GPU failures do not invalidate the browser run.**  
   [brush-worker.js:93](app/src/stages/train/brush-worker.js:93), [harness page:77](test/g1/web/index.html:77). GPU errors are counted, but training still posts `done`; the page proceeds to render/export without checking `gpuErrors`. Shader errors likewise only produce messages. Reject these runs and prevent their artifacts from becoming acceptance evidence. Device loss should also stop training, rather than merely notify the page.

4. **P2 — The scorer can pass incomplete evidence and returns success on failed criteria.**  
   [metrics.py:59](test/g1/metrics.py:59), [export comparison:125](test/g1/metrics.py:125), [CLI:152](test/g1/metrics.py:152). Unchecked `zip` truncates short `.npy` inputs. An in-memory reproduction supplying only one of thirteen views made the export check pass. Validate exact view identities, counts, dimensions and finite values. Also return nonzero status for failed comparisons; the real laptop criterion-3 failure currently exits successfully. The browser driver similarly [always exits zero](test/g1/run_browser.mjs:133), including failed runs.

5. **P2 — Worker cancellation and disposal are missing.**  
   [brush-worker.js:113](app/src/stages/train/brush-worker.js:113), [message handler:162](app/src/stages/train/brush-worker.js:162). There is no stop/dispose operation, start guard, or cleanup on failure. Repeated starts also invoke `_initialize` again on the cached wasm instance and overwrite global `training` while the previous loop may still run. Completion leaves the training stream, optimizer resources and device alive for export, with no release afterward. Enforce a disposable, single-run worker contract or implement guarded cancellation and cleanup. This is an integration gap beyond the successful one-shot demonstration.

6. **P2 — Scene preparation silently corrupts PINHOLE inputs.**  
   [prepare_g1.py:105](test/g1/prepare_g1.py:105) unconditionally interprets four camera parameters as SIMPLE_RADIAL. `train.py` also supports PINHOLE, whose four parameters mean `fx, fy, cx, cy`. These pass unpacking but become incorrect intrinsics and distortion. With the canonical intrinsics expressed as PINHOLE, this produces approximately **411,450 px** maximum remap displacement. Mirror `load_scene`’s camera-model branch, including no remap for PINHOLE. The recorded SIMPLE_RADIAL run is unaffected.

The checks that held up:

- All **201 targets** are uint16 grayscale; maximum transport error is **7.66e-6**. The reviewed training path preserves f32 bits into the loss, with no RGBA8 conversion or gamma. Masks and LPIPS remain documented unsupported paths.
- Hot weighting matches native defaults: `(1 + hot_weight)` above the threshold, with SSIM unchanged. Replayed loss/gradient checks give about **6e-6 relative error**. This equivalence assumes native `hot_tolerance=0`.
- Frozen holdouts, scale, thermal initial values and canonical undistortion are consistent with scene loading.
- The **v2 worse-baseline rules are implemented correctly**. Rescoring native, Windows and laptop artifacts reproduces the recorded summaries.
- Exported metadata contains the correct thermal scale and ENU information. PLY comparisons pass: native **74.78 dB / 0.029 C**, Windows **75.03 dB / 0.078 C**, laptop **72.55 dB / 0.020 C**.
- Browser renders remain unquantised. Native saved eval images use 16-bit transport. The constructor fix works with the supplied worker; its claim that wasm-bindgen itself calls `_initialize` is inaccurate.
- Brush intentionally retains different learning rates, full-degree SH training, densification, noise, opacity handling and filtering; native cylinder/canopy pruning is absent. These are documented algorithm differences, so similar Gaussian counts alone do not establish equivalence.

I replayed scoring and saved loss checks and inspected the recorded logs; I did not rebuild or launch another full training run.

## Round 2

**Verdict: accept after fixes.** The supplied results numerically pass the repeat policy, but enforcement gaps remain:

- **P2 — [brush-worker.js:248]:** Exceptions bypass cleanup. A failed evaluation/readback also leaves the worker able to export.
- **P2 — [run_browser.mjs:141]:** After connection loss, an old verdict and artifacts can validate a new attempt. Empty, incorrectly named artifacts also pass its completeness check. Reproduced exit zero.
- **P2 — [score_repeats.py:41]:** Duplicate runs count repeatedly and invalid attempts have no enforced ceiling. One passing run supplied five times alongside four invalid attempts produces PASS.
- **P3 — [notes/g1.md:130]:** The aborted Windows series also contains run 3, stopped at iteration 12,000 without a verdict. Count and disclose it: three unsuccessful attempts, exhausting the replacement allowance.

Rescoring confirms laptop criterion 3 passes **4/5**, median **12.70 C ≤ 14.51 C**; Windows criterion 2 passes **4/5**. No quality-based cherry-picking was evident. CubeCL’s ownership/completion repair appears sound in the reviewed training path; the negative control triggers the original bug. Strict render validation and PINHOLE handling are fixed.

The **4,282 MiB** laptop peak remains below its 8 GB capacity; attribution to another process is unproven. I could not independently establish that the untracked repeat policy existed before the runs.

Full review and reproduction details: $WEBSPLAT_DATA/codex-g1/REVIEW-g1-round2.md. No tracked files modified.

## Round 3

**Verdict: accept G1. No remaining P1/P2/P3 findings in scope.**

The round-2 reproductions now fail safely. Worker cleanup/export blocking, all 12 driver cases, duplicate-run rejection, and invalid-attempt limits passed verification.

The series 4 policy snapshot matches its logged SHA-256 and predates the first attempt by 57 seconds. The change is fair: it checks adapter availability before training, retains series 3 as failed, and preserves quality thresholds. [repeat_preflight.sh:27] records attempts before invoking training; injected post-preflight device loss counts as invalid.

Rescoring confirms:

- **Laptop:** 5 valid runs; criterion 3 passes 4/5, median **12.70 C ≤ 14.51 C**.
- **Windows series 4:** 5 valid runs, zero preflight failures; criterion 3 passes 4/5, median **12.41 C**. All other criteria pass 5/5.
- **Windows series 3 remains failed**, with all four adapter failures recorded.

The laptop’s **4,282 MiB peak** remains below its 8 GB capacity; attribution to another process remains unproven.

Full review and verification details: $WEBSPLAT_DATA/codex-g1/REVIEW-g1-round3.md. No tracked files modified.
