# MT11 rate response and calibration

## Travel and settling measurements, 2026-10-02

Bench-tested the upright MT11 at `192.168.144.25` using raw SIYI commands through
the running camera application. There was no flight-controller heartbeat during
the initial connection check. Geographic tracking was temporarily disabled;
the original parameters and starting pose (approximately yaw +2.4, pitch -30
degrees) were restored afterwards. Existing recording remained active. These
measurements used the installed firmware, without deploying a new build.

The **commandable pitch range is -90 to +25 degrees** on this unit. An angle
request for -95 stopped at -90; a request for +30 stopped at +25. Rate commands
also stopped near those boundaries. This establishes the MCU command limits,
not the mechanical travel of a disassembled gimbal. The target profile currently
specifies an upper limit of +30, which differs from this measured unit.

In particular, looking behind the aircraft requires yaw rotation: the gimbal
cannot continue pitching through nadir. A pitch-only transition from a forward
view at -60 to a rearward view at -120 is unavailable through either tested
command interface.

| Raw command magnitude | Pitch speed, deg/s | Yaw speed, deg/s |
| --- | --- | --- |
| 1, 3, 4, 5 | No sustained motion | No sustained motion |
| 6 | +6.01 / -6.02 | +6.32 / -6.24 |
| 10 | +10.13 / -10.12 | +10.25 / -10.27 |
| 20 | +20.11 / -20.12 | +20.90 / -20.24 |
| 40 | +40.08 / -40.12 | +40.35 / -40.45 |
| 60 | +59.79 / -60.56 | +59.05 / -60.35 |
| 80 | +79.83 / -80.89 | +80.37 / -80.25 |
| 100 | +98.12 / -103.76 | +102.27 / -97.49 |

The sweeps reconfirm approximately 6 deg/s minimum sustained speed and 100 deg/s
full speed. High-speed fits contain only 5–12 samples because each trial limits
travel to approximately 25 degrees; the directional differences should not be
treated as precise calibration changes. Speeds come from unwrapped Euler angle
feedback, not body gyro components.

Sending zero at full speed produced approximately **4.4 degrees of additional
pitch travel and 4.8 degrees of yaw travel**. At command 60, the corresponding
travel was 2.6 degrees pitch and 2.8–3.1 degrees yaw. Full-speed stops entered a
±0.5-degree final-angle band in 0.07–0.10 seconds for pitch and 0.07–0.17 seconds
for yaw. These are feedback-based measurements, including transport delay.

Two repetitions of absolute-angle sequences gave the following command-to-settle
times. Here, settling means entering a **±0.5-degree band around the final angle**,
remaining there through the end of the four-second observation, and having a
fitted slope of at most 1 deg/s during the next 0.4 seconds. Times refer to entry
into that interval; an online detector would need the additional confirmation
window. This is positional settling, not proof of smear-free thermal exposures.

| Axis | Actual step | Settling time |
| --- | --- | --- |
| Pitch | 5 degrees | 0.18–0.19 s |
| Pitch | 10 degrees | 0.24–0.25 s |
| Pitch | 15 degrees | 0.31 s |
| Pitch | 30 degrees | 0.44–0.47 s |
| Yaw | 5 degrees | 0.25 s |
| Yaw | 10 degrees | 0.31–0.49 s |
| Yaw | 30 degrees | 0.72–0.88 s |
| Yaw | 60 degrees | 1.07–1.40 s |
| Yaw | 90 degrees | 1.03–1.15 s |
| Yaw | 120 degrees | 1.40–1.51 s |
| Yaw | 150 degrees | 1.63–1.84 s |
| Yaw | 180 degrees | 2.08–2.18 s |

Yaw feedback has several tenths of a degree of residual variation. Tightening
the band to ±0.3 degrees made the measured times much less consistent: even
some small steps required over three seconds, and one 45-degree step failed
the strict criterion within the observation window. Do not interpret those
times as the duration of the main slew, or assume the reported variation is
entirely physical motion without an independent optical measurement.

