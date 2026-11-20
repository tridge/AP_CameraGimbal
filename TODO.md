# TODO

- Re-calibrate thermal.
- Provide parameter descriptions/metadata for regular GCS parameter editors; basic MAVLink parameter fetch/get/set is implemented ([#1](https://github.com/ArduPilot/AP_CameraGimbal/issues/1)).
- Ensure recordings have correct dates and sortable, zero-padded, hyphen-separated filenames ([#2](https://github.com/ArduPilot/AP_CameraGimbal/issues/2)).
- Check compliance with the [MAVLink camera protocol](https://mavlink.io/en/services/camera.html).
- Ensure `cam_definition_version` changes whenever the camera definition XML
  changes; consider deriving it from a 16-bit hash of the XML.

## A8 mini backend

- Clarify local RTSP versus SupportProxy stream settings ([#4](https://github.com/ArduPilot/AP_CameraGimbal/issues/4); [#7](https://github.com/ArduPilot/AP_CameraGimbal/issues/7) is a duplicate). The `main.264` alias is served since `[stream.main] alias` was added.
- Fix outdoor overexposure and verify that exposure adjustments work on hardware ([#5](https://github.com/ArduPilot/AP_CameraGimbal/issues/5)).
- Fix photo capture while video recording, including repeated photogrammetry captures ([#8](https://github.com/ArduPilot/AP_CameraGimbal/issues/8)).

- Gimbal link follow-ups: the vendor app sends a 1 Hz status heartbeat
  (sub-command 0x3f) and 25 Hz per-frame data (0x02) to the gimbal MCU that
  the backend does not send yet; check whether anything (LED, motor
  behaviour) depends on them. Sub-command 0x14 is still undecoded.
