# MT11 support tools

Normal firmware builds unpack these prebuilt tools instead of compiling their
third-party source trees. They are stripped, statically linked AArch64 Linux
executables, compressed individually with XZ. The initial bundle uses the same
binaries previously built and included in MT11 firmware; it does not change their
versions, options or runtime dependencies. Other camera targets do not use them.

`make mt11_tools` verifies the compressed and decompressed SHA-256 checksums and
ELF architecture, then installs the tools in `build/deps/mt11-tools/bin/`.
It requires only Make and Python 3 (including its standard `lzma` module), with
no compiler, network access or build-environment installation. Repeated calls
preserve unchanged files and repair missing or damaged cached executables.
`MT11_TOOLS_ROOT` can override the output directory.

The binaries are `rsync`, `strace`, `tcpdump`, `ltrace`, `dropbear` and
`dropbearkey`. `manifest.json` records executable sizes, checksums, component
versions and the source-build recipe hash. `sources.json` records the upstream
source archive URLs and SHA-256 pins used by that recipe. Upstream license texts
and notices are in `licenses/`; licenses of the original components apply to
these binaries.

## Updating

Run from the repository root on an x86_64 Linux build host:

```sh
python3 tools/install_build_environment.py --targets mt11 --with-tool-build-deps
# Use a fresh directory so old configure results cannot affect the rebuild.
tools/build_mt11_tools.sh build/mt11-tools-rebuild aarch64-linux-gnu-
python3 tools/prebuilt_mt11_tools.py --refresh-from build/mt11-tools-rebuild
make mt11_tools
python3 tests/test_prebuilt_tools.py
```

The refresh command strips again, verifies static linkage and AArch64 format,
then writes deterministic XZ archives and their manifest. Update the pinned
source versions in `tools/build_mt11_tools.sh` first when upgrading, and update
`sources.json` and the corresponding license notices with the same versions.
Test the updated tools on MT11 before publishing a release. Commit the refreshed
archives, manifest, source pins, notices and recipe together. The build recipe
is retained so these binaries can always be rebuilt or replaced locally.
