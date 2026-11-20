# ZT30 application bundles

Application bundles update only camera-app, zt30-web, encoder-sdk.so and webroot
assets. Settings, credentials, SSH identity/binaries, stable launchers, retained
vendor SDK, kernel, bootloader and gimbal MCU firmware are outside the bundle.
They require the stable AP launchers and a safe slots directory, at launcher
version 1. Staging remains available when the active selection is damaged.

Build the release bundle and its instructions/checksums with:

```
make release RELEASE_TARGETS=ZT30
```

The package is written to `release/<version>/SIYI_ZT30/`. The default build
checks the pinned runtime in `packaging/zt30/platform` and builds the native
camera application, web server and SDK adapter before creating the bundle.
`make zt30_package` builds just the bundle under `build/`; `ZT30_PACKAGE_OUT`
overrides its path. This is a web application update, and requires the stable
AP launchers described below to be installed already.

For a different retained runtime, set `ZT30_RUNTIME_ROOT` to a captured root
containing `/config/lib` and `/lib`, with a `SHA256SUMS` manifest at its root.
`ZT30_SDK_LIBDIR` defaults to that root's `/config/lib`. The retained libraries
must match the receiving camera. The bundle builder can also be run directly
after compiling the three native binaries:

```
python3 tools/build_zt30_app_bundle.py --runtime-root /path/to/captured/root \
    --version v1.0.0 --output ZT30_AP_v1.0.0.apbundle
```

The builder runs check_zt30_elf.py on both executables and the SDK adapter,
including ARMv7 hard-float attributes, interpreter, version ceilings, exact
runtime exports and transitive dependencies in /config/lib, /lib and /usr/lib.
The camera checks basic ELF32/ARM/EABI5/hard-float headers and interpreter, and
compares every declared retained runtime file against its qualified size and
SHA256. It never executes candidate code while staging. Full ABI qualification
is a builder responsibility; the native tool does not independently prove the
manifest describes all dependencies of an arbitrarily constructed executable.

Copy the bundle to /tmp using SSH, then:

```
/customer/camera-app/zt30-slot stage /tmp/ZT30_AP_v1.0.0.apbundle
/customer/camera-app/zt30-slot activate <printed-slot-id>
```

`stage` takes the shared writer lock, validates identity/paths/modes/bounds/runtime,
checks space, copies to a private directory, verifies payload hashes and basic
ELF, and fsyncs the complete slot before publication. It prints the SHA256 of the
canonical manifest, which is the slot ID. It does not change active or restart
services. `activate` re-verifies the canonical manifest ID, entire file tree, modes,
checksums, ABI and runtime pins before atomically changing active under the same
lock; use a
coordinated application/listener restart to run the selected bundle. Explicit
rollback uses activate with the previous AP slot ID and the same restart.
Trusted Stage 2a development manifests remain available for rollback, with
manifest-ID and structural checks; they are SSH-only migration artifacts, not
qualified release bundles. Runtime pins apply to rollback too: if retained
platform libraries change, an older bundle needs its matching runtime.
Native ZT30 web upload is supported after installing the current stable
zt30-slot, app-init.sh, app-supervisor.sh and web-supervisor.sh together. This
bootstrap maintenance is required when migrating Stage2a/2b development units;
layout version1 alone does not identify the newer trial-maintenance commands.
Do it through the existing SSH maintenance path before selecting the first
bundle with the web uploader. Subsequent application bundles use the web UI.

Select ZT30_AP_*.apbundle on Status. Authentication and CSRF protection use the
same policy as other camera installers. One upload is allowed, at most32MiB
and120seconds, into tmpfs. Restart and slot-writer locks protect the transaction.
The persistent starting trial precedes activation. A new app must have the
selected executable identity and two complete live RGB fragments; thermal is
reported separately and may need up to12seconds for a preview keyframe.
The HTTP201 means the app is ready and web confirmation remains pending.
The browser waits for the exact candidate and a new web epoch to confirm.

