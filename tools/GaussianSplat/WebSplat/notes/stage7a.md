# Stage 7a: export/viewer round trip

Tests: `test/stage7a/run_tests.sh` (outputs in `$WEBSPLAT_DATA/stage7a`; `FULL=0` skips the run13
check). All pass on the laptop (about 40 s including run13).

## What is implemented

`app/src/stages/export/` (no DOM; runs in a module worker and in Node):

- `ply.js`: the splat object (the WSSP `params.bin` layout of CONTRACTS.md 6-7), `readParams` /
  `writeParams`, `writePly` (exactly train.py `save_ply` / export_web.py `write_ply`: plyfile header,
  property order, normals 0, `f_rest` channel-major, quaternions normalised as numpy does in float32),
  `readPly` + `splatFromPly` (any binary little-endian 3DGS PLY, any property order: Brush exports
  too), `greyscale` (export_web.py's thermal channel mean, torch float32 order).
- `pyjson.js`: Python `json.dumps` text (float repr, `ensure_ascii`, `indent=1`). Native floats that
  happen to be integers (`k_c` 8.0, hot spot 10.0) are listed in `meta.js` `FLOAT_FIELDS`, so headers
  and meta.json are byte-identical with the native ones.
- `meta.js`: `apcg_splat` header info (`trainInfo` as train.py, `webInfo` as export_web.py) and
  `startCamera` (export_web.py `start_camera`, both branches).
- `spz.js`: SPZ via Spark's `transcodeSpz` (the encoder export_web.py runs through `ply2spz.mjs`),
  an SPZ v2/v3 decoder and `spzErrors` (per-field error against the source, in quantisation steps).
- `export.js`: `exportInputs(dataset.json, frames.json)`, `loadSplat` (params.bin or PLY),
  `exportSplat` → `splat.ply` (export_web.py `--ply` form: greyscale, header with temperature scale,
  start camera, display range; opens with temperatures in the viewer), `splat.spz`, `meta.json`
  (export_web.py site metadata with download sizes); `trainPly` for train.py's form.
- `export.worker.js`: `{type: 'export', bytes, opts}` → files (transferred). It imports Spark,
  resolved by an alias in `app/vite.config.js` to the splat viewer's pinned copy; the worker bundle
  is about 3 MB (Spark + three), loaded only for the export.
- `embed.js`: the result view. The viewer is a page-level WebGL app, so it is embedded as a
  same-origin iframe (`vendor/splat-viewer/index.html?embed=1`, copied from the viewer's `dist/` by
  `app/scripts/prepare-assets.mjs` when built) and opened by `postMessage` with the exported bytes
  (PLY: metadata from its header; SPZ: with meta.json). No WebGPU canvas is involved, so the laptop
  NVIDIA presentation bug does not matter.

## Conventions checked (tiny model: 10 Gaussians, SH 3, anisotropic, unnormalised quaternions)

| convention | how checked | result |
|---|---|---|
| property layout, f_rest channel-major | JS PLY bytes vs train.py `save_ply` and export_web.py `write_ply` | byte-identical (4300 / 4603 bytes) |
| quaternion w,x,y,z, normalised | bytes; plyfile read; viewer image vs gsplat, control read x,y,z,w | identical; control rejected (image error 10.7 vs 1.0 grey levels) |
| opacity logit | plyfile; viewer readout of an α 0.7 Gaussian; control without logit | identical; control rejected (14.3 C readout error) |
| log scales | plyfile; control with linear scales | identical; control rejected |
| SH order and direction (camera→Gaussian) | JS SH eval vs gsplat `spherical_harmonics` (8e-8); viewer readouts of SH 1 and SH 2-3 Gaussians from two opposite views; control with odd bands negated | match; control rejected (8.8 / 17.4 C) |
| axes / up | `up` [0,0,1] in the header; control with the viewer's no-header -Y up | control rejected |
| units | metres, ENU from the header origin; SPZ 12 fractional bits (0.24 mm) | positions exact in PLY |
| temperature metadata | header and meta.json `temperature_scale`, display range; viewer readouts decode to the gsplat temperatures | byte-identical; readouts within 0.27 C (scaled value 0.003) |
| start camera | JS (float64) vs export_web.py (float32 viewmats inverted by numpy); viewer opens there (image = gsplat at that camera) | 4.3e-6 m; with the native camera passed in, PLY and meta.json byte-identical |
| greyscale (thermal) | per-channel-different Gaussian, export_web.py's `sh.mean(2)` | byte-identical |

## Round trip numbers

- JS PLY → plyfile (and G1's `render_ply.load_ply`): every property float32-equal to the source
  (quaternions equal to numpy's normalisation); Brush property order + "Vertical axis: y" comment →
  canonical PLY byte-identical; renormalising an already normalised quaternion moves it ≤ 1 ulp.
- SPZ (v3; Spark's antialiased flag cleared for classic since round 1): byte-identical to export_web.py's for the same PLY. Decode
  error, tiny model: positions 0.36 steps, scales 0.43, DC 0.50, alpha 0.55, SH1 0.20, SH2-3 0.40,
  rotation 0.085°. run13: positions 0.50, scales 0.56, DC 0.52, alpha 0.56, SH1 0.56, SH2-3 0.53
  steps, rotation 0.246°; 60 scale and 21 DC values clamped by the format's range. Spark stages values
  before quantising (float16; SH at 8 bits), so errors reach about 0.56 steps; the bound in
  `spzErrors` is half a step plus that staging. In temperature: SPZ vs PLY at the tiny model's
  Gaussian centres max 4.97 C (the 96 C Gaussian), median 0.42 C.
- Worker export in Chrome = Node export, byte for byte (PLY, SPZ, meta.json).
- run13 (591k Gaussians) in Node: read 1.8 s, export with SPZ and SPZ check 4.9 s. Header
  byte-identical with export_web.py; positions, scales, opacities identical; SH differ by ≤ 1.4e-5
  (export_web.py re-bakes the lobe on the GPU, train.py's splat.ply has its own bake) and quaternions
  by ≤ 3 ulp (torch vs numpy normalisation); export_web.py's own PLY read and rewritten is
  byte-identical.

## Viewer check (laptop Chrome, WebGL viewer, built dist/ as deployed)

Routes: the open-a-file page with the JS PLY (as plot.ardupilot.org/splat/), `?url=splat.spz&meta=`,
an export_web.py site page holding the JS SPZ/meta.json, and the embedded viewer fed from the worker
export (start view, opposite view, SPZ). Screenshots (whitehot, fixed display range) vs gsplat at the
same camera: mean 0.6-1.0 grey levels over covered pixels, coverage IoU 0.92-0.95; the viewer's own
readouts at every Gaussian centre within 0.27 C (PLY) and 0.42 C (SPZ, vs the decoded SPZ); embedded
= open-a-file pixel for pixel, site page = `?url` page. Thermal controls shown, temperatures decoded
from the PLY header alone.

## Changes outside WebSplat/

`tools/GaussianSplat/viewer/src/viewer.js` (rebuild `dist/` to deploy), plus round 1: per-asset
render mode in viewer.js, the SPZ flag in `viewer/ply2spz.mjs`, `rasterize_mode` written by
train.py, train_rgb.py and export_web.py:
1. Embedding: a same-origin parent may post `{type: 'apcg-splat-open', bytes, name, meta}`; the
   viewer replies `apcg-splat-opened` (with `error` on failure) and announces `apcg-splat-ready`
   when framed; `?embed=1` suppresses the open-file panel. Nothing changes for existing pages.
2. Bug fix: the render loop could draw a thermal frame before the thermal set-up (which awaits the
   colormaps fetch) and threw `Cannot read properties of null (reading 'width')` in `autoRange`
   (seen on the deployed open-a-file and site pages). It now waits for the set-up.

## Round 1 review fixes (REVIEW-stage7a.md)

1. Render mode per asset. Every native trainer (train.py, train_rgb.py) uses gsplat's default
   `classic` rasterisation, and Brush's exported PLY renders as gsplat classic (G1 criterion 9:
   75 dB against Brush's own eval renders), so the exports record `rasterize_mode: "classic"`:
   train.py and train_rgb.py `save_ply` info, export_web.py info (PLY header and meta.json), and the
   JS `trainInfo` / `webInfo` (after `origin`, as export_web.py). The viewer renders an asset that
   says `classic` with `preBlurAmount 0.3, blurAmount 0` (gsplat classic) and anything else as
   before (Spark's antialiased default). The SPZ antialiased flag is cleared for classic exports:
   Spark always sets it, so the SPZ is gunzipped, flag bit 0 cleared and re-gzipped with a fixed
   10-byte gzip header around zlib raw deflate, in `spz.js` (CompressionStream) and in
   `viewer/ply2spz.mjs` (node:zlib) alike; Chrome's worker, Node and ply2spz give identical bytes.
   Tests now use gsplat classic as the reference (viewer readouts within 0.35 C; the thin disc from
   the back 49.4 C) and assert the mode both ways: the classic cases reject the antialiased render
   (6.96 C on the disc), and `embed_legacy_back` (meta.json without the field) matches antialiased
   and rejects classic (6.84 C).
   Already deployed splats: their PLY headers and meta.json have no `rasterize_mode`, so the
   rebuilt viewer shows them exactly as now. Re-exporting them with export_web.py (nothing
   retrained) adds the field and clears the SPZ flag; the viewer then shows them as they were
   trained: small and thin Gaussians brighter, hot peaks higher (run13 held-out view 00140 peak
   99.6 C instead of 76.5 C in gsplat, 00149 44.0 instead of 29.4 C; median pixel 0.1 C), and
   slightly more opaque overlap. Nothing was deployed.
2. `spzErrors` refuses an SPZ whose SH degree is not the source's (at most 3) or whose arrays have
   the wrong length, counts non-finite decoded values (`over.nonFinite`), and compares values
   outside the format's range with their saturated value (still counted in `clamped`). Controls in
   `run_node.mjs`: a real degree-0 SPZ (rejected), NaN in the decoded splat (rejected), a DC value
   2 steps off (rejected); out-of-range scale, DC and SH decode to the limits, and a wrongly decoded
   saturated value is rejected.
3. `run_tests.sh` builds the viewer from the current source into `$WEBSPLAT_DATA/stage7a/viewer-dist`
   every run and serves that build.
4. Streaming (deferred to stage 7, required for 1-2M Gaussians; a 2M synthetic export peaked at
   1.84 GiB RSS): the export worker owns an OPFS sink (`createSyncAccessHandle`) and writes the
   PLY header, then the body in chunks of about 64k Gaussians, each chunk built from the
   params.bin slices with greyscale and quaternion normalisation computed per chunk (no full
   greyscale copy, no whole-body buffer); the PLY is then closed and released, and SPZ encoding
   runs afterwards from the PLY file (Spark needs the whole input; its output is written to OPFS,
   and the SPZ check decodes and compares chunk by chunk against params.bin). The page receives
   file handles, not buffers, and gives the viewer a `File` from OPFS.
5. `compare_views.py` requires exactly the seven viewer cases and one readout at every Gaussian
   centre (dropping one readout per case now fails 7 checks).

## Round 2 review fix

The PLY header readers (viewer `plyMeta`, `viewer/ply2spz.mjs`, `ply.js` `readPly`) found the text
`end_header` anywhere, so a title containing it cut the header JSON short. They now match the
terminator line (`\nend_header\n`, or `\r\n`). Regression: the tiny model's title is
`tiny model end_header\nend_header` throughout (native export_web.py and ply2spz, JS export,
every viewer route checks the shown title), and a CRLF copy of the header reads the same.
`run_tests.sh` builds with `--configLoader native`, so Vite writes no bundled-config temp files
into the source tree.

## Open issues

1. Fixed in round 1 (above). Spark renders with mip-splatting antialiasing (`blurAmount` 0.3 with opacity compensation),
   while train.py trains with gsplat classic (0.3 px² added without compensation). The viewer
   matched gsplat `antialiased` (the round 0 reference); against classic it reads thin or small
   Gaussians low: tiny model disc 42.6 vs 49.4 C; run13 held-out view 00140 peak 76.5 vs 99.6 C,
   00149 29.4 vs 44.0 C (median pixel difference 0.1 C). At hot peaks this is a larger effect than
   SPZ quantisation.
2. A downloaded SPZ opened with the viewer's file picker has no temperature scale (SPZ has no
   metadata): it shows as a colour splat. The PLY carries everything; the SPZ needs its meta.json.
3. SPZ limits: positions ±2048 m at 12 fractional bits (fine for orbits; Spark offers more bits),
   scales and DC values outside the format's range are clamped (run13: 81 values).
4. Brush PLYs say "Vertical axis: y" although the data are ENU z-up; the viewer uses the
   `apcg_splat` `up`, other tools may rotate. `loadSplat` + `exportSplat` rewrite them canonically.
5. Stage 7 still has to wire `exportSplat` into a stage (streaming as in round 1 item 4), take the start camera from the dataset's
   views (Brush does not refine poses; train.py uses refined ones), and add the result view to the
   page.
