# UniGCS / private SIYI protocol

The shared UniGCS service is built for MT11, A8 mini and ZR10, on hardware
and in SITL. It provides discovery, a private TCP session and camera controls.
It is **not yet full UniGCS feature parity**. The simulator emulates startup
queries, pan/tilt rates and four preset movements. Native mode, rangefinder
and telemetry subscription commands still need decoding and tests.

MT11 forwards private v3 gimbal frames unchanged to its MCU. A8 translates
pan/tilt and preset requests to its native MCU commands, as described below.
For other gimbal requests, A8/ZR10 convert the v3 network frames to their v2 MCU framing and convert replies back to v3,
preserving source/destination IDs, sequence, link, command and payload. Their
MCU payload limit is 255 bytes; longer requests are rejected. Camera replies
use network address 34 for the v3 endpoint queried by UniGCS, independently
of the A8/ZR10 MCU transport's 2C address. Product IDs are MT11: 89, A8: 72,
ZR10: 6B (hexadecimal; the ZR10 identity still needs vendor confirmation).
The network service accepts v3 framing and the older `55 66 AA BB` format
selected by UniGCS for A8. Each TCP session keeps the format of its first
valid request, and replies and heartbeats use that format. MCU UART parsers
do not accept the older network framing.

The A8 scan capture shows UniGCS querying `34/F0/01` and ignoring our initial
`2C/F0/01` reply with product 73. Discovery was confirmed in UniGCS after
changing the source to 34 and the camera product ID to 72.
Stock A8 `cardv` v0.3.7's `camera_sdk_get_ver_action` at 0x62370 returns
`07 03 00 72`, confirming that 72 is the camera's version product ID; 73 is
the gimbal identity.
The inspected `cardv` SHA256 is
`5c33c92acf12b2145327f6a86082110c5b9c4fcdf0ac52a2a29a419580737f50`.

Real A8 testing also found an Ethernet receive-filter issue: unicast discovery
worked, but multicast discovery failed despite the correct `224.0.0.1`
membership and `01:00:5e:00:00:01` MAC entry. Enabling `ALLMULTI` immediately
restored multicast replies. Startup scripts for all three SIYI cameras now
enable it on `eth0` so discovery does not depend on vendor multicast filters.
The receive-filter failure and fix are hardware-verified on A8; MT11 and
ZR10 still need that hardware check. SITL does not change host interface flags.
The service retries multicast membership every five seconds to handle interfaces
that become ready after startup or are recreated. Discovery uses the interface
MAC on Linux, Cygwin and BSD; SITL adapters without one use a stable locally
administered address derived from their IPv4 address.

MT11 and A8 SITL H.265 display have been confirmed in the UniGCS GUI.
Discovery, connection and H.265 display are also confirmed on the real A8
after enabling multicast reception. All three models have isolated
protocol/video tests; ZR10 GUI interoperability and the remaining hardware
controls still require validation. Their shared camera handlers use the
MT11 capture as a starting point, not an assertion that all vendor command
layouts match.

The older network header consists of four magic bytes, flags (request 1,
reply 2), a 32-bit little-endian payload length, 16-bit sequence, command
byte, and a 32-bit header checksum. Payload starts at offset 16 and is
followed by a second 32-bit checksum. Both checksums use polynomial 04C11DB7,
initial zero, MSB first, no final XOR, stored little-endian. The first covers
bytes 0–11; the second covers the complete header and payload. Captured A8
status request: `5566aabb01000000000000802d977a34b7ad40eb`.
Old-format pan/tilt 9A and preset 9B enter the gimbal handler; on A8 it sends
camera-originated `6B/06` for rates, `6B/07` for yaw centring, and explicit
SDK angle targets for presets involving pitch. Other decoded camera
commands share the camera handlers. Unimplemented A8
version/configuration and image-control variations are logged, not treated
as implemented merely because their frame checksum is valid.

### Android A8 camera controls

