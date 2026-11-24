# Diagnostic BIN logs

The camera writes ArduPilot DataFlash-compatible, self-describing logs to the
microSD card at `/mnt/logs/00000001.BIN` (`/mnt/mmc/logs/00000001.BIN` on A8),
incrementing the number for each session.
`LASTLOG.TXT` records the latest number. Existing BIN files are never overwritten
or automatically deleted. Download them through the web UI **Files → logs**.
A log can be downloaded while it is being written; an interrupted file remains
decodable through its last complete record.

Logging starts when the selected vehicle's component-1 HEARTBEAT reports armed,
including a camera-app restart while already armed. Disarming closes the log.
Losing telemetry is not treated as disarming. Set **Log when disarmed** in the
web UI, or `LOG_DISARMED=1` through MAVLink, to log bench operation as well.
The default is `0`; this setting applies without restarting. Starting/stopping
video recording does not control BIN logging.

Every session begins with FMT definitions and all numeric saved parameters in
PARM records. Runtime camera controls, including zoom and lens, are also logged
as PARM values. PRMA records identify effective configuration values, which can
differ from saved values while a recording format is pending or a transport
setting needs a restart. Configuration writes and accepted parameter changes are logged at application
time. Runtime/vendor controls are also sampled at 10 Hz, and logging policy and
mode transitions are checked on the control loop. Passwords and signing keys
are not logged.

| Record | Contents |
| --- | --- |
| FMT | Message IDs, packed lengths, formats, and field labels |
| MSG | Build identity and tracking/configuration events |
| PARM / PRMA | Saved/live camera parameters / effective configuration |
| TIME | Monotonic `TimeUS` and corresponding UTC microseconds |
| POS | Flight-controller position, altitude, NED velocity and source boot time |
| ATT | Flight-controller attitude; Src 1 is gimbal-state telemetry, Src 2 is ATTITUDE |
| GIMB | Every received gimbal attitude/rate sample and its sample timestamp |
| PIDP / PIDY | Target/actual angle, reported rate, feed-forward, error, P/I/D, output, loop interval and feedback age |
| GCMD | Backend angle/rate requests and quantised wire commands (Mode 1/2); Z1-Mini has no equivalent wire rate and logs NaN there |
| VEND | Received SIYI command opcode, full payload length, and first 31 payload bytes in hexadecimal |
| CMD | Incoming MAVLink commands (Result 255) and their acknowledgements |
| MODE | Vehicle flight mode, armed state, geographic ROI active (Mode 1), tracking method, yaw lock and recording state |
| ROI | Geographic target and active flag |
| CAM | Photo operation outcome, scope, vehicle position and camera attitude |
| VID | Recording transitions, outcome and recording path |
| STAT | Queue depth, cumulative dropped records and storage errors |

Angles use degrees, rates degrees/second, positions degrees, distances metres,
and `DT`/`Age` seconds. Invalid or unavailable measurements are NaN. GIMB rates
retain the camera's reported axis rates; they must not be assumed to be Euler
angle derivatives. ATT Src 2 rates are body rates; Src 1 provides Euler yaw
rate and has no roll/pitch-rate fields. The tracking controller uses feed-forward
plus filtered proportional correction; A8 also uses a bounded integral correction
near the target to overcome its rate dead zone. PID D is currently zero.

A dedicated writer thread uses a bounded approximately 1 MiB queue, flushes data
at least every 250 ms when storage is responsive, and requests a file sync every
2 seconds. All file operations, including directory scanning and sync, run on
that thread. Queue overflow drops complete records and counts the loss. Storage
failure disables that session and is reported in the camera app's text log;
logging retries after 10 seconds while requested. Shutdown drains queued data.
Slow storage can extend the amount of unsynced data beyond two seconds.

Thermal settings are refreshed on a separate background thread. Log sampling
reads a cache and performs no sensor USB transactions. Pipeline reconfiguration
and shutdown join the polling thread before closing its media implementation.

A8 uses `/mnt/mmc/logs` on the microSD card. SITL uses its runtime `mnt/logs`
directory. `CAMERA_APP_LOG_ROOT` can override the
location for isolated tests. Run `make -f tools/dev.mk sitl-live-tracking-test` to check DataFlash
decoding, parameter history, capture events, logging policy, and live settings.