The raw absolute-angle command uses the MCU's own controller. Survey geographic
tracking uses the application's rate controller, which was capped at **60 deg/s**
when these measurements were taken. The cap has since been raised to **100 deg/s**
in the shared MT11 firmware/SITL profile to match the measured capability.
The approximately 2.1-second raw 180-degree move is still not a measured survey
transition timing: application acquisition and settling affect that timing.
The 6 deg/s minimum sustained speed
also remains relevant to tracking a ground point at survey height; these static
tests do not validate moving-aircraft tracking or exposure sharpness.

Feedback was quantised to 0.1 degree, sampled approximately every 30 ms, with
median request/response time 12 ms and 95th percentile 17 ms. Capture artifacts
are in `/data/review/mt11-motion-20261002/`: raw CSVs and trial metadata,
`rate_metrics.csv`, `step_metrics.csv`, the reproducible `analyze.py`, PNG plots,
and `mt11-motion-report.pdf`. The analyzer uses NumPy, pandas and Matplotlib.

To repeat the stationary upright tests, disconnect other gimbal controllers and
run each mode sequentially:

```sh
python3 tools/mt11_motion_bench.py --host 192.168.144.25 --mode travel --output travel.csv
python3 tools/mt11_motion_bench.py --host 192.168.144.25 --mode rates --output rates.csv
python3 tools/mt11_motion_bench.py --host 192.168.144.25 --mode steps --output steps.csv
```

## Isolated upright bench sweeps, 2026-09-15

The raw SIYI rate command is a signed integer from -100 to +100. Tests on the
stationary MT11 measured both directions, first near the low-speed threshold,
then through full command. Each sample started near yaw zero, pitch -45 degrees.
The threshold region was repeated in descending magnitude order at -20 degrees
pitch. The MCU reported mounting direction 1 (upright).

| Raw command magnitude | Pitch speed, deg/s | Yaw speed, deg/s |
| --- | --- | --- |
| 1 through 5 | Approximately zero | Approximately zero |
| 6 | +6.05 / -5.96 | +5.42 / -6.42 |
| 7 | +7.04 / -7.03 | +6.64 / -6.77 |
| 10 | +10.12 / -10.09 | +9.47 / -10.11 |
| 20 | +20.07 / -20.05 | +19.51 / -20.20 |
| 40 | +39.96 / -40.19 | +40.91 / -39.71 |
| 60 | +60.68 / -60.47 | +61.28 / -58.83 |
| 80 | +79.98 / -79.95 | +82.84 / -78.21 |
| 100 | +99.87 / -100.33 | +103.50 / -97.64 |

Motion was measured from angle changes, not reported gyro rates. Those gyro
axes are not Euler yaw/pitch derivatives, especially when looking steeply down.
The table uses a straight-line fit after 0.5 seconds for small commands and
0.15 seconds for larger commands. High-speed samples are short because angular
travel is limited. These are individual runs, not statistical confidence bounds.
Yaw feedback is noisier and its observed directional asymmetry needs more
measurement before encoding a more elaborate curve.

The useful first approximation for **both axes** is therefore:

* Commands -5 through +5: no motion.
* Larger magnitudes: approximately one degree/second per command unit.
* A finite response time: pitch onset extrapolates to roughly 40–50 ms after
  sending the command. At command 100, several more degrees of motion occur
  after sending zero before settling. Yaw also shows delayed onset/stopping.

The simulator uses independent signed `APCAM_SIM_PITCH_RATE_CURVE` and
`APCAM_SIM_YAW_RATE_CURVE` tables in `target_mt11.h`, with a 40 ms first-order
rate response. It integrates motion through the response and deceleration,
including when a zero command arrives. This is an approximation of the combined
command/actuator response: transport and feedback latency have not been separated
from motor dynamics. Signed tables permit later asymmetric calibration. Other
targets retain their existing linear, immediate response.

Firmware requested-rate conversion is separate from the simulated physical
response. Both MT11 axes now use 100 deg/s per full-scale command. The old pitch
conversion assumed 60 and consequently commanded about 67% too much speed.
A requested speed below the hardware minimum remains unachievable as a constant
rate command. The realistic simulator exposes roughly 2.4 degrees of following
error in the 4 deg/s ROI test; the previous ideal model hid that effect. The
regression bound is 3 degrees, while still checking that no absolute angle
commands are sent. Low-speed compensation remains separate controller work.