UniGCS Android 3.2.1 selects the legacy command namespace for A8. Its photo
button sends `9F/00`, not the MT11 `C6/01` request. The APK's `E1` serializer
selects these by camera model. Stock A8 `cardv`'s
`camera_sdk_gimbal_camera_mode_action` at `0x6339c` replies with the camera
mode byte (`00` for photo), not C6's boolean success byte. The legacy handler
now captures through the shared media backend and replies `9F/00` only when
capture succeeds. It rejects other modes, malformed payloads and v3 `9F`
requests; those are not established photo aliases.

Hardware testing with the Android tablet on 2026-09-28 found three separate
causes for controls appearing ineffective:

- Photo: `9F/00` was unhandled. With the handler installed, the Android button
  produced a 270568-byte JPEG on the A8 card.
- Gimbal: Android sent `9A/0C00` followed by `9A/0000`, but the camera still
  ran build `31d096`, which predates the A8 MCU translation in `5720d8c`.
  Installing the current application made the same gesture move measured yaw
  from 1.7 to 6.8 degrees, settling at 6.7 after release.
- Recording: `81/01` was already supported and returned `81/0100` (failure)
  while the card was full. Once space was available, the Android buttons
  returned `81/0101` for start and `81/0001` for stop and created an MP4.

The legacy protocol tests replay these controls and verify image creation,
storage failure, no-ACK and malformed requests, recording state, and actual
simulated A8 motion/stop in both mounting orientations. A movement ACK alone
does not establish that the MCU moved; likewise a video stream does not
establish that the camera control session is connected.

## Running and testing

Build with `make sitl`, `make a8_sitl` or `make zr10_sitl`, then launch the
selected model as the first SITL instance.
UniGCS must discover the **SITL host's LAN address**, not the physical stock
camera at 192.168.144.25. Allow UDP 37258, TCP 37256 and TCP 8554 through the
host firewall. Video uses the existing `/video1` RTSP service. For UniGCS,
select H.265 for the main stream (`codec = h265` in `[stream.main]`). This is
the configuration confirmed to display video in the tested UniGCS client.
The server also supports H.264, but that client remained blank with H.264
even after the RTCP fix; the cause is not yet established.

Run `python3 sitl/test_unigcs.py build/sitl/camera-app` for an isolated test of
wire framing, discovery, startup, gimbal routing, reconnects, camera operations,
persistence and coexistence with the public SDK. It starts its own simulated
MCU and camera on temporary ports and does not access the physical camera.
Use `--backend a8` with `build/a8-sitl/camera-app`, or `--backend zr10` with
`build/zr10-sitl/camera-app`, for the other SIYI cameras. These tests also run
in CI and under the corresponding `sitl-test`, `a8_sitl-test` and
`zr10_sitl-test` make targets.
Add `--video-codec h264` or `--video-codec h265` to include live video tests.
These validate decoder parameters in SDP before PLAY, RTSP aliases, replies
on two local addresses, FFmpeg decoding, and initial and periodic RTCP sender
reports against the received RTP timestamps, SSRC and counters. CI runs both
codecs. The encoder is initially paused to check that DESCRIBE and PLAY succeed
before any parameter sets exist, and that the first frame carries those sets.
Run `sudo unshare -n python3 sitl/test_unigcs.py build/a8-sitl/camera-app --backend a8 --late-interface`
to check discovery on an interface created after startup and then replaced.
This test creates interfaces only inside the isolated network namespace.

The protocol test also restarts the UDP gimbal simulator while the camera is
running and verifies that native commands work again after it returns. SITL
consumes transient UDP port-unreachable errors so a simulator restart does
not terminate the camera and video services.

RTSP descriptions include H.264 SPS/PPS and packetization mode 1, or H.265
VPS/SPS/PPS, taken from the encoder. Parameter sets are collected even without
viewers, and aliases share them. SDP is regenerated for each DESCRIBE, avoiding
stale codec headers or an address cached from another interface.
Before the encoder supplies its first parameter sets, DESCRIBE returns the
basic codec description and clients obtain headers from the stream. Later
DESCRIBEs include the complete decoder configuration.

