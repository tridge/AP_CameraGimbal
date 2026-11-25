# Live streams in the video viewer: plan

Add a live source to `tools/viewer`: an APCG raw thermal Matroska stream
(FFV1 gray16, `apcg.thermal.v1` block additions) relayed by SupportProxy,
viewed live with the map path growing, and pause / seek back / click the
path over everything received since the page connected. First deployed at
`https://plot.ardupilot.org/video-live/`, later the standard `/video/`.

Decided with the user: history is browser-side only, from page open (no
SupportProxy change); the stream is addressed by `?live=wss://HOST:PORT/`;
the FireVPS test stream loops and has no viewer password.

## What exists

- Camera (ZT30 / MT11) publishes with HTTP chunked PUT to a SupportProxy video
  port: EBML header, unknown-size Segment, Info, Tracks, then one Cluster per
  frame (1 ms TimestampScale, every frame independently decodable).
- SupportProxy (`videoview.cpp`, `videomkv.h`) serves a viewer the stored
  prefix (EBML header + Segment header + Info + Tracks) and then whole
  Clusters from the newest one, verbatim. Over `wss://HOST:PORT/` (TLS
  WebSocket on the video port, binary messages of arbitrary size, credential
  `?pw=` or `?t=`, no CORS needed) or plain `http://HOST:PORT/vN.mkv`. A
  WebSocket viewer connected before the publisher waits. When the publisher
  leaves, all viewers are dropped. A viewer that falls behind is dropped.
  A TLS viewer with a bad credential is closed after the upgrade.
- Viewer: `LocalSource` (index worker over a `File`) posts `opened`, `batch`
  and `done`; `main.js` appends frames, plays while `!state.indexed` by
  waiting for the next frame, rebuilds the map track every 1.2 s while
  batches arrive. Decoders fetch packets only through
  `source.readPacket(frame)`. `state.file` is used for name / size /
  lastModified, the review storage key and the WebSplat hand-off.

## Design

### `src/media/matroska-live.js` (pure, unit tested)

A push demuxer for the APCG live profile, fed arbitrary byte chunks:

- Frames top-level elements the way SupportProxy's scanner does: EBML header,
  unknown-size Segment (descended), Info, Tracks, then Clusters. Bounds: head
  elements 64 KiB, Cluster 16 MiB (`maxPacketBytes`); anything else is an
  error that ends the connection (reconnect follows).
- Head and Cluster contents are parsed by the existing code in
  `containers.js` (`parseMatroskaTracks`, `blockGroup`, `blockFrame`), run
  over an in-memory blob of the complete element. Those functions are
  exported / lightly refactored; `matroskaFrames` keeps its behaviour and
  tests. Telemetry and thermal metadata therefore match file playback.
- Output: `{tracks}` once per epoch and `{frame, bytes}` per Cluster, where
  `frame.offset` is relative to the Cluster bytes.

### `src/media/live-store.js` (pure over an injected storage, unit tested)

An append-only byte store with random reads, the "File" of a live stream:

- Appends the prefix once, then Clusters. Returns the absolute offset of each
  append; frame packet offsets are absolute in this logical stream, so a
  snapshot of the store is a normal APCG Matroska file with the same offsets.
- 8 MiB segments. The open segment is in memory; a full one is written to an
  OPFS file (`live-streams/<session>/<n>`, sync access handle in the worker,
  then closed and read back through `getFile()`). Reads slice a segment File
  or the in-memory tail.
- Without OPFS (private browsing) segments stay in memory, up to 512 MiB.
- When storage runs out (quota error, or the memory cap) the oldest segments
  are dropped and the store reports the new first available offset. Dropped
  frames stay on the timeline and the map path but are marked
  `decodable = false`; `showFrame` already skips to the next decodable frame.
- A Web Lock per session protects its directory; directories whose lock no
  page holds are deleted on the next start (same scheme as `remote-file.js`).
- `snapshot()` returns the segment Files plus a copy of the tail; the page
  builds `new File(parts, name)` without copying the segments.

### `src/media/live.worker.js` and `src/media/live-source.js`

The worker owns the WebSocket, the demuxer and the store. `LiveSource` is the
page-side adapter with the `LocalSource` surface (`tracks`, `frames`,
`selectedTrackId`, `format`, `readPacket`, `close`) plus `onLive(state)`.

