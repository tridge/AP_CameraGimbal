# WebSplat stage contracts

Version 1.11 (2026-11-11). Formats exchanged between stages through OPFS, and the job manifest
that ties them together. Each format is derived from what the native scripts
(`extract_frames.py`, `sfm.py`, `train.py`, `splat_common.py`) actually produce, so every browser
output can be compared with `$WEBSPLAT_DATA/ref`. A change to a format bumps its `version`; a
reader refuses versions it does not know (the job then re-runs the producing stage).

## Conventions (all formats)

- Binary data is **little-endian**, tightly packed, no padding, unless stated. Arrays are
  row-major. Images are stored top row first, `H x W`.
- Frame order: a stage's frame list is ordered by MKV packet index (`index`, ascending). Within a
  job, a frame is identified by `index` (MKV video packet number from 0, counting only non-empty
  packets, as `splat_common.read_mkv_metadata`) and named `'%05d.png' % index` as natively, even
  where no PNG exists. Image indices in feature, match, track and model files are positions in
  the stage 2 `frames` list (0..N-1), not MKV indices.
- Coordinates: local **ENU** metres. Origin = the target's latitude/longitude (after the hot spot
  re-centring of extract_frames.py), up = height above the **takeoff altitude** (`origin.alt_amsl`
  is the median of `alt_amsl_m - alt_relative_m` over all frames). Flat earth: `east =
  rad(lon - lon0) * 6378137 * cos(lat0)`, `north = rad(lat - lat0) * 6378137`
  (`splat_common.LocalFrame`).
- Camera axes: OpenCV (x right, y down, z forward). Two transform directions are used and always
  named:
  - `R_wc` / `world_from_cam`: columns are the camera axes in ENU; `x_world = R_wc x_cam + C`
    with camera centre `C`. Used by frames (`rotation`, `position`).
  - `cam_from_world` (COLMAP, gsplat `viewmat`): `x_cam = R_cw x_world + t`,
    `R_cw = R_wc^T`, `t = -R_cw C`. Quaternions are **w, x, y, z** (COLMAP order).
- Pixel coordinates: COLMAP convention, (0, 0) is the top-left corner of the top-left pixel, so
  pixel (u, v)'s centre is (u + 0.5, v + 0.5). Note: extract_frames.py and train.py's hot spot
  rays use the integer pixel index without +0.5 against `cx = W/2`; keep that when porting them
  (it is part of the reference behaviour).
- Temperatures: degrees C, `celsius = raw * temperature_scale_k + temperature_offset_k - 273.15`
  computed in float32 (`splat_common.raw_to_celsius`), per frame, defaults `1/64` and `0`.
- Hashes: `h = "sha256:" + hex` of the bytes for buffers up to 64 MiB; larger files use
  `"sha256c:" + hex(sha256(concat(sha256(chunk_k))))` over 4 MiB chunks (WebCrypto has no
  streaming digest). The source MKV always uses `sha256c`.
- Canonical JSON (for every hash of a JSON value: manifest checksum, stage keys, config hashes):
  UTF-8 of a serialisation with no whitespace, object keys sorted by UTF-16 code units (JS
  `Array.prototype.sort` order), keys whose value is `undefined` omitted, `undefined` array
  elements written as `null`, strings and numbers as `JSON.stringify` writes them
  (`stableStringify` in `app/src/storage/hash.js`). Hash = `"sha256:" + hex`.
- JSON metadata files carry `{"format": "websplat.<name>", "version": N, ...}`. Large arrays live
  in sibling binary files referenced by name with `dtype` (`u8`, `u16`, `u32`, `i32`, `f16`,
  `f32`, `f64`) and `shape`.

## Source identity (`websplat.source` v1)

```json
{"format":"websplat.source","version":1,"name":"orbit.mkv","size":31752639,
 "lastModified":1762578000000,"hash":"sha256c:…","copied":true}
```

`copied`: the MKV was copied into OPFS (`source/source.mkv`). Otherwise, on resume the user
re-selects the file and it must match `size` and `hash` before any stage re-reads it.

## 1. Ingest index (`websplat.ingest` v1) — stage 1

`ingest/index.json`: one record per non-empty FFV1 video packet, in packet order:

```json
{"format":"websplat.ingest","version":1,"source":{…source identity…},
 "track":{"codec":"ffv1","width":640,"height":512,"pixel_format":"gray16le","timescale_ns":1000000},
 "frames":[{"index":0,"pts":0,"keyframe":true,"offset":1234,"size":56789,"meta":{…}}],
 "warnings":[]}
```

- `meta` is the APCG per-frame JSON verbatim: Matroska BlockAdditional bytes 8.. (bytes 0-7 are
  `00 00 00 00 'APCG'`), schema `apcg.thermal.v1`, with `telemetry` (`apcg.telemetry.v1`:
  `utc_us`, `position{lat_e7, lon_e7, alt_amsl_m, alt_relative_m}`, `vehicle_attitude{roll_rad,
  pitch_rad, yaw_rad}`, `gimbal_attitude{roll_rad, pitch_rad, yaw_rad}`), `rotation_deg`,
  `hfov_deg`, `temperature_scale_k`, `temperature_offset_k`, `minimum_c`, `maximum_c`, … Unknown
  keys are kept.
- `pts` in track timescale units (the canonical MKV: ms). Native `pts` from PyAV must match.
- Raw frames (decoded on demand or cached): `u16` `[H, W]` gray16le **as stored in the MKV**
  (before any rotation; `rotation_deg` in `meta` is the display rotation and is applied by
  stage 2). Optional cache `ingest/raw.u16`, shape `[N, H, W]`, frame order.
- `track.codecPrivate` (optional, base64 of the Matroska CodecPrivate, i.e. the FFV1 configuration
  record): lets later stages build decoder input from the index without re-indexing the MKV.
  Readers that find it absent re-index (60-270 ms for the canonical MKV).
- `ingest/proposal.json` (`websplat.proposal` v1): ingest's automatic proposal for the selection
  options, shown to the user for confirmation:

```json
{"format":"websplat.proposal","version":1,
 "proposal":{"rotation":0,"hfov":24.2,"target_alt":null,"lat":-35.5799111,"lon":148.9066524},
 "hotspot":{"lat":…,"lon":…,"up":45.4,"frames":15,"errorPx":10.0},
 "lookAt":{"enu":[E,N,U],"errorPx":…},
 "rotations":[{"rotation":0,"frames":15,"errorPx":10.0},{"rotation":180,"frames":15,"errorPx":29.0}],
 "homeAltitude":978.445,"notes":["…"]}
```

  `proposal` holds selection option values (names as `selection.options` below); `rotation` is
  the one of 0/90/180/270 whose hot-pixel rays triangulate most consistently (the recorded one
  unless another is better by 1.5x); `lat`/`lon` the hot spot triangulated in a provisional frame
  (null without hot frames; then `lookAt` is where the optical axes converge); `target_alt` null =
  use the hot spot. On failure `{"format","version","error":"…"}`. `notes` explain choices and
  telemetry gaps. The confirmed values become the job's `config.select`.