The server sends compound RTCP sender reports and SDES/CNAME on the negotiated
TCP channel or UDP port, initially and every five seconds while streaming.
Reports pair the wall clock with the transmitted RTP timestamp and count the
packets and payload bytes sent to that client. The stock camera also sends
these reports. UniGCS video display was confirmed with H.265 after adding
the reports; the same stream had remained blank without them. Automated
decoding and transport checks pass for both H.264 and H.265. The subsequent
H.264 test still produced no picture in UniGCS despite correct H.264 codec
advertisement in both private command `16/83` and SDP. H.265 was restored for
continued testing. This does not establish full control compatibility or
hardware parity.

The private service is enabled for all three SIYI models using the standard
SDK port 37260.
Other SDK ports leave it disabled, avoiding collisions between cameras on the
same SITL host. `CAMERA_APP_UNIGCS_PORT` explicitly enables/overrides the TCP
listener; `CAMERA_APP_DISCOVERY_PORT` overrides UDP discovery. These overrides
are useful for isolated tests; UniGCS itself expects the standard ports.
Only one private TCP client is accepted at a time: MCU replies do not echo
request sequence numbers, so clients sharing source D0 cannot be safely
multiplexed. Five seconds without a valid request releases the session.

If UDP 37258 is occupied, the application leaves discovery to the existing
owner and logs this. On physical stock-derived MT11 systems, `product_upgrade`
usually owns that socket. Our application still provides TCP 37256. Kernel,
updater and gimbal firmware are not replaced by this change.
TCP 37256 must be available: a bind failure stops camera-app with an error,
as it does for the public SIYI listener. This prevents a competing process from
silently owning private controls while our application serves the camera.

## Time synchronisation

Stock firmware requests time from its connected TCP client using command `91`
with one-byte payload `01`, while its time-initialised flag is clear. On MT11
this is camera link `16`. The misleadingly named `get_utc_time_info_action`
receives calendar time and sets the system clock; it is not a query of the
camera's current time. Our private service requests time once per second after
the first valid client command establishes its routing ID and wire format,
while the camera date is earlier than 1 September 2026 UTC. It supports both
MT11 v3 and A8/ZR10 legacy TCP framing. Replies must come from the same client
and address the camera's time command while a request is outstanding.

The service accepts packed seven-byte or padded eight-byte calendar payloads,
with response, request or no-ACK control flags, without replying to the time
reply. UniGCS supplies local wall time, so the camera's configured timezone
must match the GCS timezone. The service converts it with `mktime`, matching
stock firmware, and lets libc determine daylight saving. MAVLink `SYSTEM_TIME`
and public SDK `30` still supply epoch timestamps independent of timezone.
Invalid dates, dates before the validity threshold, years after
2099 and dates the platform's `time_t` cannot represent are rejected. Clock
setting errors are retried, but only the first failure is logged until a
supported source establishes a valid clock. Reconnecting does not reset this
suppression. Failed clock updates do not refresh the five-second client idle
timeout. Successful updates log the effective timezone and UTC offset.
Once the clock is valid, including if
MAVLink or public SDK `30` sets it first, private requests stop and delayed
replies cannot step the clock. Existing files are not renamed or retimestamped.

The seven-byte calendar reply has no UTC offset or DST indicator. During the
repeated local hour when daylight saving ends, either occurrence can pass the
round-trip check; libc chooses one, and the result can be an hour wrong.
Nonexistent local times during the spring-forward gap are rejected. Correctly
configured clients never emit those nonexistent times, but differing client
and camera timezone rules can cause rejection. Use matching timezone settings.
MAVLink `SYSTEM_TIME` only initializes an invalid clock; it does **not** correct
a clock that UniGCS has already set to a valid but wrong epoch. Public SDK `30`
can explicitly set the epoch. The validity cutoff is 1 September 2026 **UTC**,
regardless of the timezone used in the calendar reply.

