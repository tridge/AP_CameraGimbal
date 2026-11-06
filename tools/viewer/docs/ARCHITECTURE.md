# Architecture and extension contracts

## Sources and presentation

`LocalSource` owns a worker per selected recording. `openMedia(File)` sniffs the
container without reading the entire file. Its async `index()` yields bounded
batches; it has independent index and packet readers so seeking does not disrupt
indexing. Closing a source prevents pending reads and superseded batches from
being published. The UI keeps a presentation-sorted view while decoder adapters
retain decode order and stable frame IDs.

The source index retains MP4 decoder preroll/postroll; only frames marked
presentable enter the UI timeline and map track. Per-track edit intervals apply
to classic and fragmented samples. A separate decodable flag prevents initial
mid-GOP deltas from stranding playback: initial selection waits for a keyframe,
and seeking past the undecodable prefix preserves elapsed PTS.

The image footprint samples the raw perimeter. A complete perimeter closes;
contiguous ground-visible runs remain open when any perimeter portion points
above the horizon or has unavailable terrain. The map explains partial or absent
footprints. No closure edge is invented through sky or missing coverage. Thermal
outer HFOV takes precedence over nested RGB lens metadata; a user override is
explicit, and its source is retained with findings.

Footprint pieces use dynamic Cesium position properties and reuse their line
primitives during playback. Replacing constant geometry on every frame triggers
asynchronous rebuilds and can leave a valid outline absent from most rendered
frames. Qualification checks rendered yellow pixels during continuous movement,
in 2D and 3D, rather than relying only on entity visibility flags.

Tracks contain codec, codec initialization, dimensions and timescale. Frames
contain stable ID, track ID, original PTS/DTS, duration/keyframe flag, packet
location/compact TS descriptor and optional telemetry/thermal metadata. Packet
bytes are fetched only by `readPacket(frame, {signal})`. MP4 edit-list movie
mapping preserves original mediaPTS/mediaDTS separately. PTS is never promoted
to UTC or assumed to match a filename clock.

`FrameDecoder` selects native WebCodecs when supported, with a bounded decoded
frame cache and persistent decoding. The software adapter constructs a small
Matroska from indexed compressed packets and codec initialization, then feeds
it to a dedicated single-thread FFmpeg worker. Only one output frame is retained;
the full recording never enters WASM memory. Both return either original gray16
samples or display RGBA with an explicit kind, dimensions and source frame ID.

Future source adapters should supply the same append-only batch notifications,
tracks, stable frames and cancellable packet reads. A live source additionally
needs a bounded ring with availability bounds, stream epochs, rewind semantics
and transport/telemetry latency provenance. Do not make live transport code part
of the map or temperature renderer.

## Telemetry and clocks

The normalized schema is `apcg.telemetry.v1`. Unknown fields are retained, unsafe
JSON integer literals remain decimal strings, and invalid/missing values remain
null. MISB is converted to this schema; co-carried APCG takes precedence. KLV
presence and validity remain separately identifiable. Thermal metadata retains
the outer `apcg.thermal.v1` object and explicit raw calibration.

TS stream type 0x15 uses exact PTS matching for conforming complete metadata
cells. Type 0x06 and legacy unmarked but independently validated complete cells
use nearest available PTS with a two-second trailing/half-frame leading window
(the latter capped at 100 ms). Reused component ages include the absolute timing
difference, while original metadata UTC stays unchanged. The association and
signed time difference are provenance. Held snapshots are bounded and cleared
on relevant transport discontinuities. Sparse metadata PES are completed when
their declared lengths arrive, without waiting for another metadata packet.

Keep three axes: container playback time, recorded source UTC, and corrected
pose-selection UTC. `PoseTimeline` looks up/interpolates valid embedded pose;
clock anchors never rewrite video packets or labels. Per-file offset and anchors
are exported with findings. Repeat/backward UTC diagnostics help identify source
label problems. No global correction is inferred from the known lamp position.

## Geometry and map state

Domain geometry uses raw sensor coordinates and an explicit nominal pinhole FOV.
Gimbal roll/pitch are level-referenced and yaw is vehicle-relative. Aircraft
heading supplies the earth-relative yaw. Per-field age must be finite and within
2 seconds. WGS84 local curvature radii convert NED ray positions to latitude and
longitude. The first ground crossing is refined by bisection. No DEM, unknown
coverage, void terrain, an upward ray or stale pose yields no ground coordinate.