- The stage's manifest `result` also holds `range` (`{start, end, frames}`, a request's range
  resolved to packet-order indices; null without a request) and `homeAltitude` (the
  whole-recording takeoff altitude, recorded even when the proposal fails; null if no frame has
  a relative altitude). With `config.proposal.range` the proposal covers only those frames.
- Accept: raw pixel equality (sha256 of each frame's `u16` bytes) with `splat_common.decode_frames`
  (`frame.to_ndarray(format='gray16le')`), identical `meta` and `pts`.

## 2. Frames and pose priors (`websplat.frames` v1) — stage 2

`frames/frames.json` is **the native frames.json schema** (so native tools can read it) plus a
`format`/`version` and a `selection` record:

```json
{"format":"websplat.frames","version":1,
 "source":"orbit.mkv","width":640,"height":512,"hfov_deg":24.2,
 "K":[[f,0,320],[0,f,256],[0,0,1]],"rotation_deg":0,
 "origin":{"lat":-35.57991111814981,"lon":148.9066523807906,"alt_amsl":978.445,
           "frame":"ENU, up from takeoff altitude"},
 "target_enu":[0,0,45.423214771010485],
 "temperature_c":{"p1":13.6,"max":127.7},
 "frames":[{"index":0,"name":"00000.png","utc_us":1766059345029000,
            "position":[E,N,U],"rotation":[[…],[…],[…]],
            "minimum_c":13.79,"maximum_c":21.35,
            "temperature_scale_k":0.015625,"temperature_offset_k":0}],
 "selection":{"options":{"lat":-35.5802022,"lon":148.9065438,"target_alt":null,
              "locate_hotspot":true,"hotspot_min_c":60,"hotspot_max_error":60,"rotation":0,
              "hfov":null,"margin":0.1,"radius":null,"start":0,"end":null,"step":1,
              "max_range":2000,"max_median_dev":15},
              "hotspot":{"enu":[9.8,32.4,45.4],"frames":15,"median_ray_error_px":10.0,"used":true},
              "mode":"single","considered":201,"selected":201,"blank":[],"drifted":[],
              "typical_median_c":0}}
```

- `width`, `height` are **after** rotation; `K = [[f,0,W/2],[0,f,H/2],[0,0,1]]`,
  `f = (W/2)/tan(hfov/2)`, `hfov` = option, else first frame `hfov_deg`, else 24.2.
- `rotation_deg`: rotation applied to raw frames, `np.rot90(raw, k)` with `k = (deg // 90) % 4`
  (counter-clockwise). For the canonical MKV the stored value is 180 and the reference uses 0.
- `position` = telemetry camera centre `C` (ENU); `rotation` = `R_wc` (world_from_cam) from
  `splat_common.camera_rotation`: `R_ENU_NED · Rz(vehicle yaw + gimbal yaw) · Ry(gimbal pitch) ·
  Rx(gimbal roll) · R_BODY_CAM` (gimbal roll/pitch earth-level, yaw vehicle-relative).
- `source` is a name only (native writes an absolute path; comparisons ignore it).
- `selection.mode`: how pixels were produced (`single`: one decode pass, drifted frames compacted
  away afterwards; `redecode`: native's two passes; `cache`: raw frames cached in OPFS). All give
  identical outputs. `selection.considered` = `end - start + 1` (frames in the index range, with
  or without a pose); `selected` = frames passing the view test (after `step`).
- Pixel data, same order as `frames`:
  - `frames/features.u8` `[N, H, W]`: CLAHE 8-bit images = native `images/NNNNN.png`
    (`p1, p99` percentiles of celsius, `v = clip((c - p1)/max(p99 - p1, 0.5), 0, 1)`,
    `uint8(v*255)` truncation, OpenCV CLAHE clip 3.0, 8x8 tiles).
  - `frames/thermal.u16` `[N, H, W]`: rotated raw radiometric values = native `thermal/NNNNN.png`
    (16-bit). Not temperatures; convert with the per-frame scale/offset.
- Accept: identical frame list; `position`/`rotation` within 1e-6; images identical (see stage 2
  plan for any documented tolerance). Reference: `$WEBSPLAT_DATA/ref/work` (reproduced bit-exact,
  notes/stage0.md).

## 3. Features (`websplat.features` v1) — stage 3

`features/features.bin`, little-endian:

```
header   u32[4]  MAGIC 'WSFT' (0x54465357), VERSION 1, numImages, descriptorDim (128)
index    numImages x u32[2]  [count, offset]   offset in keypoints (and descriptors) rows
keypts   total x f32[6]   x, y, a11, a12, a21, a22   (COLMAP affine keypoint, pixel coords above)
descs    total x u8[128]  COLMAP SIFT descriptor bytes as stored in database.db
```

- Image k = frames[k], computed on `features.u8` (the CLAHE images). The descriptor block is one
  contiguous `u8 [total, 128]` array in frame order, so a worker can take frame k's
  descriptors as a subarray (`offset*128 .. (offset+count)*128`) without copying.
- Keypoint order is the extractor's order; match indices refer to it.
- Descriptors as COLMAP stores them (`FeatureDescriptorsToUnsignedByte`, L1-root normalised
  then `min(255, round(512 * v))`); the matcher L2-normalises them as float (sfm.py).
- `features/features.json` records `{extractor, max_num_features: 8192, options…, per-image
  counts}`. Native reference: `database.db` tables `keypoints` (rows x 6 f32) and `descriptors`
  (rows x 128 u8). Note: native GPU SIFT is not deterministic (notes/stage0.md: 50 of 201 images
  differ between two runs) and returns up to 10099 keypoints per image despite 8192 max.

## 4. Pairs, matches, verified geometry — stage 4

- `match/pairs.json`: `{"format":"websplat.pairs","version":1,"options":{max_angle:25,
  max_scale:2.2, neighbours:30, sequential:5}, "pairs":[[i,j],…]}`, `i < j`, sorted, = native
  `pairs.txt` (3175 pairs for the reference) by name.
- `match/matches.bin` (`app/src/stages/match/format.js`, `WSMT` v1), u32:
  header `[0x544d5357, 1, numPairs, numMatches]`; pairs `numPairs x [i, j, status, count,
  offset]`; matches `numMatches x [featureIndexInI, featureIndexInJ]`. status 0 matched (count
  may be 0), 1 skipped (an image has < 2 descriptors; native writes no row). Options in
  `match/matches.json` (`ratio: 0.8`, rule: L2-normalised, forward ratio on L2 distance
  `sqrt(2 - 2 sim)`, reverse nearest-neighbour identity).
- `match/geometry.bin` (`WSTV` v1, two-view verification output; `encodeGeometry` /
  `decodeGeometry` in `app/src/stages/match/format.js`; `test/stage4/export_native.py` writes the
  native database in this form). One container, pairs in `pairs` order, little-endian, no padding:

```
header  u32[4]                  MAGIC 'WSTV' (0x56545357), VERSION 1, numPairs, numInliers
pairs   numPairs x u32[5]       [i, j, config, count, offset]   offset in inliers, contiguous
inliers numInliers x u32[2]     [featureIndexInI, featureIndexInJ]  (indices as in matches)
models  numPairs x f64[34]      F 9, E 9, H 9 (row-major 3x3), qvec 4 (w,x,y,z), tvec 3
```

  `models` starts at byte `4 * (4 + 5 numPairs + 2 numInliers)` (4-byte, not necessarily 8-byte
  aligned: copy before viewing as f64). The relative pose is `cam_j_from_cam_i`; models COLMAP
  does not provide are zero. `config` = COLMAP `TwoViewGeometry::ConfigurationType` (0
  UNDEFINED, 1 DEGENERATE, 2 CALIBRATED, 3 UNCALIBRATED, 4 PLANAR, 5 PANORAMIC,
  6 PLANAR_OR_PANORAMIC, 7 WATERMARK, 8 MULTIPLE); pairs COLMAP cleared (below
  `min_num_inliers`) have count 0. Options: COLMAP `TwoViewGeometryOptions()` defaults as
  `pycolmap.verify_matches`, recorded in `match/geometry.json`. Reference: 2702 pairs with
  inliers, 388488 inliers (config 0: 473 pairs, 2: 1101 / 120637 inliers, 3: 1016 / 19927,
  6: 585 / 247924).
- Matching and verification parameters (`config.match`: ratio 0.8, the pair options, the fp16
  emulation flag) are stage config keys, so changing them invalidates the stage.

## 5. Reconstruction and tracks (`websplat.model` v1) — stage 5

A **COLMAP binary sparse model** (`cameras.bin`, `images.bin`, `points3D.bin`, as written by
COLMAP; rigs/frames files optional) under `model/<name>/`, plus `model/model.json`:

```json
{"format":"websplat.model","version":1,"camera_model":"SIMPLE_RADIAL",
 "rounds":[{"threshold_px":40,"triangulated":45012,"filtered":1710,"mean_reproj_px":0.84}],
 "prior_sigma_m":5.0,"image_order":"frames"}
```

- One shared camera, `SIMPLE_RADIAL [f, cx, cy, k1]`, initial `[K00, W/2, H/2, 0]`, prior focal
  set. Images: name = frame name, pose `cam_from_world` (qvec w,x,y,z; tvec), image ids 1..N in
  frames order. Points3D tracks are `(image_id, point2D_idx)` with `point2D_idx` the stage 3
  keypoint index; the track graph before BA uses the same structure.
- Memory: the mapper runs in a 4 GB WebAssembly heap. Above 6M verified matches, each image keeps
  the same deterministic subset of its keypoints in every pair (fraction sqrt(6M / matches)) and
  only matches between kept keypoints are mapped; pairs left with fewer than 15 are dropped. The
  stage result records `matches` and `keptKeypoints` (1 when nothing was dropped); thermal jobs
  stay far below the limit. Running out of memory anyway fails the stage with a message naming
  the images and matches.
- Names: `model/telemetry/` (initial, no points), `model/final/`. Reference:
  `$WEBSPLAT_DATA/ref/work/colmap/sparse/final` (37959 points, 185255 observations, f
  1503.948, k1 -0.0972).

## 5b. Training dataset (`websplat.dataset` v1)

`dataset/dataset.json` (what train.py's `load_scene` computes):

```json
{"format":"websplat.dataset","version":1,
 "width":640,"height":512,"K":[[f,0,cx],[0,f,cy],[0,0,1]],
 "undistortion":{"from":{"model":"SIMPLE_RADIAL","params":[f,cx,cy,k1]},
                 "method":"cv2.initUndistortRectifyMap(K, [k1,0,0,0], newK=K) + remap INTER_LINEAR, BORDER_REFLECT, on celsius float32"},
 "scale":{"kind":"log1p","t0_c":10.9068,"t1_c":126.4231,"k_c":8.0},
 "hot_c":32.6,"v_hot":0.0,
 "images":[{"name":"00000.png","frame":0,"split":"train","hot":false,
            "viewmat":[16 numbers, cam_from_world 4x4 row-major]}],
 "skipped":{"elevation":[],"min_obs":[],"pose_offset":[]},
 "bounds":{"centre":[0,0],"radius":165,"z_min":7.49,"z_max":66.22,
           "ceiling":{"origin":[x0,y0],"cell":5,"n":72,"file":"ceiling.f32"}},
 "hotspot":{"enu":[-0.94,0.21,31.57],"frames":41,"median_ray_error_px":0.79},
 "extent_m":0.0,"lr":{"means":0.0,"scales":5e-3,"quats":1e-3,"opacities":5e-2,"sh0":2.5e-3,"shN":1.25e-4},
 "points":{"count":37781,"file":"points.f32"},
 "options":{…train.py scene options…}}
```

Written by the `prepare` stage as `prepare/gen-<g>/{dataset.json, targets.f16, points.f32,
ceiling.f32}` (`app/src/stages/prepare/`; clarifications, v1.3.1):
- `images[]` also carry `image_id` (model image id); `frame` is the position in the stage 2
  `frames` list (the row of `thermal.u16`). `viewmat` values are float32 (train.py's tensor).
- `skipped` also has `budget` (cold views dropped by `max_images`) and `pose_offset_limit_m`.
- `targets`: `{"file":"targets.f16","dtype":"f16","shape":[n,H,W]}`; `points`: also `dtype`, `shape`.
- `training`: train.py's settings for the configured options: `options` (train.py training option
  values), `v_hot`, `lr` (as above), `adam_eps`, `means_lr_gamma` (`0.01 ** (1/iterations)` per
  step), `strategy` (gsplat 1.5.3 `DefaultStrategy` fields with train.py's overrides:
  `refine_stop_iter = iterations // 2`, `reset_every` 3000, `absgrad`, `grow_grad2d`,
  `refine_every`), `scene_scale` (`10 * max_scale`), and fixed initialisation constants.
- `stats`: registered, hot, test, points in bounds, points valued by projection.
- Point order is `points3D.bin` file order (train.py's is pycolmap's hash-map order); a point's
  value does not depend on the order. The model has points at identical positions.
- `hotspot` is computed in float64 from the float32 viewmats (train.py inverts them in float32):
  within 1e-5 m of train.py's.

Defaults (train.py): `max_images` 500, `hot_delta` 10 C, `min_elevation` 20 deg, `min_obs` 30,
`max_pose_offset` 20 m, `holdout` 20, `hot_holdout` 8, `k` 8 C, `t0`/`t1` automatic, `radius` 150 m,
`height_margin` 15 m, `ceiling_cell` 5 m, `ceiling_margin` 5 m. Steps, in this order:

1. Images: the model's images sorted by name. Keep elevation `atan2(dz, horizontal distance)` of
   the camera centre above `target_enu` >= `min_elevation`; then `num_points3D >= min_obs`; then
   camera-centre-to-telemetry distance <= `max(max_pose_offset, 4 * median distance)` (median
   over the images left after the previous filter). Rejected names go in `skipped`.
2. `hot_c = median(maximum_c of every frames.json frame) + hot_delta`; hot = kept images with
   `maximum_c > hot_c`, cold = the others. Cold budget `n_cold = max(0, max_images - len(hot))`
   (all cold if `max_images` is 0); if `n_cold < len(cold)`, keep `cold[int(x)]` for `x` in
   `linspace(0, len(cold) - 1, n_cold)`. Chosen = hot + cold, by name.
3. Test = `cold[holdout // 2 :: holdout]` ∪ `hot[hot_holdout // 2 :: hot_holdout]` (from the lists
   of step 2); the others train. Frozen reference holdouts (13): cold `00015 00035 00066 00086
   00115 00135 00167 00187`, hot `00009 00050 00096 00140 00149` (notes/stage0.md).
4. Celsius of each chosen image (float32), undistorted: `cv2.initUndistortRectifyMap(K,
   [k1,0,0,0], None, K, (W, H), CV_32FC1)` and `remap` `INTER_LINEAR`, `BORDER_REFLECT`
   (`K = [[f,0,cx],[0,f,cy],[0,0,1]]` of the refined camera; the dataset `K` is the same).
5. Scale `log1p`: `t0 = percentile(concat(c[::8, ::8] of every chosen image), 0.5) - 2`, `t1 =
   max over chosen images`, `k = 8`; `v = clip(log1p(max(c - t0, 0)/k) / log1p((t1 - t0)/k), 0, 1)`,
   inverse `t0 + k * expm1(v * log1p((t1 - t0)/k))`. `v_hot = v(hot_c)`. `dataset/targets.f16`
   `[n, H, W]` = `float16(v)` (round to nearest even) of the chosen images in `images` order.
   Float, never 8-bit packed or gamma-encoded.
6. Bounds: `near` = model points with horizontal distance to `target_enu` <= `1.2 radius`;
   `z_min = p1(near z) - height_margin`, `z_max = p99(near z) + height_margin`, bounds radius
   `1.1 radius`, centre = target xy. Ceiling over `near`: `n = ceil(2.4 radius / cell)`, origin =
   target xy - `1.2 radius`, cell `(i, j) = clip(int(((x, y) - origin) / cell), 0, n - 1)`; a cell
   with at least 10 points gets the 90th percentile of their z, then a 3x3 maximum filter
   (empty cells -inf), then `+ ceiling_margin`, empty cells `z_max`, all capped at `z_max`.
   `dataset/ceiling.f32` `[n, n]` indexed `[i, j]` (i east, j north).
7. Initial points: model points with horizontal distance <= `radius` and `z_min <= z <=
   ceiling(x, y)`; each valued by the mean of `targets` at its first 4 track observations in chosen
   images (test images included), keypoints undistorted with `cv2.undistortPoints(xy, K, dist,
   P=K)`, pixel `(round(x), round(y))`, inside the image. Fallback (native `train.py`, kept):
   a point with no track observation in any chosen image is projected into the chosen images in
   `images` order with the undistorted `K` and `viewmat` (`u = f x/z + cx`, `v = f y/z + cy`, no
   +0.5), and each view where `z > 1` and `0 <= u < W`, `0 <= v < H` supplies a sample, at most 4
   per point; these samples are averaged like observations. Points with no valid sample are
   dropped (fewer than 100 points before valuing is an error). For a reconstruction made from
   these frames every point has observations, but filtering or subsampling images can leave some
   without, so the fallback matters. `dataset/points.f32` `[count, 4]` x, y, z, v.
8. `extent_m` = max distance of the chosen images' camera centres from their mean. Learning
   rates `lr`: means `1.6e-4 * extent_m` (decaying to 1% over the iterations), scales 5e-3, quats
   1e-3, opacities 5e-2, sh0 2.5e-3, shN 2.5e-3 / 20.
9. `hotspot`: rays through each hot image's `argmax` pixel of `targets` (integer pixel, no +0.5),
   `triangulate_rays` with focal `f` (at least 5 hot images, else null).

## 6-7. Splat output (`websplat.splat` v1)

Stage 6 output (`train/gen-<g>/`), what the export stage consumes (until stage 6 is written, the
'train' stand-in, `app/src/stages/export/standin.js`, loads a trained splat into this layout):

```json
{"format":"websplat.train","version":1,"splat":"splat.ply","gaussians":594940,"sh_degree":3,
 "rasterize_mode":"classic","iterations":30000,"eval":{"dir":"eval","views":["00009.png",…]},
 "standin":false}
```

- The stand-in's stage version is `"standin-1"` and its `train.json` says `"standin": true`; no
  real stage uses such a version, so when stage 6 replaces it, stand-in outputs (and the exports
  made from them) fail validation and are redone. The export's result records `trainStandin`.
- `train.json` as above; `splat` names the splat file: `splat.ply` (a Brush export as G1's worker
  writes it: binary little-endian 3DGS PLY, any property order, normals optional, `comment
  apcg_splat` optional and ignored) or `params.bin` (the WSSP layout below). `rasterize_mode`:
  how the trainer rasterised (Brush fork and train.py: `classic`; written to the exports).
  `iterations` and `eval` are optional (`eval/<name>.f32`: held-out renders, float32 `[H, W]`
  scaled values, as G1's browser runs; the stage 7 tests score them). Stage 6 also writes
  `trainer` (fork and patch series), `brush_config` (the Brush options used) and `resumed`.
- The export also reads 5b's `dataset.json` (scale, hot spot, views for the start camera) and
  stage 2's `frames.json` (origin, `target_enu`).
- Stage 7 output (`export/gen-<g>/`): `splat.ply`, `splat.spz`, `meta.json` as below (the
  downloads), written in chunks; its manifest result has the counts, temperature range, sizes,
  timings, the SPZ decode errors (`spzErrors`, merged over chunks; any value beyond its bound or
  not finite fails the stage; format clamping is counted in `clamped`) and `spz.temperature`
  (the largest drop/rise of a Gaussian's emitted temperature over 32 directions, opacity >= 0.5).
  Its start camera is the hot view with the highest frame `maximum_c` (export_web.py's geometry
  from that view); export.js (7a, native-identical) keeps export_web.py's middle hot view. The
  result view also offers map's final COLMAP model as a tar (`sparse/0/*.bin`) built from the
  OPFS files.

- Checkpoint `train/ckpt-<g>/params.bin` (job layout below; the final output is `train/gen-<g>/`): header `u32[4]` `'WSSP' (0x50535357), 1, count,
  sh_degree`, then f32 arrays in this order, each `[count, …]`: means 3, scales 3 (log), quats 4
  (w,x,y,z, unnormalised), opacities 1 (logit), sh0 3, shN `((d+1)^2 - 1) x 3` (coefficient-major,
  channel last, as gsplat/train.py); the trainer's raw parameters (quaternions not normalised,
  3D-filter floor not baked), taken between two training batches so they are exactly those of
  `iteration`. `train/ckpt-<g>/state.json`: `iteration`, `gaussians`, `resumed` (earlier resume
  points). v1 resumes from params only: optimiser moments, refine statistics and data order
  restart; the step count (LR and SH schedules, refine window) continues from `iteration`; the
  3D-filter floor is recomputed from the restored positions.
- **Stop and accept** (`controller.acceptEarly`): training ends early and the stage completes as
  usual (eval renders, `splat.ply`) with the iterations done. `train.json` then has `iterations` =
  the iterations run, `iterations_planned` and `stopped_early: true`; the stage result always has
  `iterations`, `iterations_planned` and `stopped_early`; the manifest record's `progress.done` is
  the count reached. The acceptance is recorded in the train record as `accept: {iteration, at}`
  (written before the stage completes; a running worker is told first and refuses before training
  has started). A later attempt of the stage with `accept` set (a reload before it completed, or
  an accept while stopped) ends right after its checkpoint (it asks Brush for one step from the
  restored parameters; Brush reports steps in small groups, 5 measured, so `iterations` is a few
  more than `checkpoint.done`), or without a checkpoint at `accept.iteration`. A stopped
  stage can only be accepted with a checkpoint. A done stage under its key is not run again; a
  config or input change resets the record (and with it `accept`) as for any stage.
- **Preview**: the running train worker answers the command `preview` (host `{type:'command'}` /
  worker `{type:'reply'}`, `workers/host.js`) with the current splat (Brush `exportPly` at a batch
  boundary) in the export's PLY form (`stages/train/preview.js`: colours or temperature scale,
  start camera from `config.export`); a stopped job previews its checkpoint's `params.bin` the
  same way (3D-filter floor not baked). Nothing is written to OPFS.
- Exported PLY: exactly train.py `save_ply`: binary little-endian, one `vertex` element of f32
  properties `x y z nx ny nz f_dc_0..2 f_rest_0..(3K-4) opacity scale_0..2 rot_0..3`, normals 0,
  `f_rest` channel-major (`shN.transpose(0,2,1)`), quaternion normalised w,x,y,z, header
  `comment apcg_splat {"kind":"thermal","up":[0,0,1],"target":[E,N,U],
  "temperature_scale":{…},"origin":{…},"rasterize_mode":"classic","trim_centre":[E,N],
  "trim_radius":R}`. Greyscale: value = `SH_C0 * f_dc + 0.5` (all three
  channels equal), decoded with the scale's inverse `t0 + k * expm1(v * log1p((t1-t0)/k))`.
- Downloads as `export_web.py` (stage 7a, `app/src/stages/export/`): `splat.ply` in its `--ply`
  form (SH channels replaced by their mean; header info `kind, gaussians, sh_degree, title,
  description, camera{position,target,fov_deg}, up, target, origin, rasterize_mode, trim_centre,
  trim_radius, temperature_scale, hotspot, colormap, display_range_c`, Python `json.dumps` text; `rasterize_mode` "classic": gsplat
  classic, as trained), `splat.spz` = Spark `transcodeSpz` of that PLY (`maxSh` 3, SPZ v3, 12
  fractional bits, antialiased flag cleared for classic, gzip with a fixed header holding the PLY's
  apcg_splat JSON in an `AP` FEXTRA subfield, `viewer/spz-meta.mjs`; the stage
  encodes it in chunks of 131072 Gaussians, which gives Spark's whole-file bytes), `meta.json` = the same info plus
  `downloads{ply,spz}` (bytes), `json.dumps(indent=1)`. With `config.export.view` (a request's
  captured view) the start camera is `position` = the view camera in `frames.origin` ENU, `target`
  = the export target, `fov_deg` = `view.fov_deg`, the distance from the target clamped to
  [1, 8] x `dataset.bounds.radius` along the same direction (unclamped, with a warning, if the
  radius is not positive). With `config.export.target` `"picked"` the export target is
  `frames.target_enu`, also when prepare found a hot spot. Otherwise: start camera `export_web.start_camera` from
  the dataset views (middle hot view). Target: `dataset.hotspot.enu`, else `frames.target_enu`.
- Trim circle (`splat_common.trim_info`, `export/meta.js` `trimInfo`): `trim_centre` =
  `dataset.bounds.centre` (E, N; thermal: the target's, not the hot spot's), `trim_radius` =
  0.85 x `dataset.bounds.radius` (metres, horizontal; the radius beyond which training prunes
  Gaussians: thermal 1.1 x the prepare radius, RGB the radius), Python floats, right after
  `rasterize_mode` in train.py's header, export_web.py's info and the RGB forms. The viewer hides
  Gaussians outside the circle (switchable, with a radius slider). Absent without bounds (older
  datasets and splats), in which case the viewer estimates a circle from the splat. export_web.py
  reads the bounds from `scale.json` (`train.py` always wrote them; `train_rgb.py` now writes
  `bounds {centre, radius}`).

- Training stopped early: the PLY header info and `meta.json` add `stopped_early: {iterations,
  iterations_planned}` (absent otherwise, so full runs keep export_web.py's bytes); the export
  result has `stoppedEarly` (the same, or null).

## RGB sources (stage set `rgb`)

RGB recordings (H.264/HEVC in MPEG-TS or MP4, pose telemetry from MISB ST 0601 KLV or APCG SEI)
run the `rgb` stage set: its own ingest, select and prepare stages (stage versions `rgb-1`;
prepare `rgb-2`),
then the thermal features, match, map, train and export stages, which read the formats below.
Native references: `extract_ts_frames.py`, `sfm.py`, `train_rgb.py`, `export_web.py`. Job ids
start `rgb-`. Coordinates and frame naming follow the conventions above (`'%05d.png' % index`,
ENU about the target, up from the takeoff altitude).

- **1 (`websplat.ingest-rgb` v1, `ingest/index.json`)**: `{format, version, source, container
  ("mpegts" | "mp4"), track {codec, width, height, timescale}, frames [{index, pts, keyframe,
  offset, size, telemetry}], warnings}`. Frames are the video track's packets in file order from
  the viewer's demuxer (`containers.js`): MPEG-TS `offset` = the first TS packet of the frame's
  PES, `size` = its elementary-stream bytes, `pts` the demuxer's `timestampTicks` (90 kHz PES
  PTS, unwrapped across the 33-bit wrap); MP4 the sample's offset, size and track timescale.
  `telemetry` = the viewer's normalised record (`normalizeTelemetry`) reduced to `{utc_us, position {lat_e7, lon_e7, alt_amsl_m, alt_relative_m}, vehicle_attitude,
  gimbal_attitude, hfov_deg, offset_ms, source}` (`offset_ms`: distance of a KLV sample matched
  by nearest timestamp). Its `gimbal_attitude` (earth-frame roll/pitch, yaw relative to the
  vehicle) gives `extract_ts_frames.py`'s `camera_rotation` through the thermal `cameraRotation`.
  A frame has a pose when its position, platform attitude and sensor roll/pitch are finite
  (sensor yaw may be absent); others are skipped. Longitudes are taken within 180 degrees of the
  local origin's, so a flight across the antimeridian stays in one frame.
  Stage result: `kind: "rgb"`, `range` (as thermal), `homeAltitude` and `homeAltitudeRule`: with
  relative altitude (APCG SEI) the thermal rule; without (KLV) the first record's GPS altitude,
  as `extract_ts_frames.py`. `ingest/proposal.json` (`kind: "rgb"`): `proposal {lat, lon,
  target_alt: 0, hfov}` where the optical axes meet the takeoff-height plane (the standalone
  form only).
- **2 (`websplat.frames` v1 with `kind: "rgb"`)**: `frames.json` with `width`/`height` the
  processing size (`select.width`, height scaled with the aspect kept), `K` from the HFOV
  (request `hfov`, else the recording's, else 70), `rotation_deg` 0, `origin`, `target_enu`,
  `frames [{index, name, utc_us, media_s, position, rotation}]` and `selection {version, options,
  source_size, counts, step, selected, decoder}`; no temperatures. `features.u8` `[N, H, W]` grey
  (cv2 BGR2GRAY weights) for SIFT; `rgb.u8` `[N, H, W, 3]` RGB8. Options (`config.select`):
  `lat, lon, target_alt` (above takeoff), `hfov`, `width`, `start, end`, `fps` (3), `min_height`
  (5 m, above the target: KLV recordings may start airborne, so takeoff height cannot be relied
  on), `max_offset_ms` (200), `margin` (0.1), `max_range` (2000), `budget` (500 for requests and
  the standalone form: every ceil(n / budget)-th of the frames that see the target; RGB frames
  bring far more matches than thermal ones and mapping runs in a 4 GB WebAssembly heap: 168
  frames at 8192 features took 1.6 GB, 361 frames ran out). Selection: in packet order, a
  frame at least 1/fps after the last kept one, with a pose, telemetry within `max_offset_ms` and
  the camera `min_height` above the target, then the target projected inside the margin and
  nearer than `max_range`. Decoding: WebCodecs where the browser has the codec, else the viewer's
  FFmpeg core on one keyframe group per batch (each group decoded once), scaled with area
  averaging.
- **3-5**: unchanged formats on the grey images; one SIMPLE_RADIAL camera seeded from `K` at the
  processing width (`config.map.camera` `"RADIAL"`: `[f, cx, cy, k1, k2]`, as `sfm.py
  --camera-model RADIAL`; a legacy `cameraModel` key is ignored, as it always was, so stage keys
  stay truthful; map checkpoints record `camera` and are not resumed under another model). RGB jobs set `config.features = {maxNumFeatures: 8192}` and `config.map.camera = "RADIAL"`
  (the native RGB recipe) when first opened; jobs made earlier keep 4096 and SIMPLE_RADIAL.
  Mapping thins matches to stay within its heap. Mapping that runs out of memory fails with a message naming
  the images and matches.
- **5b (`websplat.dataset` v1 with `kind: "rgb"`)**: `train_rgb.py`'s setup: images undistorted to
  the pinhole `K` (initUndistortRectifyMap with `[k1, k2, 0, 0]` + remap INTER_LINEAR, constant
  border, on RGB8; SIMPLE_RADIAL, RADIAL or PINHOLE models),
  `targets.u8` `[n, H, W, 3]`; images: every registered one, evenly thinned to at most
  `max_pixels / (W H)` (500 x 1280 x 720 pixels: Brush keeps them in memory), every 25th held out
  (`holdout`); points `points.f32` `[count, 6]` (x, y, z, r, g, b in 0..1): model points within
  `radius` (200 m, or `config.prepare.radius`) of the model's median point (request jobs:
  `config.prepare.centre` `"target"`, about `frames.target_enu`), coloured as the mean
  of up to three of their observations; bounds `{centre, radius, z_min (lowest point - radius: no
  floor), z_max, ceiling, max_vertical, vertical_prune_from}` with `train_rgb.py`'s height
  ceiling; `training` as `train.py`'s structure with `train_rgb.py`'s values (SH degree 1, SSIM
  0.2, cap 3M, grow_grad2d 0.0008, `max_scale` radius / 100 or 16 ground pixels); no `scale`,
  `hotspot` null. `max_vertical` (`train_rgb.py`'s `--max-vertical`: max(1, max_scale / 2) m;
  null with option 0) and `vertical_prune_from` (iterations / 2): Gaussians whose vertical 1-sigma
  extent `|row 2 of R(q) * scales|` exceeds it are pruned with the bounds every refine period
  from that step to the end, and once more after the last step (`too_tall`; for a run accepted
  before its last step the train worker applies that pass to the PLY). The too-big rule
  (`max_scale`) keeps gsplat's window, `reset_every < step < refine_stop_iter`, as natively.
  `config.prepare.max_height` (thermal and RGB): initial points (RGB; thermal through the
  ceiling) and every ceiling cell and `z_max` no higher than `frames.target_enu` z + max_height.
- **6**: `bounds.json` adds `max_vertical` and `vertical_prune_from` when the dataset has a
  `max_vertical` (fork patch 0017; thermal `bounds.json` is unchanged). The Brush input images are
  8-bit RGB PNGs (Brush's stock colour loss; the fork's numeric
  targets are off), `init.ply` coloured from the points; Brush's `max-splats` is the dataset cap
  limited per GPU class (desktop 3M, laptop 2M, integrated 1M, unknown 1.5M). Eval renders and the
  preview hold channel 0 (red) only.
- **7**: the PLY keeps the trained colours; header and `meta.json` info is `export_web.py`'s rgb
  form `{kind: "rgb", gaussians, sh_degree, title, description, camera, up, target, origin,
  rasterize_mode, trim_centre, trim_radius}`; target `frames.target_enu`; start camera from the captured view, else the
  middle view (`export_web.start_camera`). The stage result has `kind`.
- **Request**: `kind: "rgb"` with `rgb: {width}` selects this set (see below).

## Job request (`websplat.request` v1)

What the video viewer's Create splat hands to WebSplat (PLAN-v2.md). Heights are metres AMSL.

```json
{"format":"websplat.request","version":1,
 "source":{"name":"flight2_thermal.mkv","size":123456789,"lastModified":1762600000000},
 "range":{"timescale":1000,
          "start":{"pts":1234,"offset":40960,"size":81234,"index":12},
          "end":{"pts":56789,"offset":98765432,"size":80111,"index":3456}},
 "target":{"lat":-35.1234567,"lon":149.1234567,"alt_amsl":612.4,"refine":true},
 "hfov":null,
 "view":{"camera":{"lat":-35.12,"lon":149.12,"alt_amsl":700.0},"fov_deg":60.0},
 "title":"flight2_thermal"}
```

- `radius` (optional, metres, [10, 10000]): prepare's radius about the target
  (`config.prepare.radius`; thermal default 150, RGB 200). The viewer's default is the ground
  width the camera sees, `2 d tan(HFOV / 2)` at the median camera-to-target distance `d` and HFOV
  of the range, rounded to 10 m and clamped to [100, 2000].
- `max_height` (optional, metres above the target, [1, 10000]): prepare caps the ceiling at the
  target height + max_height (`config.prepare.max_height`), so pruning removes everything above it
  to the end of training. Absent: the data's own ceiling (canopy rule).
- `kind`: `"thermal"` (default when absent) or `"rgb"`; an rgb request also carries
  `rgb: {width}` (integer in [64, 16384], at most the recording's width, checked by select) and
  must have `target.refine` false; a thermal request has no `rgb`. Ingest fails if the
  recording is not of that kind.
- Validation (`app/src/request.js` `validateRequest`) is strict: known keys only, finite numbers,
  lat in [-90, 90], lon in [-180, 180], heights in [-500, 9000], `hfov` and `view.fov_deg` in
  [10, 120] degrees, `radius` and `max_height` as above, `range.start.pts <= range.end.pts`, integer `pts`, `offset` >= 0 and
  `size` >= 1, `timescale` > 0, `refine` boolean, `title` <= 120 characters. Optional: `hfov`,
  `radius`, `max_height`, `view`, `title` (absent or null), `source.lastModified`, endpoint `index`. A rejection names
  the field.
- `range.start` / `end`: the first and last frame (inclusive) as packet identity from the shared
  demuxer: `pts` = `timestampTicks` in track `timescale`, packet `offset` and `size`; `index` is a
  hint only. Ingest finds each endpoint in its frame list by offset, checks size, pts and
  timescale, and requires strictly increasing pts within the range; a mismatch fails ingest.
- `target.refine`: true lets selection's hot spot search refine the target; false makes it
  authoritative (`locate_hotspot` false, export target `"picked"`).
- `view.camera` is the start camera position; it looks at the export target; `fov_deg` vertical.
- Identity (`requestIdentity`): `{range: {timescale, start, end: {pts, offset, size}}, target,
  hfov, view}`, plus `kind: "rgb"` and `rgb` for rgb requests (thermal identities keep this form),
  and `radius` / `max_height` when given (identities without them are unchanged);
  `requestKey` = the first 16 hex digits of its canonical-JSON hash. A request job is
  `<kind>-<source hash, 16 hex>-<requestKey>`.
- Handover (window messages, same origin; local development pages also accept any
  `http://localhost:*` / `http://127.0.0.1:*` origin): the viewer opens
  `<websplat>/?request=<nonce>` (32 hex digits); WebSplat posts `{type:"websplat.ready", nonce}`
  to its opener after the device check (or `{type:"websplat.rejected", nonce, reason}` if the
  check failed) and waits 30 s for `{type:"websplat.request", nonce, request, file}` from the
  opener (other sources, origins, types and nonces are ignored). It then validates, checks the
  File's name and size against `source`, copies the File into OPFS while hashing it (staged under
  `incoming/<nonce>` with the Web Lock `websplat-incoming-<nonce>`, then moved to the job's
  `source/source.mkv`), saves the job and posts exactly one of `{type:"websplat.accepted", nonce,
  job}` or `{type:"websplat.rejected", nonce, reason}`. The page URL then becomes `?job=<id>`.
  An existing job with that id but another stored identity is refused; a different title
  updates `config.export.title` (re-runs export only).

## Job manifest (`websplat.manifest` v1)

OPFS layout, one directory per job:

```
websplat/
  jobs/<jobId>/
    manifest.a.json, manifest.b.json      alternating manifest slots
    source/source.mkv                     optional copy
    <stage>/gen-<g>/…                     stage outputs, one directory per generation
    <stage>/ckpt-<g>/…                    checkpoints of a running stage (same numbering)
```

Manifest:

```json
{"format":"websplat.manifest","version":1,"seq":17,"jobId":"…","created":"ISO",
 "app":{"version":"0.1.0"},"source":{…source identity…},"config":{…user options…},
 "stages":{"<id>":{"state":"done","version":1,"key":"sha256:…","generation":3,
   "outputs":{"frames.json":{"path":"select/gen-3/frames.json","bytes":117852,"hash":"sha256:…"}},
   "progress":{"unit":"frames","done":201,"total":201},
   "checkpoint":{"generation":2,"unit":"iterations","done":12000},
   "timings":{"startedAt":"ISO","elapsedMs":0,"phases":{"compile":0,"load":0,"work":0,"checkpoint":0}},
   "error":null}},
 "checksum":"sha256:… of the JSON with checksum set to \"\""}
```

Config keys (`config`): `proposal` (ingest; a request adds `range` = `request.range` and `hfov`),
`select` (selection options; a request job's are derived from the request and ingest's result:
`lat`, `lon`, `target_alt` = `alt_amsl - homeAltitude`, `locate_hotspot` = `refine`, `rotation`
and `hfov` from the request or proposal, `start`/`end` = the resolved range, `step` =
`max(1, ceil(frames / 1000))`, and with `refine` false `margin` = -0.25 (the picked target may
project up to a quarter frame outside the image, as telemetry poses can be tens of metres off;
measured 34 m on the orbit); an rgb request's: `lat`, `lon`, `target_alt`, `hfov`, `width`,
`start`/`end`, `budget` 500), `prepare` (a request's `radius` and `max_height`), `features`
(`maxNumFeatures`; RGB jobs 8192), `map` (`camera`; RGB jobs RADIAL), `export` (`title`, `description`, `view` = `request.view`,
`target` = `"picked"` when `refine` is false), `request` (a request job's identity plus
`source{name,size}`; in no stage's `configKeys`: display and reopen checks only), `standin`, and
run-time knobs (`speed`, `ckptMs`, `selectMode`, `rgbDecoder`: `auto` | `webcodecs` | `software`).

Rules:
- **Stage key** = canonical-JSON hash of `{id, version, config: {key: value or null for each
  configKey}, inputs: {upstream id: its outputsHash, "source": source hash}}`; a stage's
  `outputsHash` = canonical-JSON hash of the array of its output hashes, files in depth-first
  order with names sorted at each directory level. On load, a stage whose stored key differs from the recomputed
  key is invalid, and so is every stage downstream of it (transitively). Stale `version`s
  (format or stage) invalidate the same way.
- **Generations**: a stage writes into a new `gen-<g>/` directory (g = previous + 1), closes and
  flushes every file, hashes them, and only then publishes by writing a manifest with the new
  generation. A crash before publication leaves the previous generation in force; unpublished
  `gen-*` directories are deleted at startup. The previous published generation is kept until the
  next one is published (then removed).
- **Manifest writes**: serialise an immutable snapshot with the next `seq`, compute `checksum`
  over that snapshot (canonical JSON with `checksum: ""`), and write it into the slot not holding
  the current manifest (`a`/`b` alternate). On load, take the valid slot (format, version,
  checksum) with the highest `seq`. No rename is needed, so a reload during a write is safe.
- **Unreadable jobs**: no manifest files and an empty directory = no job. A slot of another
  `version` in either slot (unsupported, even next to a valid v1 slot), manifests present but
  none valid (damaged), or data without a manifest: the job is refused and **nothing is
  deleted**; only an explicit user action removes it. One damaged slot next to a valid one is an
  interrupted write and the valid slot is used.
- **Transactions** (publishing outputs or a checkpoint) are applied to a copy in the write queue
  and become visible (UI, later saves) only after their manifest write succeeded; a failed write
  leaves memory and storage at the previous state.
- **Completion** is one manifest write: the stage's outputs, `generation`, `state: "done"`,
  result and timings together. Old generations and checkpoints are deleted only afterwards.
- **Runs**: each stage attempt has a run identity; a checkpoint publication from an earlier,
  stopped or failed attempt is refused, and an attempt ends only after its pending publications
  have settled. Before an attempt (and after a failure) the stage's unpublished generation
  directories are deleted.
- **Save failures** (e.g. quota) fail the running stage with a storage error (`error.quota`);
  the in-memory manifest is rolled back to what was last written for that stage.
- **States**: `waiting, running, checkpointing, paused, interrupted, done, failed`. On load,
  `running`/`checkpointing` become `interrupted` (resumable from the last published checkpoint
  generation or from the start).
- **Single tab**: a tab holds the Web Lock `websplat-job-<jobId>` (`ifAvailable`, exclusive) for
  as long as it has the job open; another tab that cannot get it refuses to open the job.
- **Storage**: `navigator.storage.persist()` is requested when a job starts; denial is shown
  (data may be evicted) but does not stop the job. Quota errors fail the stage with a storage
  error; the job stays resumable.

## Stage module interface (app)

A stage is defined in `app/src/stages/<stage>/stage.js` with `defineStage` from
`app/src/pipeline/registry.js` and listed in pipeline order in `app/src/stages/index.js`
(`registerStageSet`). Fields:

- `id` (lower-case, also its OPFS directory), `title`, `version` (bump when its output changes;
  stale outputs are invalidated), `unit` (shown with progress: `bytes`, `frames`, `pairs`,
  `BA rounds`, `iterations`, …).
- `inputs`: upstream stage ids (or `'source'`); `configKeys`: the job config keys its output
  depends on. Both feed the stage key (manifest rules).
- `estimate(config, env)` → `{ total, seconds, overhead, cumCost, uncertain, checkpointEvery,
  checkpointSeconds }`: work count in `unit`, prior work seconds for `env.gpuClass` (`laptop`,
  `desktop`, `integrated`, `unknown`), prior non-work seconds (shader compile, load, evaluation),
  optional `cumCost(u)` (cost of the first u units, e.g. training slowing with Gaussians),
  `uncertain` for open-ended work, and the checkpoint interval and prior duration. `env` also has
  `sourceSize`, `frames` (from finished stages) and `results` (finished stages' return values).
- Estimates with an unknown `total` count down their prior and then report the remaining time as
  unknown (never zero); `uncertain` marks open-ended work in the display.
- `ready(config, results)` (optional): false makes the pipeline wait for user input before the
  stage (selection waits for the confirmed proposal).
- `run(ctx)` → a JSON-serialisable result (kept in the manifest as the stage's `result`). `ctx`:
  `config`, `env`, `signal` (AbortSignal: Stop), `sourceFile` (the MKV `File`: the OPFS copy or
  the user's re-selected file, verified by size and hash), `outDir` (write outputs here; they are
  hashed and published when `run` returns), `input(id)` → `{ dir, outputs, result }` of a
  finished upstream stage, `checkpoint` (the last published checkpoint `{ generation, dir, done,
  … }` or null), `nextCheckpoint()` → `{ generation, dir }`, `progress(done, total, extra)`,
  `phase(name, kind)` (the label is shown; `kind` `'work'` counts the time towards throughput,
  `'overhead'` does not; default: `compile`, `load`, `startup`, `eval`, `checkpoint` are
  overhead, any other label is work); progress `done` must not decrease within an attempt, `log(text, level)`, and `runWorker(factory, msg)`.
- `runWorker(() => new Worker(new URL('./x.worker.js', import.meta.url), { type: 'module' }),
  msg)`: the worker uses `serveStage(async (msg, api) => result)` from
  `app/src/workers/runtime.js`. `msg` also carries `outDir`, `checkpoint`, `ckptBase`,
  `firstCheckpointGeneration`, `config`, `env`. `api`: `progress`, `phase`, `log`, `stopped` /
  `throwIfStopped()` (Stop sends a message, then the worker is terminated after 3 s),
  `checkpoint(generation, writeFn, info)` (writes into `ckptBase + generation`, then waits until
  the controller has published it). Throwing an `AbortError` means stopped; any other error
  fails the stage with its message.
- Shared assets: the viewer's FFmpeg core is served at `vendor/ffmpeg/<version>/` (copied by
  `app/scripts/prepare-assets.mjs`; the app pins the viewer's `@ffmpeg/core` version). Stages may
  import from `tools/viewer/src` (allowed by `app/vite.config.js`).

## Change log

- v1.11 (2026-11-11): `trim_centre` and `trim_radius` in the splat metadata (train.py header,
  export PLY header, `meta.json`, SPZ gzip header), from the dataset bounds; the viewer's
  circular trim. Additive; stage versions unchanged.
- v1.10 (2026-11-10): RGB jobs default to 8192 SIFT features and the RADIAL camera (set when a
  job is first opened; earlier jobs keep 4096 and SIMPLE_RADIAL).
- v1.9 (2026-11-10): request `radius` and `max_height` (optional, in the identity when given;
  `config.prepare.radius` / `max_height`); RGB dataset bounds `max_vertical` and
  `vertical_prune_from` (train_rgb.py's too-tall pruning to the end, Brush fork patch 0017; RGB
  prepare stage `rgb-2`); RADIAL camera (`config.map.camera`, RGB undistortion with k2). RGB request jobs set
  `config.prepare.centre` `"target"`: the radius, initial points and ceiling are about the request's
  target (the standalone page keeps `train_rgb.py`'s model median). An accepted early stop gets the
  too-tall pass on its PLY (train worker).

- v1.8 (2026-11-10): Stop and accept (train record `accept`, `train.json` and result
  `iterations_planned` / `stopped_early`, export info `stopped_early`, export result
  `stoppedEarly`); training preview command. Additive; stage versions unchanged.

- v1.7 (2026-11-09): RGB sources: the `rgb` stage set (`websplat.ingest-rgb` v1, frames and
  dataset `kind: "rgb"`, RGB training input and export); request `kind` and `rgb.width`.
  Mapping memory: matches above 6M are thinned by a consistent keypoint subset (map result
  `matches`, `keptKeypoints`); RGB jobs use 4096 SIFT features and a 500-frame budget. The SPZ
  check allows Spark's float16 staging of both the colour coefficient and the colour.

- v1.6 (2026-11-09): `websplat.request` v1 (job request from the video viewer and its handover);
  `config.request`, `config.proposal.range`, `config.export.view` and `config.export.target`;
  ingest result `range` and `homeAltitude` (additive; stage versions unchanged).

- v1.5.2 (2026-11-09): `splat.spz` embeds the apcg_splat JSON in its gzip header.
- v1.5.1 (2026-11-09): stage 6 as written: `train.json` `trainer`, `brush_config`, `resumed`;
  checkpoint parameters raw and taken at a batch boundary; `state.json` fields.
- v1.5 (2026-11-08): stage 6 output as the export consumes it (`train.json`, Brush PLY or WSSP);
  stage 7 output.
- v1.4 (2026-11-08): stage 7a export details (splat PLY, SPZ, meta.json).
- v1.3.1 (2026-11-08): 5b as written by the prepare stage (additive: `image_id`, `skipped.budget`,
  `training`, dtype/shape records, `stats`, point order, hot spot precision).

- v1.3 (2026-11-08): native point-valuing fallback documented; mixed-version slots refused;
  transactions copy-on-write.
- v1.2 (2026-11-08): verified geometry is the single `WSTV` container (pair table, inliers,
  models) as implemented; training dataset steps, defaults, cold budget, `v_hot`, `extent_m`,
  `lr`; canonical JSON; checkpoints in `ckpt-<g>`; manifest snapshot, unreadable-job, completion,
  run and save-failure rules; phase kinds.
- v1.1 (2026-11-08): `ingest/proposal.json`, `track.codecPrivate`, `selection.mode` and the
  definition of `selection.considered`; checkpoint directories; stage module interface;
  verified geometry uses the matches container (stage 4's proposal), models optional.

- v1 (2026-11-08): first version (stage 0).