- Connection states: connecting, waiting for the camera, live, reconnecting,
  ended (by the user). Reconnect with backoff 0.5 s to 5 s for ever while the
  session is open. A close straight after open with no data, twice running,
  is reported as "refused: wrong or missing viewer password, or not a video
  port", and the page asks for the password.
- Epochs. Each connection delivers a prefix. If the Tracks are compatible
  with the session's (codec, dimensions, CodecPrivate, timescale) and the
  first cluster timestamp is later than the last one stored, it is the same
  publisher: clusters append unchanged, and the gap in PTS is real. If the
  timestamps restart (camera or publisher restarted; the looping test), a new
  epoch starts: its clusters are stored with the Cluster Timestamp element
  rewritten to `stored + offset`, where the offset continues the timeline by
  the wall-clock gap (at least one frame). Block payloads, BlockAdditions and
  the JSON are never changed; each frame keeps `sourceTimestampTicks` and
  `epoch`. The first epoch is stored byte for byte. Incompatible Tracks end
  the session with an explicit message (open the stream again).
- Batches are posted at most every 100 ms: `{frames, tracks, progress:
{live, bytes, epochs, firstAvailableOffset}}`. Frame ids are the arrival
  index. `readPacket` is a message round trip returning a transferred buffer;
  a read of dropped data rejects.
- No `done` while live. **End live** (button) closes the socket, flushes the
  store, hands the page a snapshot `File`, and the page sets `state.file` to
  it and `state.indexed = true`. From then on it is an ordinary indexed
  recording: Create splat, video information and everything else that waits
  for the index works unchanged. **Save recording** downloads a snapshot at
  any time (live or ended).

### `main.js` and UI

- `openVideo` is split so the per-recording reset and the batch handlers are
  shared; `openLive(url, password)` uses them with a `LiveSource` and a file
  descriptor `{name: live-HOST-PORT-<UTC>.mkv, size, lastModified}`. Opening
  a file, a `?url=` or another live stream closes the live session.
- `?live=wss://HOST:PORT/` connects at load; `#pw=` supplies the viewer
  password and is removed from the address bar once read. A **Live** button
  beside Open video opens a small dialog (URL, optional password). Only
  `wss:` is accepted on an https page (`ws:` on http localhost for tests).
  The password is sent as `?pw=` on the WebSocket URL only, never stored.
- Live edge. A `● LIVE` control beside Play. While following, each batch
  shows the newest frame as soon as the decoder is free (frames that arrive
  while one is decoding are skipped, never queued), and the seek bar stays at
  its end. Pause, a seek, a frame step, a click on the map path or a finding
  stops following; the control then reads `Go live · −m:ss` and returns to
  the edge when pressed. Play from an earlier point uses the existing play
  loop (and Speed); reaching the newest frame resumes following.
- The seek bar's range, duration, frame count and track legend grow with
  each batch, as they do while indexing. Status shows the connection state,
  received size, and the lag of the newest frame's telemetry UTC behind the
  browser clock (labelled as depending on the camera clock).
- Map path. `updateVideoTrack` currently aborts a build when a newer one
  starts, which would starve a long live track. For live: at most one build
  in flight, the next scheduled after it finishes, no more often than every
  1 s or four times the last build's duration. Follow, Project video,
  footprint, pixel picking, 2D / 3D, flight logs, findings, palettes, the
  thermal workbench and video zoom run on the existing per-frame path and
  are unchanged. `poseTimeline.rebuild` per batch is measured on a 20 000
  frame synthetic stream and made incremental only if it costs more than a
  few ms.
- Review storage (findings, offset, rotation) is keyed by live URL (without
  credentials) and session start time.
- CSP `connect-src` gains `wss://firevps.tridgell.net:*`. Other proxy hosts
  are added there when wanted; the dialog explains a CSP refusal as the
  download path does.

### Not in this change

Whole-flight history for a late joiner (needs SupportProxy to serve the
recording so far), signed viewer tokens from the SupportProxy web UI, H.264 /
HEVC MPEG-TS live streams, and live WebSplat hand-off before End live.

## Test stream

- `tools/replay_raw_thermal.py FILE http://HOST:PORT/vN.mkv [--pw] [--loop]
[--speed]`: Python standard library only. Splits an APCG Matroska file into
  prefix and Clusters and publishes them as the camera does (chunked PUT,
  `Expect: 100-continue`), paced by Cluster timestamps. `--loop` closes and
  publishes again from the start, so each pass is a new publisher session.
