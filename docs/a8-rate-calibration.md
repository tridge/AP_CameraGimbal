# A8 rate response and tracking calibration

## Bench measurements, 2026-09-16

The A8 MCU reported inverted mounting. With no flight controller connected,
raw SIYI command sweeps exercised each axis separately, in both directions.
Each sample recentered first. The firmware logged every command and feedback
sample in self-describing BIN files. The complete sweeps had no competing GCMD
records, no dropped records and no storage errors.

Rates below are measured from angle derivatives, not the reported gyro axes.
Values are approximate; directional yaw differences persisted in a descending
repeat at a second pitch angle.

| Raw command magnitude | Pitch + / −, deg/s | Yaw + / −, deg/s |
| --- | --- | --- |
| 1–5 | 0 / 0 | 0 / 0 |
| 6 | 4.3 / −4.3 | 5.0 / −4.6 |
| 10 | 7.3 / −7.3 | 8.6 / −7.6 |
| 20 | 14.7 / −14.6 | 17.8 / −14.4 |
| 40 | 29.6 / −29.4 | 35.1 / −26.6 |
| 60 | 44.4 / −43.9 | 53.5 / −39.4 |
| 80 | 59.4 / −58.9 | 71.8 / −51.7 |
| 100 | 73.9 / −73.6 | 89.3 / −64.7 |

Pitch onset and stopping fit a first-order response with about 0.10 s time
constant plus 0.03 s delay. Fits to the shorter high-speed samples include both
the running and stopping portions. Early negative-pitch high-speed samples
approached the stop and were discarded; repeating from level provided clearance.
Yaw fits have more noise, with combined lag roughly 0.1 s. Transport and feedback
latency are not separated from actuator dynamics.

The simulator uses signed per-axis curves and a 0.12 s first-order response as
an approximation of that combined lag. The real requested-rate conversion uses
separate signed curves, chooses the nearest achievable integer command, and
explicitly compares with zero across the dead zone. It no longer assumes both
axes reach 60 deg/s at command 100. Vendor SIYI commands still pass through
unchanged. End-to-end MAVLink requests of ±10 deg/s measured +10.38/−10.27 pitch
and +10.47/−10.48 yaw after installation, within integer-command quantisation.

## Tracking correction and validation

The old proportional controller stopped correcting when its output fell into
the motor dead zone, leaving about 2–3 degrees of static pointing error. A8 now
uses the calibrated inverse rate maps and a bounded integral correction:

- Integral gain 0.4 s⁻², configured by `APCAM_TRACKING_RATE_I`.
- Integrate the filtered error outside the existing 0.2-degree tolerance only
  within 5 degrees of the target; reset during larger acquisition errors.
- Limit integral output to ±6 deg/s; reset at joint limits and when tracking
  stops, including stale telemetry or feedback.
- Other targets retain their previous zero integral gain. The existing MT11
  tracking, seam crossing, stale-data stop and BIN logging regressions pass.

The same 53-second synthetic flight-controller trajectory was replayed against
the stationary hardware before/after installation and against calibrated SITL.
Yaw steps between ±15 degrees then ramps at 2 deg/s; pitch steps between −15 and
−5 degrees then ramps at −0.667 deg/s. These ramps are slower than either motor's
minimum sustained speed, so some small intermittent motion is unavoidable.

| RMS pointing error | Hardware before | Hardware after | Calibrated SITL |
| --- | ---: | ---: | ---: |
| Yaw, positive step settled | 2.88° | 0.52° | 0.40° |
| Yaw, negative step settled | 2.81° | 0.47° | 0.60° |
| Yaw, slow ramp | 1.36° | 0.49° | 0.49° |
| Pitch, positive step settled | 1.90° | 0.10° | 0.10° |
| Pitch, negative step settled | 1.70° | 0.30° | 0.30° |
| Pitch, slow ramp | 2.29° | 0.55° | 0.56° |

Settled windows are seconds 15–17.75 and 25–27.75; the ramp window is 33–42.75.
These exclude acquisition transients, not ordinary tracking fluctuations.
Hardware peak errors in the ramp window were 1.02° yaw and 1.21° pitch; SITL was
1.08° and 1.23°. The final hardware log had zero dropped records or storage errors.

BIN logging previously used `/mnt/logs` on A8, outside its SD mount. The default
is now `/mnt/mmc/logs`, with the boot supervisor's no-card guard also covering
logging. The original logs were downloaded before the calibration firmware reboot.

## Reproduce

Disconnect the flight controller and use a stationary bench with unobstructed
gimbal travel. The tools temporarily enable disarmed logging and restore the
original settings afterward. Select mounting explicitly; the examples use
inverted mounting. Use smaller travel windows near mechanical stops.

```sh
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --backend a8 --inverted \
    --pitch -10 --axis yaw --commands 0,1,2,3,4,5,6,7,10,20 --output yaw.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --backend a8 --inverted \
    --pitch 0 --axis pitch --commands 20,40,60,80,100 --max-travel 10 \
    --stop-hold .8 --output pitch.csv
python3 tools/mt11_rate_sweep.py --host 192.168.144.25 --backend a8 --inverted \
    --pitch -10 --axis yaw --commands 10 --mavlink-rates --output yaw-mavlink.csv
python3 tools/gimbal_tracking_bench.py --host 192.168.144.25 --backend a8 --inverted
python3 tools/analyze_gimbal_rates.py sweep.BIN --settle .5 --window .3
make -f tools/dev.mk a8_sitl-rate-test
```

Do not fit short, high-speed samples with a settling window longer than their
running time. Inspect travel limits and stopping motion as well as the table.

Remaining validation: physically upright mounting, more temperatures and
headings, moving-base behaviour, and hysteresis without recentering. These curves
currently apply to both mounting modes; only the reported inverted mode was
measured here. In that mounting, a −45-degree start request stopped near −25;
the sweeps and tracking comparison stayed clear of that limit. Orientation-specific
travel limits need separate verification before changing advertised capabilities.