Camera altitude is AMSL. The configured quantized-mesh provider supplies picking
heights through TerrainElevation, independently of globe mode/LOD. Orthometric
SRTM tiles are used directly; ellipsoidal tiles subtract the configured geoid
offset for picking. Displayed entities use the terrain's numeric reference.
The adapter caps concurrent loads, cached tiles and bytes; rejects unknown
coverage; cancels and ignores obsolete generations; and negative-caches failures.
A32×32 triangle index accelerates the pinned Cesium quantized layout, verified
against the public interpolation API. Unknown layouts keep that public path.
One ray uses one advertised level (at most 12), with tile keys/datum in findings.
Tile completion coalesces a refresh of the current frame. Marking captures its
frame/pose before awaiting terrain, discards results after file replacement,
and rejects a terrain-configuration change. No flat fallback is invented.
Display rotation transforms drawing and pointer conversion only; findings store
raw pixels and the projection rotation hypothesis.

Optional video projection samples a bounded 16×16 grid across the raw image
plane through the same ElevationModel.project calls. Triangles touching missing
intersections are omitted. A textured Cesium mesh uses the resolved AMSL heights
and the current map datum in both 2D/3D, with a small surface offset to avoid
z fighting. The texture comes from the rendered, unrotated image before review
annotations or viewport zoom. Texture storage is reused for successive frames;
geometry is reused until pose, FOV, image dimensions or terrain change. Terrain
completion invalidates the current mesh. Disabling, file replacement and viewer
closure release the projection's GPU resources.

The map model owns aircraft, track, footprint, hovered ground point, mission,
fences and findings. Both 2D and 3D share the same entities. Route segments come
from the domain mission splitter, not ad-hoc links between every logged command.
Altitude frames remain AMSL/home-relative/terrain-relative; unresolved altitude
is labelled and excluded from 3D route lines. Inclusion/exclusion polygons and
circles retain their meaning. Provider/terrain changes prepare asynchronously
and commit atomically, preserving the previous map on failure or supersession.

Video navigation uses a domain track separate from log/mission overlays. Gap
checks run before decimation, and explicit segment IDs survive long intervals
between displayed samples. Original frame PTS binds each point to the video;
aligned poses supply its location. Nearest-segment picking uses CSS window
pixels, an 8 px radius and current-time tie breaking. 3D horizon/depth occlusion
rejects hidden points. Clock/offset changes rebuild the association.
Large builds yield in chunks and abort on newer builds or file replacement;
old lines remain visible but cannot seek while their replacement is pending.
The hard display budget omits whole segments rather than connecting gaps.

Follow is independent of Cesium trackedEntity. The checkbox starts on, pauses
automatic movement while dragging, and always respects fresh position telemetry.
2D pans retain frustum width and heading. 3D transports camera position/direction/up
between old and new target ENU frames, preserving range/orientation/roll without
a nadir singularity. With Follow off the camera is manual; Center is a one-shot
recentring operation. SDK double-click tracking is disabled so it cannot diverge
from the checkbox. Input listeners and track builds are released on destruction.

## Logs

A separate worker streams ArduPilot BIN or timestamped MAVLink tlog records in
bounded chunks. Log mission/fence snapshots are not unions of repeated dumps.
Sequence, complete/partial/confirmed/rejected state, source vehicle and relevant
times survive parsing. `selectLogAt` and `homeAt` select evidence available at a
frame's pose UTC; a separate latest-log overview is explicit in the UI. Unknown
video time in frame mode never silently becomes the latest plan.

Track receipt UTC, autopilot boot-derived UTC, clock segments and diagnostics
remain distinct. Vehicle telemetry from an optional log does not overwrite the
embedded camera's pose. CRC failures resynchronize without swallowing following
valid messages. Older unsupported record variants remain diagnostic evidence.

## Review annotations and thermal tools

A finding stores a globally unique ID, human label/note, selected-file identity,
source frame ID/PTS, raw pixel/value, recorded/pose UTC, calibration, alignment,
pose/FOV, elevation and display/projection orientation provenance. Positions are
explicit approximations. JSON retains all observations; GeoJSON also retains
unmapped observations in collection metadata rather than dropping them.

Palettes use copied display data; temperature readings always use original
uint16 samples with valid scale/offset. Local peak ranking is a replaceable domain
function. Future PSF filters, temporal stacks and detector pipelines should emit
review candidates without changing original temperature measurements or claiming
that enhanced scores are source surface temperatures or ignition probabilities.