- FireVPS: a new entry (10094 / 11094 `VideoLiveTest`), one video slot on
  port 40007, recording off, a random publish password, no viewer password. A
  systemd unit (user `fire`) runs the replay of
  `2025-12-18_log80_thermal_orbit_1202-1207UTC.mkv` with `--loop` against
  `127.0.0.1:40007`. No change to the running proxy binary or other entries.

## Tests

- `node --test`: demuxer at every chunk split of a generated stream, bounds
  and malformed input, parity of frames / telemetry with `openMedia` on the
  same bytes; store segmenting, reads across segments, dropping and
  snapshot equality; epoch continuation, restart rebasing and the rewritten
  Cluster header; live URL parsing; track-rebuild scheduling.
- Browser (Playwright, in Xephyr, never the main desktop): a small Node
  WebSocket server in the test plays a generated stream. Checks: frames
  advance while live, the path grows, pause holds, seek back and path click
  show the right frame, Go live returns, a publisher restart continues the
  timeline, End live gives an indexed recording, Save recording reopens in
  the file viewer with identical frame count and pixels.
- Manual qualification against a local SupportProxy build and the FireVPS
  stream: an hour-long run for memory and storage growth, a network drop,
  2D and 3D, Project video.

## Deployment

`deploy.sh` (local, untracked) gains a target argument (`video` default,
`video-live`): base directory, URL and `VIEWER_BASE_PATH`.
`/data/sites/logviewer/video-live/` is seeded from the current `/video/`
release and shared config. `deploy/plot-video-live.nginx` mirrors
`plot-video.nginx` for `/video-live/`; installing it needs root on plot.

## Order of work

1. Demuxer, store and their unit tests; `containers.js` exports.
2. Worker, `LiveSource`, epochs; `replay_raw_thermal.py`; test against a
   local SupportProxy.
3. `main.js` / UI: open, follow, Go live, End live, Save, map scheduling.
4. Browser tests, docs (README, ARCHITECTURE, TESTING).
5. FireVPS entry and replay service; deploy to `/video-live/`; live checks.
6. Reviews (self and codex) of the result, fixes.

## Plan review (codex) and what changed

These override the text above where they differ.

1. No eviction. When storage runs out, capture stops at the last complete
   Cluster with an explicit error; everything received stays seekable and
   saveable. Frames are never marked undecodable for lack of bytes.
2. Segments are immutable once written (the byte count of each write is
   checked). The session directory and its lock outlive End live and are
   released only when another source replaces the session and no WebSplat
   hold is outstanding; pruning happens at the next live start.
3. The page owns the WebSocket and transfers buffers to the worker, so the
   page's CSP governs it and violations are reported on `document`.
4. A complete Cluster is the commit unit: partial input is discarded on
   disconnect. Bounds follow the relay: 128 KiB prefix, 4 MiB Cluster. The
   worker processes input serially; a backlog over 64 MiB closes the
   connection (reconnect resumes at the newest frame).
5. Epochs keep raw ticks, the presentation offset and the last committed
   Cluster separately. A join Cluster identical to the last committed one
   is dropped. Same epoch needs byte-identical Tracks, raw ticks later than
   the last raw ticks, and a raw advance within 10 s of the wall-clock
   advance; otherwise a new epoch starts. Different Tracks end the session.
6. Clusters of later epochs are rewritten before they are parsed, so frame
   `offset` and `extents` come from the stored bytes. The Info Duration
   element is removed from the stored prefix.
7. Following is a mode of the one play loop, not a second presenter: while
   following, the loop shows the newest frame when the last presentation has
   finished instead of the next frame after its duration. Pause, seek and
   step stop it as they stop playback.
8. Live frames arrive in presentation order with id equal to index, so the
   live batch path appends without sorting or id searches. The software
   decoder gets a one-frame group for all-keyframe tracks. Video information
   refreshes are throttled while live. Costs are measured at 90 000 frames.
9. The map keeps the previous pickable track until its replacement is
   built. Track segments break at epoch boundaries and reconnect gaps; pose
   lookup does not cross epochs. Findings record epoch and source ticks.
10. Connection problems are reported as observed (closed before any data,
    never opened, MPEG-TS on this port); entering a password is offered as
    an action, not inferred. MPEG-TS on the port is terminal.
11. End live closes the socket and marks the recording indexed; the
    `LiveSource` stays as the packet source. The review key is the live URL
    without query plus the session start, independent of size. Opening a
    live stream cancels a pending `?url=` download.
