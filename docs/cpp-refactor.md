# C++ migration and structure

`pr-cpp` starts from `pr-mt11-fixes` (10aeba9). This is the first migration
step, not a replacement of the camera drivers or protocols.

## Compiler and compatibility boundary

Project-owned camera, web, native capture helper and platform utility sources
use `.cpp` and GNU C++17. Firmware disables exceptions and RTTI. Vendor SDK C
sources (including the MT11 scene and sample libraries) still use the C
compiler. The RTSP dependency retains its existing C++11 build. No SDK source
or binary ABI is converted. Dynamic SDK symbols and kernel structures retain
their existing layouts; compile-time ABI assertions remain enabled.

`CXX` selects the cross C++ compiler and `HOST_CXX` the host compiler.
Camera `CFLAGS` remain shared warning/optimization settings, with the C
language standard removed before applying `CXXFLAGS`. Required GNU C++17,
no-exceptions and no-RTTI flags follow user overrides; the web build also
preserves its required warning flags. The web build accepts `CXXFLAGS` and
retains `CFLAGS` compatibility with existing scripts. ARM release binaries
link the C++ runtime statically so they do not require newer libraries on the
camera. The Windows native builder uses the same C++ sources and still bundles
its Cygwin dependencies. Native macOS SITL uses Apple Clang and libc++, with
small host-only wrappers for Linux descriptor flags, monotonic waits and file
sync. Its web service uses macOS process APIs, and the launcher uses psutil to
find detached restart children. XOP is prepared into a separate host source
tree for the select-based event loop; the downloaded SDK tree stays unchanged.
See `sitl/README.md` for Mac setup.

The compatibility changes include explicit pointer/enum conversions, ordered
initializers, C-linkage declarations at external boundaries, and C++ atomics.
Objects containing atomics are constructed with checked `new (std::nothrow)`
and destroyed with `delete`; they must not be copied or cleared with `memset`.
Existing allocation failure paths and worker shutdown ordering are preserved.

## First structural changes

* `APC_Camera` is an abstract, non-copyable capability interface. Its immutable
  target implementation reads the existing target headers, which remain the
  single source for firmware, XML, the web UI and Python simulator exports.
* `APC_Lens` provides sensor dimensions, lens type, optical/digital zoom
  capabilities and horizontal `get_FOV()`. Zoom is relative to that lens.
  FOV uses the existing focal-length model or measured wide/tele endpoints;
  it is not a linear division of the angle by the zoom factor. Unknown or
  invalid FOV inputs return zero, preserving the metadata convention.
* Lenses and streams remain separate. A stream has a lens mask and resolution
  mask; recording has its own resolution mask. No single stream resolution is
  substituted for sensor or recording resolution.
* `APC_CameraApp` owns backend, media, protocol servers and the manual-control
  lease. Its destructor handles partial startup and orderly shutdown. Command
  line validation takes place before service ownership starts.
* `APC_StringBuffer` owns web output storage, is non-copyable, and reports
  allocation errors without exceptions. Explicit `release()` transfers a
  completed response to the existing HTTP layer. HTML/URL/log escaping stays
  in this class. Translations have their own header, separate from routing.

The existing `ca_*` driver/control interfaces remain during this migration.
It deliberately does not change wire signs, rate calibration, lens selection,
XML parameter types, recording formats, service paths or installed settings.
Capability queries allocate no memory and do no hardware I/O.

## Second structural stage

* `APC_Media` owns the stable media frontend, diagnostic workers, live settings
  and pipeline replacement/rollback. `APC_Media_Backend` defines the image
  pipeline interface; MT11, A8, ZR10, Z1-Mini and SITL each implement a concrete
  backend that owns SDK state and capture-worker shutdown. Gimbal transport
  remains independent. Existing protocol callbacks retain the `ca_media_*`
  facade while new services can use the class directly.
* `APC_Config` supplies the shared field descriptors, allowed values and
  validation. The camera parser, numeric MAVLink parameters, XML and parameter
  logging retain their existing order/names, and the web form now uses this
  schema for validation and numeric limits. UI labels and translations remain
  web metadata rather than creating a second configuration schema.
* `APC_HTTPRequest` owns parsing storage; `APC_HTTPResponse` centralizes response
  framing and security headers. `APC_WebRoot` restricts reads to a generated
  asset manifest and regular files, and expands explicit HTML placeholders.
  Nine JavaScript files, two CSS files and seventeen HTML fragments live under
  `web/webroot/`. Dynamic/translated content still renders on the server.
* Every hardware package installs its webroot with the web binary. Native SITL
  uses the source webroot; the Windows bundle carries its own copy and supplies
  its location to each simulated web service. Z1-Mini firmware predating this
  change needs one update through the XFRobot updater because its old web
  updater rejects additional package filenames (see its installation notes).
  New Z1 web binaries also have a built-in recovery login/upload page if their
  webroot is missing or incomplete; they can install a complete package using
  the normal authenticated updater without relying on external assets.
* Scoped mutex, condition and descriptor owners begin replacing explicit
  cleanup in the extracted media frontend and application startup. Media
  capability decisions use `APC_Camera`; SDK selection remains compile-time.

## Further work

Continue splitting authentication/session storage, parameter rendering, media
browsing and firmware update transactions out of the remaining web service.
Convert individual run-loop protocols into narrow owners, maintaining their
shutdown order and wire behaviour. Adopt scoped locks/descriptors inside the
vendor backends incrementally with hardware verification; this stage preserves
those existing SDK transaction sequences and keeps the `ca_*` compatibility
interfaces available.

Follow the ArduPilot style of `APC_` classes, private underscored members,
explicit ownership and capability methods. Avoid broad inheritance trees,
exceptions, RTTI, implicit heap allocation in control loops, and a second
hand-maintained database of camera properties.

## Validation

Run `make release` for all hardware packages, `make -C camera_app test
exposure-test zr10-test`, `make -C web test zr10-test network-status-test`,
`python3 tests/test_z1mini.py`, and the existing XML, overlay and simulator
checks. `tests/test_targets.cpp` checks the capability class on all four
targets, including calibrated FOV and unsupported zoom. The web suite includes
buffer ownership, escaping, reuse and allocation-overflow checks.

The initial compiler migration was tested successfully on a real MT11. This
second structural stage still needs device verification, especially MT11 USB
thermal capture and each vendor SDK pipeline. Host compilation and simulator
tests are not proof of vendor hardware behaviour.