The independent supervisor identifies the actual TCP LISTEN owner, starts the
selected web/assets, and confirms only when candidate app, live RGB and candidate
web login response are available. An unconfirmed trial on a later Linux boot,
startup deadline, or three failed web launches restores the previous AP selection.
Recovery is journaled so an interruption during rollback can resume. An operator
selection outside the transaction is preserved. Status shows selected, camera
and web bundle versions, and offers Restore previous application using the same
transaction. Settings/authentication remain outside every slot.

The previously selected AP bundle must still pass full verification for rollback.
If its payload or retained runtime changed, recovery remains over independent
SSH; the updater does not bypass integrity checks or launch vendor software.
Healthy restart tests do not prove recovery of the known intermittent thermal
USB stall. Thermal degradation does not prevent a healthy RGB/web update.

Slots are retained. No published slot is deleted or overwritten, including one
used by a running app, web listener or logger. Re-staging the same ID succeeds
only when the entire existing tree, permissions and bytes match. Damaged slots
are rejected. Interrupted private stages are removed under the writer lock on
the next valid staging attempt; symlinks inside them are unlinked, never followed.
A malformed/symlink stage root fails closed for manual inspection. Only up to16
orphan stages,512 entries/tree,6 directory levels are handled.

Flash must have room for one staged payload plus manifest/directory overhead and
an 8 MiB reserve. Upload to /tmp (RAM) to avoid retaining a second flash copy.
Retained slots eventually consume space; the current installer fails closed,
without garbage collection. Pruning needs separate live-process protection.
Failure after final slot rename can leave a complete unselected slot. Failed
active-link directory fsync can mean active already changed; inspect it before
retrying or assuming rollback. Initial SD-card flash packaging is separate.

## Wire format v1

* Eight bytes ASCII `APCZT301`, then a four-byte little-endian manifest length.
* Canonical ASCII JSON manifest (1..65536 bytes), then each file's raw bytes in
  manifest order. No compression, archive paths, symlinks, executable scripts,
  padding or trailing bytes. Entire bundle at most32 MiB.
* Exact object field order: schema (`apcg.zt30.app.v1`), board (`SIYI_ZT30`),
  launcher (1), version, runtime, files. No whitespace, unknown/duplicate fields
  or string escapes. Versions are bounded ASCII tokens.
* Runtime entries sorted uniquely by name: name, path, size, sha256. At most64.
  Path is a single file beneath /config/lib, /lib or /usr/lib. Actual runtime
  symlinks may be followed, as captured platform libraries use them.
* Files sorted uniquely by path: path, mode, size, sha256. At most128.
  Files, derived directories and manifest combined must not exceed256 entries.
  Three binaries use mode493 (0755), at most16 MiB each. Assets use mode420
  (0644), at most2 MiB each, restricted ASCII path components, depth at most4
  beneath webroot, and html/js/css/txt extensions. No leading dots or `..`.
* The three binaries and live.js, status-actions.html, parameters-form.html are
  required. Assets may evolve without updating the stable native installer.
* SHA256 is64 lowercase hexadecimal digits. manifest.json is stored byte-for-
  byte in the slot and hashes to its directory name.

These are unsigned bundles: SHA256 detects corruption, not malicious
substitution. Install only trusted builds through authenticated SSH or the later
CSRF-protected web upload. No package-controlled installer commands run.

Trial journals keep the original strict `trial.json` schema so the stable launcher
and older web payloads can read them. The older manual restore selection is saved
in a private `trial.rollback.v1.<sha256>` sidecar with schema
`apcg.zt30.rollback.v1`. Its name binds it to the journal's previous slot,
candidate slot, boot ID and deadline (joined by newlines). The sidecar is synced
before publishing that journal epoch; old epochs remain available across an
interrupted publication. Older inline rollback fields are accepted and migrated
when a new helper writes the journal.
