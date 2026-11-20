# Built-in timezone names

The web timezone dropdown and `[general] timezone` use the 110 IANA names in
[zones.txt](zones.txt), including `GMT` (the default), `UTC`, `Europe/London`,
`America/New_York`, `Asia/Kathmandu` and `Australia/Sydney`.

All camera targets and SITL use the same built-in rules. The firmware does not
require `/usr/share/zoneinfo` or an Internet connection. Configuration stores
the selected name; `APC_Timezone` translates it to a POSIX rule before passing
it to libc. Existing explicit POSIX settings, such as `GMT-10`, remain valid
and remain selected in the dropdown until changed. Unsupported names are
rejected instead of silently becoming GMT.

These are **current recurring rules**, derived from IANA tzdata 2026d, rather
than a full historical timezone database. The generator verifies offsets and
abbreviations against that release from September 2026 through 2099, sampling
each month and both sides of every recorded transition. The selection omits
zones whose future changes cannot be represented by one recurring rule. For
example, Morocco's Ramadan exceptions require transition tables, and Israel's
`/26` transition rule exceeds the ZR10 libc's supported transition hours. Dates before
September 2026 use the current rules, not the historical rules. Future legal
changes require updating the firmware's table; the verification horizon is
not a prediction of future legislation.

This representation also works with the ZR10's uClibc, which does not implement
full IANA zoneinfo conversions. Daylight saving, southern-hemisphere seasons,
and fractional-hour offsets are included for the supported names. A UniGCS
client that supplies local calendar time must use the same timezone as the
camera. MAVLink and public SDK epoch timestamps are independent of this setting.

To regenerate after reviewing a tzdata update, update the version and SHA-256
in `tools/build_timezones.py`, review `zones.txt`, then run:

```sh
curl -fLO https://data.iana.org/time-zones/releases/tzdata2026d.tar.gz
python3 tools/build_timezones.py tzdata2026d.tar.gz
make -C camera_app timezone-test
```

Generation requires Python 3.9+ and `zic`. Ordinary builds use the checked-in
`include/apcam/timezones.h`; they neither download nor regenerate timezone data.
The IANA source data is public domain; see [LICENSE](LICENSE).
