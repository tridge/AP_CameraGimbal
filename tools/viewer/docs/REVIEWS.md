# Review record

Claude Code reviewed the plan, then each implementation stage through the
persistent `claude` skill session. Full local transcripts and browser/codec
measurements are retained in `/data/review/web-viewer/`.

## Plan

Incorporated: standard MISB tags90/91 and APCG precedence; independent mission/
fence implementation rather than copying GPL source; explicit display/raw pixel
coordinates; multiple clock anchors; local elevation and datum distinction;
single-thread WASM; prefix-hosted assets; explicit browser codec qualification.
Real2.6GB thermal tests chose indexed bounded mini-Matroska decoding, avoiding
repeated full-recording seek scans. Direct WORKERFS was a feasibility test only.

## Stage1: media and telemetry

- Replaced per-188byte TS extent objects with constant-size per-frame packet
  spans, PID and PES header descriptors, reconstructing payloads on demand.
- Accepted valid synchronous KLV cells with random-access0, preserved invalid
  full-range tag precedence and checksum rejection.
- Applied supported MP4 edit offsets to movie PTS/DTS while keeping original
  mediaPTS/mediaDTS. Fragmented MP4 and signed B-frame composition are tested.
- Indexers jump compressed VCL data and read SEI NALs, including suffix SEI;
  separate sample-table caches prevent read-ahead thrashing on large HEVC MP4s.
- Added bounded TS resynchronization, late-KLV diagnostics, truncated Matroska
  prefix recovery, safe-integer preservation and cancellation/lifetime checks.
- Native decoding now keeps a persistent WebCodecs pipeline with a bounded
  frame cache rather than rebuilding and re-decoding a GOP on every frame.
  Unsupported configuration probes are cached; file changes close resources.
- Tested the actual built classic software worker under `/viewer/`; the emitted
  worker contains no module imports/exports, so its importScripts works with
  the current build configuration. No assumed build-format change was needed.

Tests include Python MISB golden vectors, sparse offsets beyond2GiB, real full
thermal/AVC/HEVC/TS scans and exact raw-pixel hashes. Decoded data, packets and
WASM working sets are bounded; the timeline index still scales with frame count.

## Stage2: log, terrain and map domain

The first broad review timed out; a narrowed continuation completed. Fixed:

- Pose freshness requires explicit finite age within2seconds.
- Terrain coverage never silently falls back to a flat height over a void or
  outside coverage. The hosted viewer now uses quantized mesh directly and has
  no manual elevation-file or flat-ground controls.
- Added frame-time mission/fence/home selection, completion/confirmation/
  rejection times, and an explicit latest-plan overview option. Unconfirmed and
  uncertain selections are labelled. Relative altitude uses the applicable home.
- Corrupt/truncated MAVLink headers rescan bytewise instead of swallowing the
  next valid packet. Mission segments use the tested domain splitter.
- Both map modes use the same typed inclusion/exclusion fence and altitude model.
  Unresolved mission altitude is labelled and is not inserted into a3D route.

Real BIN/tlog requalification preserves the81-item mission and26vertex fence.
UAVLogViewer was inspected as a behavior reference; no source fragments copied.

## Stage3 and final qualification

UI/controller review and completed qualification results are recorded below as
reviews finish. Browser tests run in a dedicated visible Xephyr window, not the
operator's main desktop. File/network privacy, decoder accuracy and map semantics
are acceptance checks rather than visual-only demonstrations.

### Stage3 refinements

Completed Claude UI/map review. Addressed all material findings:

- Unknown UTC in frame mode shows no applicable plan and an explicit time-
  unavailable label; the latest-log overview requires selecting it.
- Index errors become terminal, retaining a playable prefix and stopping playback
  at its end. Range/calibration failures reset to auto display before the new
  frame's telemetry is published, preventing mismatched image/pose panels.
- Map provider/terrain/datum configuration prepares and validates before commit;
  a failed load preserves the previous configuration and exported datum.
- Time-anchor status displays the additional persisted offset. Findings now
  export projectionRawRotationDegrees0 and source clock-quality warnings.
- Aircraft markers and video tracks reject missing/stale position ages.
- File replacement resets decoder, pending work and busy state. Open failures
  surface in the error banner. Rapid stepping tracks requested frame IDs rather
  than repeatedly using the last committed number; batch sorting reanchors IDs.
- Added keyboard-accessible raw pixel selection and explicit contrast eligibility.
- Visual inspection found a CSS grid intrinsic-size issue cropping the bottom of
  thermal images. The canvas now fills a fixed stage with correctly contained
  content. Tests hit-test and read all four sensor corners at0/180degrees; they
  explicitly prevent stale-hover false positives.

Production qualification:13 tests pass in visible Chrome155 inside Xephyr99,
including real files and damaged/missing-calibration fixtures.574 audited HTTP
requests were local GETs with imagery disabled. Ten physical Xephyr screenshots
and viewport captures were retained. The2.759GB archive's16,630 frames indexed
incrementally; first visible frame about1.1seconds, complete UI index about14s.
First/middle/final whole raw SHA256 matched independent PyAV and source metadata.
Measured main-page JS heap was about84MiB; this excludes workers/WASM/GPU and is
not a cross-browser memory guarantee. Real BIN qualification is81 mission items
and **one inclusion polygon with26vertices**, not26separate fences.

Final review and any final rendering refinements are appended after completion.

### Stage4 final review

Claude's final scoped review identified **no remaining correctness blockers for
local field review**. It confirmed the time/mission, prefix-end, display-recovery,
provider-atomicity, freshness and parser corrections and the qualified built
prefix. This is a code review, not a field-accuracy certification.

The nonblocking packaging/privacy notes were also addressed: a deployed About /
licences page includes GPL text, Cesium notices, dependency integrity and source/
build references; map viewport sharing by the default OSM provider is explicit
in the README; image CSP hosts are restricted to the supported providers.
No public server deployment or git push was performed.

Additional visual qualification zoomed the typed fence fixture in both2D and3D:
inclusion/exclusion polygons and circle fills have the expected locations,
colours and radii. Cesium disables native ellipse outlines when ground-clamped;
the coloured fills remain visible and polygon boundaries use clamped polylines.

Final release build qualification: **14 browser tests passed** after the notices,
restricted image CSP and versioned assets were packaged. The added mobile390×844
check has no horizontal overflow and follows the real About popup to locally
served licence text/notices and pinned dependency integrity. Across14 contextual
network audits,611 requests were local GETs with imagery disabled;12 physical
Xephyr captures are retained. The final thermal first-frame time was0.766s and
full UI index13.791s on this test machine; raw SHA256 parity remains exact.

Current source/format checks:51 unit/domain/codec tests pass, with one optional
local-file index test skipped by default (real-file browser qualification runs
separately). Build, formatter and held-prefix browser qualification pass. See
[QUALIFICATION.md](QUALIFICATION.md) for the actual command, build identity and
artifact paths. Google imagery with the user's real key and live SupportProxy
remain outside this qualification scope; the latter is a planned future adapter.