This is confirmed by stock executable analysis: MT11 V1.0.5
`get_utc_time_ontick` at `0x5f21f0` sends the request; handler `0x63d680` copies
the received data and calls `update_utc_time` at `0x5f2100`, which calls
`mktime` and `settimeofday`. A8 `cardv` v0.3.7 has the same sequence at
`0x52408` and `0x522b4`; its TCP dispatch table at `0xe39d0` maps command `91`
to wrapper `0x65575`. Calendar fields are little-endian uint16 year followed
by month, day, hour, minute and second bytes. A8 copies eight payload bytes,
but uses only the first seven for the calendar. The eighth byte is ignored.
An A8 test on 2026-09-27 confirmed the local-time interpretation: UniGCS
`.69` supplied `06:43:50` with the camera configured as `GMT-10` (UTC+10).
Treating that value as UTC set epoch `1790491430` and displayed `16:43:50`,
ten hours ahead. Local-time conversion instead yields epoch `1790455430` and
displays the original `06:43:50`. The vendor handler's UTC name is misleading.

Timezone investigation used the actual A8 log `00000519.BIN`: outgoing
`SIOU` Id 1223 requests time, and incoming `SIIN` Id 1224 at boot time
118.793202 seconds contains this complete legacy frame from `192.168.144.69`:

```text
55 66 aa bb 00 07 00 00 00 02 00 91 ee d1 df cc
ea 07 09 1b 06 2b 32 bb a9 3a 56
```

The declared payload length is **seven**, and the payload is
`ea 07 09 1b 06 2b 32`: year 2026, month 9, day 27, hour 6, minute 43,
second 50. The last four bytes are the frame checksum, not time metadata.
There is no UTC offset or DST indicator in this reply. The optional eighth
byte accepted for stock struct compatibility is not used by either stock
calendar conversion; its meaning in other client versions is unverified.
The regression test replays this exact frame, including its checksums.

Other possible timezone sources were checked separately:

- MT11 private `16/f0` feeds `update_local` at `0x5f2480`, storing a
  two-character uppercase `local:geo_code` and a three-character lowercase
  `local:alpha_code`. These are locale codes, not an explicit timezone or
  numeric UTC offset, and this handler does not set `TZ`.
- The stock A8 web CGI has a separate `setTimeZone` function which writes
  `timezone <value>` to `/tmp/cardv_fifo` and saves `Camera.Menu.TimeZone`.
  The executable also contains a `setenv("TZ", ...); tzset()` path.
  This establishes a separate web configuration facility, not evidence of
  a private UniGCS timezone message.
- The A8 log contains no incoming `f0` locale reply or identified timezone
  command. The inspected MT11 private dispatch table has no named timezone
  setter. Public SDK `30` is documented as Unix epoch microseconds, with no
  separate timezone field.

These findings establish the observed exchange, not every possible firmware
or UniGCS version. Do not infer a timezone from a country/language code:
countries can span multiple timezones. Configure the camera timezone to match
the client when using this local-calendar time source.

Run `make -C camera_app unigcs-time-test` to check the production dispatcher,
handshake, gimbal routing, malformed dates, the 2099 year limit, leap years,
timezone offsets, DST gaps/repeated hours, retry recovery, bounded failure
logging, idle disconnection and existing clock protection without altering
the host clock.

The stock MT11 capture in `analysis/MT11/unigcs-20260925/stock-camera.pcap0`
does not contain that private exchange: packet 1691 already sets its clock
through public SDK UDP 37260, command `30`, from `192.168.144.15:35136`.
Its payload `81f79408445c0600` is epoch microseconds `1790297131775873`
(`2026-09-25 00:45:31.775873 UTC`). Packet 1692 contains the success reply
`30/01` and has corrected capture time `2026-09-25 00:45:31.001678 UTC`.
UniGCS on `.69` connects about two minutes later, after time was initialised.
Our A8/MT11 firmware also supports public SDK `30` and MAVLink `SYSTEM_TIME`;
the private request/response provides a clock source when using UniGCS alone.

## Confirmed transport

Captured using stock MT11 V1.0.5 on 2026-09-25:

- UniGCS `.69:37258` sends discovery to `224.0.0.1:37258`.
- Camera `.25:37258` replies unicast with a 19-byte identity payload:
  device ID 0x89; IPv4; netmask; broadcast; six-byte MAC address.
