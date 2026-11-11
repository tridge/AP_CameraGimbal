# Thermal Gaussian splats

Tools that build a 3D Gaussian splat of a thermal feature from an APCG radiometric
thermal mkv (as written by `tools/thermal_to_video.py` with `--bin`/`--tlog`
telemetry), and view it in a browser.

Example splats, rendered in your own browser: https://uav.tridgell.net/Splats/

The pipeline:

1. `extract_frames.py` reads the mkv and its per-frame telemetry, optionally locates
   the hot spot by triangulating the hottest pixel across the hottest frames, and
   keeps the frames whose telemetry pose puts the target in view. It writes
   contrast-enhanced 8-bit images for feature matching, the raw 16-bit frames for
   training, and `frames.json` with pose priors in a local ENU frame (metres,
   origin on the target, height above takeoff). Without a hot spot the target is put
   on the ground at the SRTM terrain height (tiles from terrain.ardupilot.org, cached in
   `~/.tilecache/SRTM1` as MAVProxy does); `--target-alt` overrides it and
   `--no-terrain` uses takeoff height.
2. `sfm.py` runs GPU SIFT, picks image pairs from the telemetry view geometry
   (similar look direction and range to the target, plus time neighbours), matches
   on the GPU and verifies with COLMAP. Two mappers:
   - `--mapper incremental` (default): COLMAP incremental SfM with the telemetry
     positions as priors. Most robust to bad telemetry; slow (hours for ~1700 frames,
     CPU bundle adjustment).
   - `--mapper prior`: start every camera at its telemetry pose, triangulate, then
     alternate pose-prior bundle adjustment and re-triangulation with tightening
     reprojection thresholds. Much faster, but relies on telemetry being roughly right.

   Either way telemetry seeds and georeferences the model and the final poses come
   from bundle adjustment of image features. `sfm_report.py` checks a model: track
   lengths, triangulation angles, per-image residuals and how far each camera moved
   from telemetry (a consistent offset points at a gimbal attitude convention or
   calibration problem).
3. `train.py` trains a gsplat model on the radiometric frames. Temperatures are
   mapped through a log scale (stored in `scale.json`) so both background texture
   and hot features drive the optimisation. Every frame showing a hot feature is used,
   plus evenly spread others up to `--max-images`; hot pixels get extra loss weight.
   View-dependent emission uses spherical harmonics, and `--lobe` adds a per-Gaussian
   emission lobe for sharply directional sources (e.g. a sector emitter) that
   low-order SH can only blur. `--pose-opt` and `--appearance` (per-image gain/offset)
   are opt-in; leave them off until fixed-pose results are understood. Held-out
   metrics (`metrics.json`) are reported separately for hot and other frames: peak
   temperature error, hot-region overlap and false hot detections. The hot spot is
   re-located from the refined poses. Outputs `splat.pt`, a standard 3DGS `splat.ply`
   (greyscale scaled temperature, lobe projected onto SH) and `splat_color.ply`
   (approximate colormap, for generic splat viewers).
4. `view_splat.py` serves an interactive viewer (viser). The Python process renders
   on the local GPU and streams frames to the browser, so view-dependent emission and
   the colormap are exact. Colormap, temperature range, view-dependence toggle and
   jump-to-training-view (optionally hot views only).

Rendered temperatures are apparent temperatures decoded from alpha-blended scaled
values: good for visualising where and from which directions a source is hot, not a
calibrated surface temperature (emissivity, atmosphere and mixed pixels are not
modelled).

`make_splat.sh` runs steps 1-3.

## Setup

