# Stage 7: result (export stage and result view)

Tests: `test/stage7/run_tests.sh` (`WIN11=1` adds the win11 page run). Outputs in
`$WEBSPLAT_DATA/stage7`.

## What it does

- `app/src/stages/export/stage.js`: the `export` stage, last in the 'thermal' set (inputs select,
  prepare, train; config key `export` = {title, description}; unit Gaussians). Two workers in turn
  (`export.stage.worker.js`, OPFS sync access handles):
  1. PLY pass: reads stage 6's splat (Brush PLY in any property order, or WSSP params.bin) and
     writes `splat.ply` 65536 Gaussians at a time, greyscale SH and quaternion normalisation per
     chunk (`stream.js` `plyPass`), header with the start camera from 5b's views.
  2. SPZ pass: Spark encodes 131072 Gaussians at a time; each chunk's planar sections go to their
     places in a scratch file, each chunk is decoded and checked against its PLY rows
     (`spzErrors`, merged), then the scratch file is gzipped as a stream (the fixed gzip of 7a)
     and read back to verify; then `meta.json`. Spark quantises each Gaussian on its own, so the
     chunked file is byte-identical to Spark's whole-file SPZ (checked on G1's run, the tiny
     model and run13). Spark's memory goes with its worker.
  Stop is checked per chunk; a stopped export restarts from the beginning (it takes seconds).