- UniGCS connects to camera TCP 37256 using private v3 framing.
- Client ID is D0, camera ID 34, gimbal ID 2E. Camera link is 16, gimbal
  link 11, discovery link F0. Numbers here are hexadecimal.
- The camera emits `16/F0` payload `01` once per second. UniGCS polls `16/80`
  recording status about once per second, also keeping the stock session alive.
- A bounded UART trace with a read-only `11/A0` request confirmed that the
  stock app forwards the entire private network frame unchanged to UART3,
  and forwards the MCU's reply unchanged back to TCP.

```
AA | control | 03 | payload_length_u16_le | CRC8 |
sequence_u16_le | source | destination | link | command |
payload | CRC16_u16_le
```

CRC8/MAXIM covers the first five bytes; CRC16/CCITT with initial zero covers
all bytes before the final checksum. Requests observed use control 09 (ACK
requested), and responses use 0A. Control 08 requests are also accepted.
The TCP parser handles fragmentation, concatenation and corrupt-frame recovery.
A bad header CRC is rejected before waiting for its advertised payload length.
Network/private UART frames are included in the existing packet BIN logging,
including source and destination IDs in their original wire headers.

A8 and ZR10 report a single RGB sensor and do not expose thermal controls or
an MT11 second lens. Zoom limits come from each target's supported control
range. MT11/A8 use stepped absolute zoom; ZR10 uses its native rate control,
with an explicit stop on release or disconnect and asynchronous MCU feedback.
Image-setting writes fail on targets without image-control support (ZR10).

## Native gimbal controls in SITL

UniGCS link `11`, destination `2E` (or commands in the older A8 network format):

| Command | Payload | Behaviour |
| --- | --- | --- |
| `9A` | Signed yaw and pitch bytes, each -100 to 100 | Same rate control as public SDK `07`; zero stops motion |
| `9B` | `01` | Centre yaw and pitch |
| `9B` | `02` | Centre yaw and look down |
| `9B` | `03` | Centre yaw, preserving pitch |
| `9B` | `04` | Look down, preserving yaw |

Valid motion requests return one-byte `01` when an acknowledgement is
requested. Invalid preset IDs return `00`; malformed lengths are ignored.
Control `08` applies the command without an acknowledgement. Motion shares
the simulator's public SDK state and motor response. Presets target a physical
downward pitch of -90 degrees, clamped to the simulated gimbal's limits.

The UniGCS capture on 2026-09-25 contains all four `9B` values. The stock
MCU image (`5c78d9cc854ba9ff6f49697c761367cc8d447976f451fe727ed408d306c5b038`)
dispatch table at file offset `0x3BAA0` maps `9A`/`9B` to wrappers at
`0x80037A04`/`0x80037A24`, then handlers at `0x80039810`/`0x80039994`.
The former shares the public SDK rate handler; the latter calls ILM routines
`0x6D94`, `0x6DB0`, `0x6E04`, `0x6E1C` for the four presets. The startup
copy maps file `0x3CE10` to ILM `0xEE0`. These establish payload semantics
independently of the simulator tests. MT11 commands are forwarded to the MCU
unchanged; its command namespace must not be assumed to apply to A8.

### A8 motion translation

Stock A8 `cardv` v0.3.7 handles pan/tilt at
`camera_sdk_gimbal_turn_action` (`0x63268`) and presets at
`camera_sdk_gimbal_one_key_action` (`0x632A0`). Its worker
`gimbal_action_ontoick` (`0x64AA0`) sends their payloads unchanged as
`6B/06` (two signed rate bytes) and `6B/07` (one preset byte), from camera
CPU `2C` to gimbal `2E`. It acknowledges the network request locally.
Passing through `D0 -> 2E, 11/9B` on the real A8 produced no MCU reply or
movement, despite working in the earlier simulator.