Requires an NVIDIA GPU with a driver supporting CUDA 13, a host C++ compiler and
[uv](https://github.com/astral-sh/uv) (`python3 -m pip install --user uv`). The CUDA
compiler comes from PyPI wheels; gsplat's kernels are compiled for the local GPU on
first use (about a minute, cached in `~/.cache/torch_extensions`). The thermal maps
also need docker for OpenDroneMap (CPU only). An 8 GB laptop GPU is enough for the maps and
RGB splats at the default `--cap`.

```sh
tools/GaussianSplat/setup_env.sh            # creates tools/GaussianSplat/.venv
```

## Usage

```sh
PY=tools/GaussianSplat/.venv/bin/python
$PY tools/GaussianSplat/extract_frames.py FLIGHT_thermal.mkv WORKDIR --lat LAT --lon LON
$PY tools/GaussianSplat/sfm.py WORKDIR               # or --mapper prior
$PY tools/GaussianSplat/sfm_report.py WORKDIR
$PY tools/GaussianSplat/train.py WORKDIR             # optionally --lobe
$PY tools/GaussianSplat/view_splat.py WORKDIR     # then open http://localhost:8080
```

or `tools/GaussianSplat/make_splat.sh FLIGHT_thermal.mkv WORKDIR LAT LON [extract options]`.

Use a work directory on a large disk: a 20 minute orbit is ~1 GB of frames.

Notes:

- `--rotation 0` is needed for ZT30 captures converted with `thermal_to_video.py`'s
  MT11 default `rotation_deg=180`. `extract_frames.py` prints the median ray error
  of the hot spot triangulation; a large error (hundreds of pixels) means the
  rotation or gimbal yaw convention is wrong.
- The hot spot location replaces `--lat/--lon` when it is within 300 m and its rays
  agree within `--hotspot-max-error` pixels; `--no-locate-hotspot` keeps the given
  position (height `--target-alt`). It only sets the frame selection and local
  origin; `train.py` re-locates the hot spot with the refined poses.
- `sfm.py` is restartable: features, matches and verification are kept in
  `WORKDIR/colmap/database.db`; `--fresh` starts over.
- `train.py --iterations`, `--max-images`, `--cap` (hard limit on Gaussians, mind
  GPU memory) and `--radius` (metres of scene kept around the target) trade quality
  for time. Thermal images have low contrast, so the densification threshold
  `--grow-grad2d` defaults well below the usual RGB value.
- `sfm.py --colmap-dir` puts the database and model elsewhere, e.g. to try the prior
  mapper on a copy of the database while another mapper runs.
- The PLY files are in ENU metres with z up; generic viewers (e.g. SuperSplat) may
  need the scene rotated.

## Publishing

For public sites use the browser viewer below, which needs no GPU on the server.
`splat_site.py` serves a directory of splats with an index page and the full interactive
viewer for each, starting a `view_splat.py` process on the first visit to a splat and
stopping it once idle:

    SITE/DATE/FLIGHT/RUN/splat.pt      # from train.py
    SITE/DATE/FLIGHT/RUN/scale.json
    SITE/DATE/FLIGHT/RUN/description.txt   # optional, shown in the index and viewer

    .venv/bin/python splat_site.py SITE --host 0.0.0.0 --port 8088 --max-running 8

URLs are `/` (all splats), `/DATE/`, `/DATE/FLIGHT/` and `/DATE/FLIGHT/RUN/` (viewer).
New RUN directories appear without a restart. Put it behind a TLS reverse proxy that passes
websockets, e.g. for Apache 2.4.47 or later:

    ProxyPass        /splat/ http://HOST:8088/ upgrade=websocket
    ProxyPassReverse /splat/ http://HOST:8088/

Each viewer process takes about 600 MB of GPU memory; `--max-running` caps how many run at once,
`--max-clients` how many visitors one viewer serves.

## Browser viewer

`viewer/` is a static web viewer that renders splats on the visitor's GPU with
[Spark](https://sparkjs.dev/) (three.js, WebGL2), so the server only serves files. It shows
thermal splats with the same temperature decoding and colormaps as `view_splat.py` (colormap,
auto or fixed range, temperature under the pointer) and RGB splats in true colour. It renders
only when the view changes, and caps the render size (lower while orbiting) so integrated
GPUs cope. Live example: https://uav.tridgell.net/Splats/

The build (`viewer/dist/`) is also a stand-alone viewer page, `index.html`, that opens a splat
file from the visitor's disk (button or drag and drop; the file is never uploaded) or from
`?url=https://…` (the server must allow cross-origin requests; `&meta=META.json` adds
metadata). It reads `.ply`, `.spz`, `.splat`, `.ksplat` and `.sog`. PLY files from `train.py`,
`train_rgb.py` and `export_web.py --ply` carry their metadata (kind, up axis, orbit target,
temperature scale, start view) in a `comment apcg_splat {json}` header line that other PLY
readers ignore, so a thermal splat is a single file. SPZ files from `export_web.py` carry the
same JSON in their gzip header (an `AP` subfield of the FEXTRA field, RFC 1952, which gzip and
other SPZ readers skip). For an SPZ without it, the page uses a `.json` chosen with the file or,
for `?url=`, `NAME.json` or `meta.json` beside it; `&meta=` and a site's `meta.json` take
precedence over all of these. Other splats open in colour with a view fitted to them and an
up-axis choice.

The metadata fields: `kind` (`thermal` or `rgb`), `up`, `target` (orbit target, ENU metres),
`origin` (`lat`, `lon` and `alt_amsl` of the ENU origin; z is height above the takeoff
altitude), `camera` (start view), `rasterize_mode`, `trim_centre` and `trim_radius` (the
circular trim below), and for thermal splats `temperature_scale`, `hotspot`, `colormap` and
`display_range_c`. `train.py` and `train_rgb.py` set the trim to 0.85 times the radius beyond
which training prunes Gaussians (`--radius`; train.py prunes at 1.1 times it), about the
training centre, and record that radius as `bounds` in `scale.json` for `export_web.py`.

The panel's triangle collapses it (narrow screens start collapsed); + and − zoom without a
mouse. The gear opens view settings, kept in the browser's local storage:

- Keep the horizon level (on by default): no roll, the camera stays at least 5 degrees above
  the orbit target's horizon and above a ground estimate from the splat (per 1/16 of the trim
  circle, the 10th percentile height of its opaque Gaussians), and panning stays horizontal,
  as MAVProxy's map3d. Off is the free orbit.
- Trim to a circle (on by default): hides the Gaussians outside `trim_radius` about
  `trim_centre` (else the target), with a radius slider. Files without the fields get an
  estimate: 0.85 times the horizontal distance at which the density of Gaussians drops off.
- Satellite map below, for splats with an `origin`: Esri World Imagery (as the video viewer's
  3D map, same credit) on a flat disc at a low ground estimate, drawn under the splat, with a
  splat opacity slider. There is no terrain, and the imagery's own registration error remains;
  without network access the option reports that the tiles could not be loaded.

Build the viewer once (needs Node.js 20.19 or later), then add splats to a site directory:

    (cd tools/GaussianSplat/viewer && npm ci && npm run build)
    $PY tools/GaussianSplat/export_web.py SPLATDIR SITE DATE/FLIGHT/RUN --title "TITLE" \
        --video RECORDING.mkv --video-name DATE_FLIGHT_thermal.mkv

`SPLATDIR` is a `train.py` or `train_rgb.py` output (`splat.pt`, `scale.json`, optionally
`description.txt`). `export_web.py` writes:

    SITE/index.html                     # list of all splats in SITE, regenerated each time
    SITE/viewer/                        # copied from viewer/dist, incl. the open-a-file page
    SITE/DATE/FLIGHT/RUN/index.html     # viewer page
    SITE/DATE/FLIGHT/RUN/meta.json      # kind, temperature scale, start view, description
    SITE/DATE/FLIGHT/RUN/splat.spz      # compressed splat, loaded by the page
    SITE/DATE/FLIGHT/RUN/splat.ply      # download: 3DGS PLY with the metadata in its header
    SITE/videos/NAME                    # download: the source recording with embedded telemetry

Thermal splats are written in greyscale with the emission lobe projected onto the spherical
harmonics; the viewer decodes the blended value to degrees C per pixel. SPZ compression makes
the files about 15 times smaller than `splat.pt` (a 600k Gaussian splat is about 11 MB) at the
cost of reading the hottest pixels a few degrees low. The per-frame source strength is not
available in the browser.

### Deploying

`SITE` is plain static files: copy it to any web server, e.g.

    rsync -a --chmod=D755,F644 SITE/ HOST:public_html/Splats/

Adding a splat rewrites `SITE/index.html`; leave out rsync's `--delete` to keep splats that are
only on the server. To let `?url=` links on other sites (e.g. plot.ardupilot.org/splat) open the
splats, send `Access-Control-Allow-Origin: *` for the site, e.g. an Apache `.htaccess` with
`Header always set Access-Control-Allow-Origin "*"`. No server-side code or GPU is involved. The server should send
`viewer.js` as `text/javascript` (modules are refused otherwise); `.spz` needs no particular
type. `viewer.js` is 3.4 MB, mostly Spark's WebAssembly, so enable compression for it if the
server does not already, e.g. for Apache:

    AddOutputFilterByType DEFLATE text/javascript application/json

or for nginx `gzip on; gzip_types text/javascript application/json;` (the build also writes
`.gz` copies for `gzip_static`). Visitors download the viewer once (it is cached) plus each
splat they open.

`export_web.py SPLATDIR --ply OUT.ply` writes a single self-describing PLY from any `splat.pt`,
for sharing or opening in the stand-alone page. SPZs exported before the metadata was embedded
get it with `node viewer/embed-meta.mjs --write SITE` (or `NAME.json` for a sample pair; without
`--write` it only reports): it takes the JSON from the PLY header beside the SPZ, else from
`meta.json` less its `downloads` and `video`, and rewrites only the gzip header, checking that the
SPZ decodes to the same bytes. `viewer/public/colormaps.json` is generated
from `colormaps.py` (`colormaps.py --json viewer/public/colormaps.json`); regenerate it after
changing the colormaps.

### plot.ardupilot.org/splat

The stand-alone page is installed at https://plot.ardupilot.org/splat/, like
plot.ardupilot.org/video (see `tools/viewer/docs/DEPLOYMENT.md`): static files only, with no
upload service or server process.

- Build: `cd tools/GaussianSplat/viewer && npm ci && npm run build`. All asset paths are
  relative, so no base path is needed.
- Upload all of `dist/` as a new release, `/data/sites/logviewer/splat/releases/<release>/`
  (755 directories, 644 files), check a SHA256 manifest, then switch
  `/data/sites/logviewer/splat/current` to it atomically (temporary symlink and `mv -T`). No
  nginx reload is needed for updates; roll back by switching `current` back.
- Once, as root: copy `deploy/plot-splat.nginx` to `/etc/nginx/snippets/plot-splat.conf`, add
  `include snippets/plot-splat.conf;` inside the plot.ardupilot.org HTTPS server block next to
  the video include, run `nginx -t`, then `systemctl reload nginx`. Create the first release
  and `current` before reloading.
- Samples: the page links `samples/log80-run13` and `samples/randy-run2` (`.spz` and `.json`,
  the `splat.spz` and `meta.json` from `export_web.py`). They live in
  `/data/sites/logviewer/splat/samples/`, outside the releases, served by the snippet's
  `/splat/samples/` location.
- Check: `/splat/` shows the open-a-file page, `viewer.js` is served as JavaScript with gzip,
  and a local `.ply` and a `?url=` link both open.

Spark and three.js are MIT licensed; the build writes their notices to `LICENSES.txt`, linked
from the page.

## Poses from the RGB camera

On day flights the camera's wide RGB lens shares the gimbal with the thermal camera and has far
more texture, so SfM on its video is much more reliable than on thermal frames:

    $PY tools/GaussianSplat/extract_rgb_frames.py RGB.mp4 RGBWORK --reference WORKDIR --bin FLIGHT.BIN \
        --start-utc 2024-11-06T23:47:08.918Z    # UTC of the first video frame
    $PY tools/GaussianSplat/sfm.py RGBWORK --camera-model RADIAL
    $PY tools/GaussianSplat/transfer_poses.py RGBWORK WORKDIR --seed SEEDED
    $PY tools/GaussianSplat/sfm.py SEEDED --mapper prior --max-angle 180 --max-scale 5 \
        --neighbours 150 --sequential 10 --prior-sigma 3

The camera's recorder names files by local time and the file modification time is not
reliable, so give the UTC of the first video frame with `--start-utc` (from the camera's
own log, or by matching a turn in the video to the dataflash attitude). The RGB video may be
the camera's mp4 or the MPEG-TS from `tools/video_to_ts.py`. On transit flights
`--min-depression 10` drops frames looking at far hills and sky, which only slow SfM down.

`transfer_poses.py` interpolates the RGB poses to each thermal frame time, fitting the clock
offset and the lens boresight so the hottest pixels triangulate to one point, and writes a
model the trainer accepts (`train.py WORKDIR --sparse WORKDIR/colmap_rgb/sparse/final`). Hot
spots at the frame edge are ignored and, when several sources are in view, only the largest group
landing in one place is fitted (`--cluster`); `--dt 0 --no-boresight` skips the fit, and
`--no-boresight` alone fits only the clock (better with few hot frames, where the boresight
overfits). The transferred poses locate a hot spot well but are not consistent enough for a
splat or a map, so `--seed SEEDED` also writes a thermal workdir (frames linked, not copied)
whose pose priors are the transferred poses, and the thermal SfM above refines them.

`train_rgb.py RGBWORK` trains an RGB splat (as a check on the RGB poses, or in its own right);
`view_splat.py` and `splat_site.py` show it in true colour. `extract_ts_frames.py RECORDING.ts
RGBWORK` takes frames and pose priors from a camera recording with MISB ST 0601 KLV instead of a
dataflash log.

Splats need views from many directions, such as an orbit of the target; a transit or mapping
flight that sees each spot from a narrow range of angles gives an unrecognisable splat. Use
the thermal map below for those.

## Thermal maps

A surface model and orthophoto from the RGB frames, with the radiometric thermal frames
draped on it, as GeoTIFFs, GeoJSON hot spots, a Google Earth KMZ and a static web page.
Either with COLMAP dense stereo (CUDA):

    $PY tools/GaussianSplat/dense_rgb.py RGBWORK --centre E,N,U
    $PY tools/GaussianSplat/surface_map.py RGBWORK
    $PY tools/GaussianSplat/drape_thermal.py WORKDIR RGBWORK/dense/map
    $PY tools/GaussianSplat/make_map_site.py RGBWORK/dense/map SITE --title TITLE

or with OpenDroneMap (docker, CPU), which gives a cleaner orthophoto:

    $PY tools/GaussianSplat/odm_prep.py RGBWORK ODM/project --centre E,N,U
    ODM_IMAGE=opendronemap/odm@sha256:c0d9a0c56fcb97277b9a9f0c30f2c274afb14344ec378c9bb1d231623017596b
    docker run --rm -w /datasets/project --user $(id -u):$(id -g) -e HOME=/tmp \
        -v $PWD/ODM:/datasets -v $PWD/ODM/storage:/code/storage $ODM_IMAGE \
        --project-path /datasets project \
        --geo /datasets/project/geo.txt --cameras /datasets/project/cameras.json \
        --camera-lens perspective --boundary /datasets/project/boundary.geojson \
        --sfm-algorithm incremental --feature-quality high --matcher-neighbors 0 \
        --use-hybrid-bundle-adjustment --no-gpu --skip-3dmodel \
        --pc-quality high --pc-filter 2 --dsm --dem-resolution 12 --orthophoto-resolution 8 \
        --dem-euclidean-map --texturing-data-term area
    $PY tools/GaussianSplat/ortho_mosaic.py ODM/project
    $PY tools/GaussianSplat/odm_import.py ODM/project RGBWORK SEEDED MAP --cell 0.15 --mesh-step 5 \
        --ortho ODM/project/odm_orthophoto/best_view.tif
    $PY tools/GaussianSplat/drape_thermal.py SEEDED MAP --sparse MAP/thermal_model
    $PY tools/GaussianSplat/make_map_site.py MAP SITE --title TITLE --prefix DATE_FLIGHT_

The ODM image is pinned to the version the published maps were made with; `--max-concurrency`
limits the CPU cores it uses.

`odm_prep.py` keeps frames looking well below the horizon, drops the blurriest, masks aircraft
structure entering from the top of the frame and writes `geo.txt` and the calibrated camera;
`--telemetry --camera-from MODEL` chooses frames by telemetry attitude before the RGB SfM is done.
For an orbit of oblique frames ODM's own orthophoto smears outward from the centre;
`ortho_mosaic.py ODM/project` builds one from the steepest view of each cell instead
(`odm_import.py ... --ortho ODM/project/odm_orthophoto/best_view.tif`).
`odm_import.py` aligns the thermal model to ODM's georeferencing through the shared RGB frames.
On long flights an RGB SfM without GPS priors bends (several degrees, tens of metres) while
ODM's GPS-constrained cameras stay true: `odm_model.py ODM/project RGBWORK/colmap_warp --warp
RGBWORK` bends every RGB frame onto ODM's cameras, to use as `--rgb-sparse` for both
`transfer_poses.py` and `odm_import.py`.
For a map the thermal frames should cover the whole area, not just a target:
`extract_frames.py ... --no-locate-hotspot --radius 330 --max-median-dev 40` keeps every frame
whose view axis lands within 330 m of the origin, including frames whose median temperature
has drifted (the drape levels them).
`drape_thermal.py` levels per-frame temperature offsets (radiometric drift) before blending.
Hot spots are reported two ways: triangulated from the hottest pixel of each hot frame
(reliable for a ground fire under canopy) and as cells of the surface where several views
agree (a canopy-top surface puts a fire under trees in the wrong place).

## Reproducing the published results

The commands below use the trial's flight data files by their dated names (e.g.
`2024-10-16_flight1_thermal.mkv`). The thermal mkvs are already
converted with `tools/thermal_to_video.py` (`--gimbal-source siat --byte-order big --rotation 0
--klv`) and the RGB `*_h264.ts` files are the camera's mp4, re-encoded to H.264, through
`tools/video_to_ts.py` (`--gimbal-source siat`). Run
from a work directory on a large disk, with `T=tools/GaussianSplat` and `PY=$T/.venv/bin/python`
as paths into this repository. SfM and ODM results vary a little from run to run.

### 2024-11-07 flight 2: thermal splat (run7) and map

Inputs: `2024-11-07_flight2_thermal.mkv`, `2024-11-07_Rec_0006_h264.ts`, `2024-11-07_00000019.BIN`.

    $PY $T/extract_frames.py 2024-11-07_flight2_thermal.mkv thermal --lat -35.588281 --lon 148.969854 \
        --hotspot-min-c 60 --hotspot-max-error 150 --rotation 0 --margin 0 --max-range 800
    $PY $T/extract_rgb_frames.py 2024-11-07_Rec_0006_h264.ts rgb --reference thermal \
        --bin 2024-11-07_00000019.BIN --start-utc 2024-11-06T23:47:08.918Z --fps 2
    $PY $T/sfm.py rgb --mapper incremental --camera-model RADIAL --max-features 4096 --max-angle 45 \
        --max-scale 3 --neighbours 20 --sequential 15 --prior-sigma 10
    $PY $T/transfer_poses.py rgb thermal --seed seeded
    $PY $T/sfm.py seeded --mapper prior --max-angle 180 --max-scale 5 --neighbours 150 \
        --sequential 10 --prior-sigma 3
    $PY $T/train.py seeded --min-obs 50 --out seeded/splat_run7 --lobe --source-gain --gain-scope scene \
        --min-elevation 5 --focus-radius 10 --hot-tolerance 3

The RGB SfM takes hours (incremental mapper, CPU bundle adjustment); training takes minutes.
The map: `odm_prep.py rgb ODM/project --centre=-3.3,9.2,-1.8`, then ODM, `ortho_mosaic.py`,
`odm_import.py`, `drape_thermal.py` and `make_map_site.py` as in "Thermal maps" (the published
map used ODM's own orthophoto, without `ortho_mosaic.py` and `--ortho`).

### 2024-10-16 flight 1 (morning): thermal map

A 30 minute survey flight. Inputs: `2024-10-16_flight1_thermal.mkv`,
`2024-10-16_flight1_Rec_0003_h264.ts`, `2024-10-16_V32-00000017.BIN`.

    $PY $T/extract_frames.py 2024-10-16_flight1_thermal.mkv thermal --lat -35.588812104908975 \
        --lon 148.9695676959217 --target-alt 10.83 --no-locate-hotspot --radius 330 --max-median-dev 40
    $PY $T/extract_rgb_frames.py 2024-10-16_flight1_Rec_0003_h264.ts rgb --reference thermal \
        --bin 2024-10-16_V32-00000017.BIN --start-utc 2024-10-16T01:01:21.691Z --fps 1 --min-depression 10
    $PY $T/sfm.py rgb --mapper incremental --camera-model RADIAL --max-features 4096 --max-angle 45 \
        --max-scale 3 --neighbours 20 --sequential 15 --prior-sigma 10
    $PY $T/odm_prep.py rgb ODM/project --telemetry --camera-from rgb/colmap/sparse/final --max-images 800
    # ODM as in "Thermal maps", then:
    $PY $T/ortho_mosaic.py ODM/project
    $PY $T/odm_model.py ODM/project rgb/colmap_warp --warp rgb
    $PY $T/transfer_poses.py rgb thermal --rgb-sparse rgb/colmap_warp --out thermal/colmap_warp \
        --hot-min-c 60 --no-boresight --seed seeded
    $PY $T/sfm.py seeded --mapper prior --max-angle 180 --max-scale 5 --neighbours 150 \
        --sequential 10 --prior-sigma 3
    $PY $T/odm_import.py ODM/project rgb seeded map --rgb-sparse rgb/colmap_warp --cell 0.15 \
        --mesh-step 5 --ortho ODM/project/odm_orthophoto/best_view.tif
    $PY $T/drape_thermal.py seeded map --sparse map/thermal_model
    $PY $T/make_map_site.py map site --title "2024-10-16 flight 1 (morning): RGB map with thermal" \
        --prefix 2024-10-16_flight1_odm_

`odm_model.py --warp` is what makes this flight work: the RGB SfM alone bends by about 9 degrees
and 13 m over the flight, while ODM's cameras agree with the telemetry GPS to about 4 m. The
published map used the lens calibration from the 2024-11-07 RGB model for `odm_prep.py`
(same camera); the flight's own model works as well. Expect holes where ODM's surface is weak,
large temperature levelling offsets between frames (the thermal camera drifts over the flight),
and the small heater reported as a triangulated hot spot rather than as agreeing surface cells.

### RGB splats from a recording with KLV

For an MPEG-TS with MISB ST 0601 KLV (the camera's recorder, `tools/video_to_ts.py`, or the
Raspberry Pi camera used for the `2026-11/randy` splats) no dataflash log is needed:

    $PY $T/extract_ts_frames.py RECORDING.ts rgb --fps 3
    $PY $T/sfm.py rgb --mapper incremental --camera-model RADIAL --max-features 8192 --max-angle 60 \
        --max-scale 5 --neighbours 30 --sequential 10 --prior-sigma 5
    $PY $T/train_rgb.py rgb --out splat --width 1920 --holdout 25

With good telemetry, `sfm.py --mapper prior --thresholds 16,4` is a fast mapping mode: two
triangulate and bundle adjust rounds instead of the default four (`40,16,8,4`). On a 468-frame
1920-wide orbit (8192 features, RADIAL) prior mapping took 354 s instead of 1581 s, with a lower
mean reprojection error (0.74 vs 0.99 px) but shorter tracks, for 0.15 dB less held-out PSNR at
7k iterations. `train_rgb.py --holdout-names FILE` holds out the images named in FILE (one per
line) instead of every `--holdout`th, so runs like these are scored on the same views.

`train_rgb.py` defaults to SH degree 1 and drops Gaussians taller than 1 m: with views only
from above, higher SH degrees and vertically stretched Gaussians show up as a coloured haze
over the scene from low viewing angles. Publish a splat by copying `splat.pt`, `scale.json`
and a `description.txt` into a `SITE/DATE/FLIGHT/RUN/` directory of `splat_site.py`.