After installing the corrected firmware, MAVLink +/-10 deg/s requests measured
+10.10/-10.09 deg/s pitch and +9.40/-9.91 deg/s yaw. This verifies the complete
MAVLink-to-vendor conversion path, not just raw-command behaviour.

## Comparison with ZT30

MAVProxy's `mavproxy_SIYI.rate_mapping()` uses a measured ZT30 table with commands
4/5/6/7/10 corresponding to 0/1.75/3/4.7/9 deg/s and command 70 corresponding to
98 deg/s. It interpolates the inverse map and quantises to an integer command.
That curve should not be copied onto MT11: its dead zone, minimum moving speed
and full-scale response differ substantially.

## Earlier overshoot fixes

MT11 yaw feedback wraps at +/-180 degrees while the gimbal rotates continuously.
Treating the seam as a bounded joint caused nearly 360-degree errors and large
spurious reversals. `APCAM_GIMBAL_YAW_CONTINUOUS` enables circular error handling
for MT11 while preserving bounded-joint handling on other targets. SITL tests
both seam directions, including a 60% plant gain error.

The original proportional error filter delayed braking. Its time constant was
reduced from 0.2 to 0.05 seconds, proportional gain from 2 to 1.2, and the MT11
yaw command conversion was corrected. The subsequent hardware tracking sample
had about 1 degree RMS yaw error after acquisition and no large seam-induced
reversals. The isolated sweeps then confirmed the pitch scale and both dead zones.

## Reproducing the measurements

Use `tools/mt11_rate_sweep.py` on a stationary bench, one axis at a time. Pause or
disconnect the flight controller during raw-command sweeps. ArduPlane switches
to streaming angle commands when camera-side geographic targeting is disabled;
even a fixed mount target can override the sweep. Check the BIN for competing
GCMD records. The successful isolated run in `00000006.BIN` had none and reported
zero dropped records and zero storage errors.

The tool temporarily suspends geographic tracking, enables disarmed BIN logging,
recentres before each sample, sends raw SIYI bytes and records CSV feedback. It
records feedback after each stop, checks travel and feedback timeouts, and
restores the previous parameters on exit. The MCU generates its own reply
sequence numbers, so queued feedback is discarded before polling rather than
matching request and response sequences. Absolute yaw may settle several degrees
off zero; the tool accepts a stable central pose and measures relative travel.

```sh
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis pitch --output pitch.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis yaw --output yaw.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --axis pitch --commands 4,5,6,7,10,12 --reverse --pitch -20 --output pitch-repeat.csv
```

Follow with `--commands 16,20,30,40,60,80,100`. Inspect actual settled intervals:
high speeds reach the travel limit before the requested hold duration. The CSV
`phase` distinguishes running from stopping; stopping records have command zero.

Download numbered BIN files through **Files → logs**, then run:

```sh
python3 tools/analyze_gimbal_rates.py flight.BIN --settle 0.15 --window 0.2 --samples windows.csv > response.csv
```

Remaining calibration: physically inverted mounting, repeatability across
headings/temperature, continuous ascending/descending commands without recentering
to measure hysteresis, and separation of transport delay from actuator dynamics.
The recentered ascending/descending sweeps do not establish absence of hysteresis.

## Tracking-only low-speed conversion

Geographic ROI tracking now uses pulse-density conversion below the measured
6 deg/s minimum speed: zero and minimum-speed commands deliver the requested
average motion. Pulse credit clears on zero demand, direction reversal and
tracking stop. Existing rate limits, joint limits and stale-data stop paths
remain in force. Explicit MAVLink and SDK rate commands retain their original
conversion. PIDP/PIDY Output records the filtered continuous rate request;
GCMD records the actual zero/minimum-speed wire commands.

The calibrated MCU-model regression (`make sitl-rate-default-test`) starts with
a 3-degree pointing error and tests 0.5 deg/s motion, direction reversal and
stale feedback. This is simulation qualification against the measured steady
rate curve and response lag, not a new physical short-pulse or flight test.