On the tested inverted A8, native rates and yaw-only preset 3 worked, but
native presets 1, 2 and 4 did not move pitch. Those presets therefore use
the existing mounting-aware SDK `0E` angle path: level or -90-degree pitch,
with yaw zero for presets 1/2 and current yaw for preset 4. Preset 4 requires
attitude feedback no older than one second. Yaw-only centring retains native
`6B/07`; rates use `6B/06`.

UniGCS testing on the physical inverted A8 confirmed all four presets and
hold-and-drag motion after this change. Its logged pitch settled near -25
degrees for a -90-degree look-down request, then returned to level for
preset 1. Thus the controls work, but full downward travel in inverted
mounting has not been demonstrated; the simulated limits do not establish
the real MCU's available travel.

The A8 backend supports both network formats, validates rates (-100 to 100)
and presets (1 to 4), and acknowledges successful dispatch locally when
requested. This acknowledgement confirms acceptance, not completion of the
movement. Invalid preset IDs return zero without moving the gimbal; malformed
requests are rejected. The A8 simulator rejects untranslated `11/9A,9B`
commands and reproduces the inverted native-preset limitation. Tests verify
all four resulting positions in upright and inverted configurations, not just
receipt of an acknowledgement. ZR10's private preset translation still
requires separate hardware validation.

## Camera command coverage

Settings use existing media/backend operations. Image controls, palette,
and automatic recording persist through the shared parameter
metadata/INI machinery. Other live controls retain their existing persistence
semantics. Unsupported operations are logged once per command and are not
acknowledged as successful. Region-tracking availability follows the raw-frame
path and worker. The startup
catalogue query returns zero classes and an empty name list, so a client can
complete the query without being told that stock tracking models are present.

`16/94` reports the AP release version from the build's `MT11_VERSION`, using
the same identity source as the web Status page and BIN log. For example,
`v1.0.1` produces patch/minor/major bytes `01 00 01`, followed by the unchanged
camera-model ID. Two-part tags such as `v1.0` use patch zero. Unknown versions
or components outside 0–255 report `0.0.0` rather than a stock version or a
wrapped value. Build through the top-level Makefile to select the release tag
automatically, or provide `MT11_VERSION` when building camera_app directly.

UniGCS constructs its version label from these three bytes, so a prefix such
as `AP-1.0.1` requires a UniGCS change; this reply has no version-string field.
The model byte and optional AI capability/version bytes retain their existing
meanings. Gimbal/MCU version requests remain separate and are relayed unchanged.
`16/E1` preserves the stock
V1.0.5 layout, including its duplicate ISO byte. Encoder queries report current
sizes/codecs/frame rates and the MT11 encoder's nominal bitrate policy; the
SITL renderer does not use that hardware bitrate policy. Encoder *setters* are
not enabled yet. MT11 supports RGB, thermal and side-by-side composition;
independent secondary sensor selection is not exposed.

The table below comes from the stock executable dispatch table, not guessed
opcode correspondence with the public SDK. Stock SHA-256:
`bf6a0440e83c9ab3cd666474d3cc37afbda0140073699849db73b1b05e8f4fa3`.
`get_mini4_tcp_server_camera_action_func` at 0x638130 searches 73 ten-byte
entries at 0x19db1d0. Each entry maps link/command to a wrapper calling the
named handler. These mappings alone do not establish complete payload layouts.

