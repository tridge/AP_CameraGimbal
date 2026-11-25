# Direct cloud uploads

Photos and recordings can be uploaded directly from MT11, A8, ZR10, Z1-Mini,
ZT30 and Raspberry Pi cameras. The default transfer program is `apcam-upload`,
a separate C++ HTTPS worker using pinned libcurl and mbedTLS libraries. Neither
rclone nor Python is required on the camera. Uploads are disabled by default.
The native provider currently supports Google Drive user accounts (My Drive),
including a configured `root_folder_id`. Shared Drives and service accounts
are not supported.

## Configuration

On the web Parameters page, open **Cloud Upload**. Save your Google OAuth
client ID, client secret and offline token, choose a remote such as
`gdrive:AP_CameraGimbal`, select photos, recordings or both, and enable uploads.
See [Create the Google credentials](../packaging/rpi_libcam_caddx/README.md#create-the-google-credentials)
for the consent procedure. Rclone may be used on your desktop for this one-time
consent; it is not installed on the camera.

The existing `[upload]` settings and rclone-compatible Drive credentials format
are preserved. Files go into `<remote folder>/photos` and
`<remote folder>/recordings`, preserving subdirectories. Formats such as MP4,
MPEG-TS and raw thermal recordings are transferred without conversion.

Credentials are stored as `upload.conf` beside the persistent `camera.ini`.
On RPi, the existing `/var/lib/ap_cameragimbal/rclone.conf` path is retained.
The web service writes private files and never redisplays their contents.
The worker requires owner-only file and directory permissions; SD filesystems
must be mounted with suitable masks (for example `fmask=0077,dmask=0077` on
ZR10). The credentials directory must be writable for resumable upload state.

Optional environment overrides:

- `CAMERA_APP_UPLOAD_WORKER`: external worker executable. By default the
  camera starts `apcam-upload` beside its actual executable.
- `CAMERA_APP_RCLONE`: legacy spelling of the executable override, used only
  when `CAMERA_APP_UPLOAD_WORKER` is unset.
- `CAMERA_APP_RCLONE_CONFIG`: credential file override; set this in **both**
  the camera and web services. The name is retained for compatibility.
- `CAMERA_APP_UPLOAD_CA_FILE`: alternate CA trust store for the native worker.
  By default it uses `upload-ca.pem` beside its executable.

The camera needs working DNS, internet access and a correct clock for TLS.
Certificate and hostname verification are always enabled. Production workers
only use the Google OAuth/Drive endpoints; the local test endpoint switch is
compiled into test binaries alone.

## Scheduling and data handling

The camera supervises uploads without waiting for network or disk transfers in
its control loop. The worker runs at low CPU and I/O priority. It is stopped
when recording starts or the vehicle arms. After flight-controller contact has
been established, a heartbeat missing for five seconds also stops uploading.
With no flight controller ever connected, bench uploads are allowed.

Scans occur at startup, after disarming or recording stops, and every
`scan_minutes`. Errors retry after 30, 60 and then 120 seconds. A freshly closed
file must remain unchanged for two seconds before uploading. Hidden files,
symlinks, empty files, `.tmp` and `.part` files are skipped.

Transfers use 256 KiB resumable chunks with small read buffers and 64-bit file
offsets. Private `<credentials>.state` checkpoints allow a later worker to
query the server and resume after interruption. One worker can use a credential
set at a time. Obsolete checkpoints from files that were changed or removed
can be deleted while uploads are disabled if the bounded state budget fills.

A same-name Drive file is skipped only when both size and MD5 match. Different
contents replace that file through a resumable update. Ambiguous duplicate
remote names fail rather than choosing an arbitrary file. Size and MD5 are
queried again after uploading. Local deletion requires successful verification
and unchanged file identity, size and timestamps.

`after_upload` selects `keep`, `delete`, or `delete_when_low`. The last mode
checks free space at the start of each photos/recordings job; if it is below
`low_space_percent`, all successfully verified files in that job are deleted.
It does not stop deleting when the threshold is reached. `bandwidth_kib_s`
limits transfer speed; zero means unlimited.

## Builds and extension

Ordinary camera, SITL and package builds include the worker and CA store.
For a worker alone use `make -C camera_app upload-worker CAMERA_BACKEND=...`
with the camera's normal cross compiler.

`tools/build_cloud_upload.sh` downloads checksum-pinned curl/mbedTLS sources
and builds static HTTPS libraries separately for each toolchain under
`build/deps/cloud-upload`. It requires CMake, make, Python 3, curl, tar and
sha256sum on the build host. Full licence notices ship in the web assets;
see [THIRD_PARTY.md](../THIRD_PARTY.md) for source provenance.

The transfer protocol remains a separate executable accepting copy/move,
source, remote, credentials and bandwidth arguments and emitting JSON status.
The supervisor can therefore support other providers without adding their TLS
or blocking I/O to camera control. The native Drive implementation separates
HTTP transport, provider operations and local traversal/checkpoint handling.

`make -C camera_app native-upload-test` exercises local HTTPS OAuth/Drive
fixtures, token refresh, interruption/resume, checksum failures, changed files,
private credentials, TLS rejection and bandwidth handling. Existing supervisor
and web tests cover recording/arming cancellation and settings/credentials.
Cross-build and package checks do not substitute for a live Google account
upload and physical-camera qualification; those have not been performed.