- Stage 6 input: training is not in the app yet. CONTRACTS.md 6-7 now defines what the export
  consumes (`train/gen-<g>/train.json` + `splat.ply` | `params.bin`). The 'train' stand-in
  (`standin.js`) loads a trained splat into that layout: from `?standin=URL` (a JSON of file
  URLs; the harness) or, when the pipeline reaches it without one, a file the user picks (copied
  into the job's storage). The same stand-ins (select, prepare, train) make the 'export-test' set,
  which runs the export from prepared files without a recording (`registerStageSet(..., {source:
  false})`; main.js gives such sets a placeholder source).
- Result view (`app/src/ui/result.js`, `#result` in index.html): the embedded viewer
  (`embed.js`; the viewer now accepts a Blob/File, so the page passes the OPFS file itself)
  showing the PLY (see round 1 item 4 for when the SPZ opens), download links for PLY / SPZ /
  meta.json / COLMAP model (OPFS files as blob URLs, named after the title), and a summary:
  Gaussians, SH degree, temperature scale and display range, render mode, per-stage times and
  the total, and what the SPZ loses (including how many values it clamped).
- Harness: `test/harness/lib/serve.mjs` takes extra files and sends Content-Length;
  `thermal.mjs` feeds the training stand-in (`--trained`, default G1's laptop browser export),
  so the full thermal run now goes through export.

## Results

Inputs: G1's laptop browser run (`g1/final_laptop`: Brush PLY, 594,940 Gaussians, eval renders),
run13 (train.py, 591,039, lobe baked), a synthetic 2M-Gaussian Brush-layout PLY, the 7a tiny
model; dataset.json = 5b on the native reference model, frames.json = the reference.

Exports (all byte-identical where compared): streaming = in-memory 7a export (tiny, G1); page
downloads = stage outputs (manifest hashes) = Node exports, laptop and win11.

| | Node | laptop page | win11 page |
|---|---|---|---|
| G1 595k: time | 7.5 s | 6.8 s (PLY 1.5, SPZ 5.3) | 11.6 s incl. a Stop/resume |
| G1 595k: memory | 438 MiB RSS | +466 / +467 MiB | +390 / +441 MiB |
| synthetic 2M: time | 24 s | 21.9 s (PLY 4.6, SPZ 17.2) | 25.5 s (PLY 7.0, SPZ 18.5) |
| synthetic 2M: memory | 465-472 MiB RSS | +422 / +520 MiB (largest process 549 MiB) | +234 / +548 MiB (sampled 1/s) |

Page memory: all Chrome processes of the test profile above the idle page, peak during the
export (laptop /proc RSS every 250 ms, win11 working sets every second; two runs each). The tests
require at most 1024 MiB. Before the chunked SPZ
the 2M export peaked at +1032 MiB in the SPZ pass (Spark holding the whole splat); the 7a
in-memory export measured 1.84 GiB.

Full thermal pipeline in the laptop page (MKV to downloads, `thermal.mjs`, training stand-in):
all stages done, 183.6 s wall, export 6.9 s, no console errors.

Held-out evaluation (`evaluate.py`, G1's `metrics.py` imported; frozen 13 views):

| | cold PSNR | hot PSNR | hot peak err (median) | hot IoU |
|---|---|---|---|---|
| G1 Brush: training (eval renders) | 42.00 | 37.24 | 18.51 C | 0.283 |
| G1: exported PLY | 42.01 | 37.25 | 18.50 C | 0.283 |
| G1: PLY with SPZ's ranges | 42.01 | 37.23 | 25.57 C | 0.187 |
| G1: SPZ | 38.41 | 36.55 | 26.43 C | 0.177 |
| run13: training (splat.pt with lobe, gain 1) | 39.16 | 36.78 | 5.65 C | 0.270 |
| run13: exported PLY | 39.10 | 36.78 | 5.61 C | 0.270 |
| run13: SPZ | 35.46 | 35.74 | 6.06 C | 0.283 |

Steps: Brush -> PLY 72.5 dB min, 0.03 C (G1 criterion 9 passes); run13 lobe projection -> PLY
53.8 dB, 0.09 C; SPZ quantisation (PLY with SPZ's ranges -> SPZ) 38.1 / 35.4 dB, 2.4 / 2.6 C.
SPZ clamping: G1's splat has 72 SH coefficients beyond +-1 and 48 DC values beyond the colour
byte (hot sources); clamping them takes up to 24.5 C off a hot peak (view 00140). run13 clamps
only scales (no effect).

Acceptance: the downloaded PLY opens with temperatures in the viewer's open-a-file page (the
build deployed at plot.ardupilot.org/splat/, from the app's copy) on the laptop and win11:
thermal controls, title from the header, readouts (e.g. G1 "peak 21.0 °C in view" at the start
camera).

## Changes outside WebSplat/

- `viewer/src/viewer.js`: a posted Blob/File is read by the viewer.
- `viewer/src/viewer.js` (rounds 2-3): thermal splats use their own Spark renderer (extended
  accumulator, alpha threshold 1/255, depth order, footprints to 4096 px) and a float32 target;
  the peak in view scans all pixels and is published on its own.
- `viewer/spz-flags.mjs` (shared by ply2spz.mjs) and `viewer/migrate-classic.mjs` (round 3).
- `viewer/public/viewer.css`: `[hidden]` wins over the panel's `display: block` (the up-axis
  label was always shown, also on the deployed pages).

## Round 1 review fixes (REVIEW-stage7.md)

1. Stop and reload. The PLY and SPZ loops yield a macrotask after every chunk, so Stop lands
   between chunks (2M PLY pass: paused 210 ms after Stop). `opfs.remove` retries for about 10 s
   while a stopped or reloaded worker still holds an access handle, which makes recovery cleanup
   and an immediate Resume work. Tests (`recovery_check.mjs`): Stop during the 2M PLY pass and
   Resume at once; reload during run13's SPZ pass after a chunk (no errors, partial generation
   removed, Resume finishes); both give the Node export's bytes.
2. A training file chosen in the page is stored under `source/inputs/` (recovery keeps `source/`);
   if it is missing before the train stand-in has loaded it, the page asks for it again
   (`pick` scenario).
3. A decoded SPZ beyond its bounds or not finite fails the export stage; format clamping stays in
   `spz.clamped`.
4. The result view opens the PLY whenever SPZ clamped colour values (and otherwise unless the PLY
   is over 250 MB); the PLY is the first download ("exact"), the SPZ is labelled with the measured
   loss: the export now compares each Gaussian's emitted temperature (opacity >= 0.5, 32
   directions) between PLY and SPZ (G1: up to 10.6 C lower, 82 Gaussians more than 2 C; rendered
   hot peaks lose more, 24.5 C in the evaluation). The SPZ encoding is unchanged.
5. `run_tests.sh` compares files with a failing branch (`same`), and run13's training -> PLY
   (lobe projection) is bounded: >= 50 dB, <= 0.5 C (measured 53.8 dB, 0.09 C).
6. COLMAP model download: map's final model as a tar (`sparse/0/*.bin`), a Blob of the OPFS
   files and tar headers (nothing copied into memory); the 'export-test' set has a map stand-in
   (the reference model) and the test checks the archive's files.
7. The SPZ read-back feeds the decompressor from 1 MiB file reads.
8. Start camera: the hot view with the highest recorded frame maximum (frames.json `maximum_c`),
   with export_web.py's geometry. Rendered at the viewer's canvas (`start_view.py`): G1's PLY peaks
   at 111.1 C from it against 24.0 C from export_web.py's middle hot view. export.js (7a) keeps the
   native policy. Suggestion: export_web.py should change the same way (view_splat.py's start too);
   it would need the frame maxima (frames.json) next to splat.pt.
Stand-in marker: stand-in stages have version `standin-1` (no real stage has such a version) and
write `standin: true`; when stage 6 replaces the stand-in, its outputs and the exports made from
them fail validation and are redone. The export result records `trainStandin`.

## Round 2 review fixes

1. The viewer clipped hot values. Spark's packed accumulator clips evaluated values before
   blending (G1 at the stage 7 start view: 23 °C in the browser where gsplat renders 111 °C).
   Thermal splats now get their own Spark renderer with the extended (float) accumulator,
   gsplat's alpha threshold (1/255) and depth (not radial) order; other splats keep Spark's
   defaults (same code path as before). A related bug: on a kind change the new renderer was made
   after the classic blur settings, which then went to the old one; the renderer is now made
   first. Measured against gsplat classic at the same camera and size (`view_check.mjs`,
   `viewer_reference.py`), viewer peak / gsplat peak:
   - G1 start view: 114.3 / 111.1 °C (before: 23–95 °C depending on the build);
   - run13 held-out view 00140 (640x512): 97.6 / 100.7 °C (packed: 72 °C);
   - G1 at 00140: in run_tests; tiny model 95.5 / 95.55 °C; DC-only G1 29.9 / 29.55 °C.
   The residual left then (0.012 laptop, 0.023 win11) came from the 512 px footprint cap and the
   half-float target, fixed in round 3; the bound, briefly widened to 0.025, is 0.005 again. For the
   peak in view and the readout at gsplat's hottest pixel, in `view_check.mjs` and in page_check's
   acceptance of the downloaded PLY (laptop and win11). Laptop cost, G1 PLY: load 1.20 s (packed
   1.15 s), 40-step orbit 1.5 s (1.8 s), Chrome RSS +490 MiB (+498 MiB).
2. The SPZ loss is labelled as what it is: a sampled estimate (32 view directions per Gaussian,
   now every Gaussian, whatever its opacity), not a bound.
3. "Peak in view" is the hottest covered pixel over all pixels; the auto range percentiles stay
   sampled.
## Round 3 review fixes

1. Thermal footprint and target. Spark caps a splat's footprint at 512 px, which shrinks broad
   near Gaussians (cool foreground occluders, 18 of them in G1's start view), and the viewer's
   half-float target drifted when thousands of Gaussians blend into one pixel. Thermal splats now
   use footprints up to 4096 px and a float32 target where float blending is available
   (`EXT_color_buffer_float` + `EXT_float_blend`; else half float). Viewer / gsplat peak, laptop:
   G1 start view 110.5 / 111.1 °C, run13 00140 100.7 / 100.7 °C, G1 00140 88.4 / 88.3 °C, tiny
   95.6 / 95.55 °C, G1 DC-only 29.5 / 29.55 °C: at most 0.002 of the scaled value. The bound is
   back at 0.005 + 0.1 °C (stage 7a's), checked by `view_check.mjs` and page_check on both
   machines (win11: G1 start view 110.5 / 111.1 °C, as on the laptop). Laptop cost (G1 PLY, 1280x900,
   frame intervals while orbiting): median 16.7 ms both, p90 33 ms with the float target (16.8 ms
   half), close zoom 16.7 ms both; the 4096 px cap costs nothing measurable; RSS +70 MiB.
2. Deployment, corrected: a rebuilt viewer alone renders an unmarked (pre-marker) thermal splat
   on Spark's antialiased path, still low at hot sources (run13 00140, codex: unmarked PLY 74.7 °C,
   marked 100.3, gsplat 100.7). `viewer/migrate-classic.mjs` marks existing exports
   (`--write`; default is a dry run): meta.json gains `"rasterize_mode": "classic"` (inserted
   after the opening brace, the rest of the text unchanged); a PLY's `comment apcg_splat` gains
   the field (header-only rewrite, body bytes copied unchanged); an SPZ has its antialiased flag
   cleared, re-gzipped as ply2spz.mjs now writes it (decoded bytes identical but the flag; the
   result equals a fresh export_web.py SPZ). RGB splats are left alone unless `--kinds
   thermal,rgb`. Tested only on local legacy copies in `$WEBSPLAT_DATA/stage7/migrate-test`
   (`migrate_check.mjs`; nothing deployed was touched). For each deployed thermal run a
   deployment would change exactly: `meta.json` (+30 bytes), `splat.ply` (+27 header bytes; for
   run13 146,579,962 -> 146,579,989), `splat.spz` (same size or a byte different, new hash); the
   viewer's sample pairs (`samples/NAME.json` + `.spz`) the same way. The migrated run13 PLY renders
   within the bound of gsplat at 00140. Then rebuild and deploy the viewer.
3. The peak in view is published whenever any pixel is covered, independent of the percentile
   sample.

## Round 4 review fixes

1. Degraded precision is shown: without float32 blending (`EXT_float_blend`) the viewer falls
   back to a half-float target and now says so in the thermal panel ("Reduced temperature
   precision on this GPU ... approximate"), and marks the peak and readouts with "≈". Tested by
   hiding the extension (`view_check.mjs --mask EXT_float_blend --degraded`): notice and marks
   present; the error is reported, not bounded (laptop: G1 start view -4.1 °C, run13 00140
   -3.1 °C).
2. Without `OES_texture_float_linear` a float32 target is sampled black with linear filtering;
   the targets then use nearest filtering (the full-size target maps pixel to pixel, so the
   result is the same). Tested with the extension hidden: within the normal bound and a lit
   image; the old code under the same mask gives a black image (negative control).
3-5. `migrate-classic.mjs` now checks every file of a run before changing any (JSON; SPZ gzip,
   magic, version, header length, decoded length; PLY preamble, binary little-endian format, a
   single vertex element of scalar properties, file length; PLY kind against the sidecar's) and
   refuses the whole run on any problem (exit 1, nothing changed). It writes the PLY and SPZ first
   and the meta.json marker last, so an interrupted run is finished by the next one. Files are
   paired by name only (meta.json with splat.*, NAME.json with NAME.*). Round 5: a run whose
   markers conflict (an explicit non-classic mode in the sidecar or PLY header, or an SPZ flag,
   disagreeing with the others) is refused before any write; only a classic PLY or a cleared SPZ
   beside an unmarked sidecar counts as a finished step of an interrupted run. Tests
   (`migrate_check.mjs`): a run interrupted before the SPZ, then completed (same files as an
   uninterrupted run); an unrelated splat.spz beside NAME.json left alone; ten invalid inputs
   (SPZ not gzip, bad magic, bad version, short header, truncated; PLY bad preamble, ascii,
   truncated, no end_header, kind mismatch) and four marker conflicts each refused with every
   file unchanged; a consistently antialiased run is skipped untouched.

## Stage 6 splats and the SPZ check

Stage 6's trained splats failed the export's SPZ check on 1-2 of about 790k Gaussians: a log
scale decoded 0.5 steps + half a float16 ulp + 1 float32 ulp away (e.g. -0.8439942 stored as byte
147, -0.8125, where the float16 rounding of -0.8439942 is -0.84424, byte 146). The values are in
range and nothing is NaN: Spark holds scales linearly and takes ln again before staging ln(scale)
as float16 (its JS twin `encodeExtSplat` does `toHalf(Math.log(scaleX))`), and that float32 round
trip moved a value sitting one float32 ulp above a float16 tie below it. The bound now allows two
float32 ulps of ln(scale) beyond the half step and half float16 ulp; the two stage 6 runs are in
`run_tests.sh` as regression cases (both export with nothing over bounds).

## Open issues

1. SPZ is lossy beyond quantisation for Brush splats: values outside SPZ's ranges are clamped
   (G1: 24.5 C lost at a hot peak). The PLY is exact; the result view says so and counts the
   clamped values. A fix would need SPZ with wider SH ranges (not in the format) or keeping hot
   Gaussians' SH within +-1 in training.
2. export_web.py still starts at the middle hot view (24 C peak in view on G1's splat).
3. Stage 6: replace the stand-in in `stages/index.js` with the real stage; it must write
   `train.json` (and ideally `eval/`).
4. The win11 harness cannot download over CDP; downloads there are read from the link's blob, and
   the acceptance check selects the same bytes from OPFS (Playwright refuses files over 50 MB to
   a remote browser).