| Link/command | Stock handler suffix | Implementation |
| --- | --- | --- |
| `f0/3b` | `reset_dev_param_action` | Not implemented |
| `f0/07` | `shell_action` | Not implemented |
| `16/f0` | `get_local_info_action` | Heartbeat; client location payload accepted but not used |
| `16/94` | `get_camera_ver_action` | AP release version / camera-model identification |
| `16/91` | `get_utc_time_info_action` | Receives validated local calendar time following camera's `91/01` request |
| `16/74` | `get_ip_action` | Pending payload/backend implementation |
| `16/75` | `set_ip_action` | Pending payload/backend implementation |
| `16/eb` | `zoom_switch_mode_action` | Pending payload/backend implementation |
| `16/ec` | `zoom_control_action` | Pending payload/backend implementation |
| `16/ef` | `zoom_focus_test_action` | Pending payload/backend implementation |
| `16/b2` | `dehaze_type_action` | Pending payload/backend implementation |
| `16/e1` | `get_isp_all_param_action` | Image settings query |
| `16/e3` | `set_isp_one_param_action` | Validated, persistent image parameter setting |
| `16/e5` | `set_get_isp_mode_action` | Exposure mode query only |
| `16/83` | `get_encode_param_action` | Encoder query: 0 recording, 1 main RTSP, 2 secondary RTSP |
| `16/84` | `set_encode_param_action` | Pending payload/backend implementation |
| `16/92` | `get_vdieo_splice_action` | Image selection query |
| `16/93` | `set_vdieo_splice_action` | Single RGB/thermal selection; no picture-in-picture |
| `16/ca` | `dis_stat_action` | Pending payload/backend implementation |
| `16/c9` | `hnr_stat_action` | Pending payload/backend implementation |
| `16/a7` | `set_ircut_mode_action` | Pending payload/backend implementation |
| `16/a8` | `get_ircut_mode_action` | Pending payload/backend implementation |
| `16/9c` | `night_led_stat_action` | Pending payload/backend implementation |
| `16/c8` | `night_frame_rate` | Pending payload/backend implementation |
| `16/85` | `get_auto_record_sta_action` | Automatic recording query |
| `16/86` | `set_auto_record_sta_action` | Automatic recording setting (reply is 85) |
| `16/80` | `get_sd_record_status_action` | Recording status and elapsed seconds |
| `16/81` | `set_sd_record_action` | Start/stop recording |
| `16/82` | `sd_format_action` | Pending payload/backend implementation |
| `16/c6` | `capture_action` | Photo capture; accepts UniGCS payload 01 and empty requests |
| `16/fe` | `advance_capture_action` | Pending payload/backend implementation |
| `16/b3` | `set_capture_screen` | Pending per-sensor capture enable support; two-byte sensor/enable request and B3 echo |
| `16/b4` | `get_capture_screen` | Photo scope query |
| `16/c2` | `set_record_screen` | Pending payload/backend implementation |
| `16/c3` | `get_record_screen` | Pending payload/backend implementation |
| `16/b1` | `capture_resolution` | Pending payload/backend implementation |
| `16/ed` | `get_sd_information_action` | Pending payload/backend implementation |
| `16/bd` | `set_thermal_gain_mode_action` | Thermal gain selection |
| `16/bc` | `get_thermal_gain_mode_action` | Thermal gain query |
| `16/8a` | `get_thermal_point_info_action` | Pending payload/backend implementation |
| `16/8b` | `get_thermal_rect_temp_action` | Pending payload/backend implementation |
| `16/a4` | `get_thermal_pseudo_color_action` | Thermal palette query |
| `16/a5` | `set_thermal_pseudo_color_action` | Persistent thermal palette selection |
| `16/cf` | `set_thermal_shutter_action` | Pending payload/backend implementation |
| `16/cb` | `get_thermal_zoom_mode_action` | Pending payload/backend implementation |
| `16/cc` | `set_thermal_zoom_mode_action` | Pending payload/backend implementation |
| `16/be` | `get_thermal_envcorrect_action` | Pending payload/backend implementation |
| `16/bf` | `set_thermal_envcorrect_action` | Pending payload/backend implementation |
| `16/e9` | `get_thermal_envcorrect_param_action` | Pending payload/backend implementation |
| `16/ea` | `set_thermal_envcorrect_param_action` | Pending payload/backend implementation |
| `16/8d` | `set_thermal_temp_frame_action` | Pending payload/backend implementation |
| `16/8c` | `get_thermal_temp_frame_action` | Pending payload/backend implementation |
| `16/e6` | `start_thermal_calibration_action` | Pending payload/backend implementation |
| `16/e7` | `get_thermal_calib_param_action` | Pending payload/backend implementation |
| `16/e8` | `set_thermal_calib_param_action` | Pending payload/backend implementation |
| `16/a0` | `get_set_ir_sr_state_action` | Pending payload/backend implementation |
| `16/a2` | `get_ai_target_tracking_switch_action` | Reports AI tracking unavailable/off |
| `16/a3` | `set_ai_target_tracking_switch_action` | Pending payload/backend implementation |
| `16/aa` | `select_ai_target_tarcking_action` | Pending payload/backend implementation |
| `16/ab` | `ai_detect_data_action` | Pending payload/backend implementation |
| `16/ac` | `cancel_ai_target_tracking_action` | Pending payload/backend implementation |
| `16/ad` | `set_get_ai_threshold_action` | Pending payload/backend implementation |
| `16/a9` | `get_set_ai_model_id_action` | Pending payload/backend implementation |
| `16/af` | `get_set_plate_rec_mode_action` | Pending payload/backend implementation |
| `16/ae` | `load_or_unload_model_action` | Pending payload/backend implementation |
| `16/d5` | `get_set_object_count_or_mask_action` | Query 03 returns an empty AI catalogue; setters pending |
| `16/f1` | `ai_model_manage_action` | Pending payload/backend implementation |
| `16/98` | `zoom_action` | Continuous zoom, stopped by zero or disconnect |
| `16/97` | `auto_focus_action` | Point autofocus (1920x1080 coordinates) |
| `16/99` | `manual_focus_action` | Manual focus, stopped by zero or disconnect |
| `16/d0` | `get_zoom_range_action` | Supported visible zoom range (1–10x) |
| `16/d1` | `get_zoom_value_action` | Visible zoom query |
| `16/d2` | `set_zoom_value_action` | Absolute visible zoom |

## Remaining work before full GUI acceptance

- Decode remaining native link-11 modes and telemetry subscriptions;
  add stateful simulator handling with captured command/reply fixtures.
- Verify the complete UniGCS GUI startup and exercise each implemented control.
- Implement encoder setters, remaining image/composition controls, temperature
  point/rectangle requests, thermal shutter/calibration and storage operations.
- Object classification and model management remain unsupported; rectangle
  tracking uses dlib without a classifier or pretrained weights.
- Capture media browsing/download, network configuration and any additional
  services used by those GUI screens. The initial session only exercised
  discovery, private control and RTSP.

The stock MT11 remains available for read-only queries and controlled captures.
Testing the new application on hardware comes after the initial SITL GUI pass.

## Rectangle tracking, scenes and range

The shared [image tracker](image-tracking.md) supports UniGCS region selection
and MAVLink rectangle commands. Camera-version query `16/94` appends an AI
version and type `87` when the frame path is available. This exposes region
tracking on A8/ZR10 without claiming the stock object-classifier features.
MT11's model ID already enables its tracking UI; it receives the same extension.

`16/A2` queries enable state; `16/A3 01` enables selection (`01 00` reply).
`16/AA` takes enable plus four little-endian 16-bit encoder-pixel coordinates.
Both drag directions are accepted. Its reply acknowledges a queued selection;
`16/AC 00` is emitted only after image acquisition, `AC 01` on loss/cancel.
`AA` with nine zero bytes cancels either protocol's target. A disconnect stops
only a SIYI-owned target. Live video contains the rectangle overlay.

MT11 `16/93` selects visible (`00 00`), thermal (`02 00`) or side-by-side
(`03 00`); replies/`16/92` state are `00 02`, `02 00`, `03 02` respectively.
The visible scene remains `00` (Zoom) when the MT11 uses its wide RGB sensor
at low zoom. Scene `01` (Wide) is a different UniGCS UI and hides the target
button; the physical RGB sensor must not determine the reported scene.
The native combined view uses the SDK scaler with aspect-preserving padding.
Tracking spanning two sensor images is rejected in combined mode.

MT11 measure commands remain a transparent gimbal-MCU service: `11/BA`
queries state/type, `11/BB 01/00` enables/disables ranging; subscribed `11/89`
distance and `11/B0` coordinate notifications return to the requesting client.
SITL implements that exchange, including stopping periodic updates when disabled.
A8/ZR10 do not acquire nonexistent thermal sensors or lidar capabilities.
