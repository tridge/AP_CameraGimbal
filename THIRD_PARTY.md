# Third-party dependencies

The `modules/mavlink` git submodule pins the MAVLink message definitions and
the pymavlink generator (`git submodule update --init --recursive`):

| Dependency | Revision | License | Source |
|---|---|---|---|
| MAVLink (ArduPilot fork, with pymavlink) | `71d925850d7ddb01d0e56defc1046f9d9d3bf8ae` | LGPL-3.0-or-later generator; generated code MIT (see its `COPYING`) | <https://github.com/ArduPilot/mavlink> |

The camera application generates its MAVLink C bindings from that submodule
at build time with `mavgen.py` under the ignored `camera_app/build/mavlink`
tree, so only MIT-licensed generated code is linked into the binaries.

`make dependencies` and `make mt11_tools` fetch these pinned public build
inputs:

| Dependency | Revision | License | Source |
|---|---|---|---|
| SS928V100 SDK V2.0.2.2 MPP Sample | `eef3b001cbf3d55f44392878bb7f4817cdb45e6c` | Apache-2.0 | <https://gitee.com/hieulerpi/SS928V100_SDK_V2.0.2.2_MPP_Sample> |
| minimp4 | `5a212a18dba7dca09543bbc7d65619274fd2931a` | CC0-1.0 | <https://github.com/lieff/minimp4> |
| rsync | 3.5.0 | GPL-3.0-or-later | <https://download.samba.org/pub/rsync/src/rsync-3.5.0.tar.gz> |
| strace | 7.1 | LGPL-2.1-or-later | <https://github.com/strace/strace/releases/tag/v7.1> |
| ltrace | 0.8.1 | GPL-2.0-or-later | <https://gitlab.com/cespedes/ltrace/-/tree/0.8.1> |
| elfutils libelf | 0.196 | LGPL-3.0-or-later OR GPL-2.0-or-later | <https://sourceware.org/elfutils/ftp/0.196/elfutils-0.196.tar.bz2> |
| zlib | 1.3.2 | Zlib | <https://zlib.net/zlib-1.3.2.tar.xz> |
| musl libc | 1.2.5 | MIT | <https://musl.libc.org/releases/musl-1.2.5.tar.gz> |
| libpcap | 1.10.6 | BSD-3-Clause | <https://www.tcpdump.org/release/libpcap-1.10.6.tar.xz> |
| tcpdump | 4.99.6 | BSD-3-Clause | <https://www.tcpdump.org/release/tcpdump-4.99.6.tar.xz> |
| libxcrypt | 4.5.2 | LGPL-2.1-or-later | <https://github.com/besser82/libxcrypt/releases/tag/v4.5.2> |
| Dropbear SSH | 2026.94 | MIT | <https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2> |

The bootstrap script verifies the downloaded minimp4 header against its pinned
SHA-256. The camera build copies that header and applies
`camera_app/src/recording/minimp4_metadata.patch` to support the `mett` JSON
metadata track and omit the unknown fragment duration header so VLC can
derive duration from the recorded fragments; it also enables fragment
decode-time support. The verified
download is left unchanged. The SS928 checkout is detached at the exact revision above.
The MT11 tools builder likewise verifies every release archive before building
static AArch64 executables. These utility sources and build products remain
under the ignored `build/deps` tree.

The MT11 package also uses the reviewed binary `kernel` and `rootfs` images in
`packaging/mt11/base`. They were extracted from the publicly distributed MT11
V1.0.5 update and are kept separate from the GPL-3.0 project sources. The
rootfs includes third-party userspace and SS928V100 binary kernel modules; the
kernel contains Linux under GPL-2.0. Review the vendor redistribution terms and
corresponding-source obligations before publishing or mirroring these binary
inputs.

The ZR10 backend vendors an MIT-licensed subset of the OpenIPC/divinus
SigmaStar MI ABI headers at revision `0244156023a2ff13fc6571c7851332e0a1818d76`
from https://github.com/OpenIPC/divinus. The declarations and copyright notice
are in `camera_app/src/backends/zr10/mi`; the application package includes the
notice as `OpenIPC-LICENSE.txt`. Camera SDK libraries and factory IQ data are
loaded from the existing ZR10 installation and are not redistributed in the
application archive. The Bootlin ARMv7 hard-float uClibc 2018.11-1 toolchain is
checksum-pinned by `tools/bootstrap_zr10_toolchain.sh`; its toolchain binaries
are build inputs and are not shipped in the camera package.

`tools/install_build_environment.py` additionally installs the Bootlin ARMv7
hard-float glibc 2020.02-2 toolchain for A8 and Arm GNU 10.2-2020.11 for Z1-Mini,
using fixed SHA-256 checksums. It fetches AX620A headers from
[`sipeed/axpi_bsp_sdk`](https://github.com/sipeed/axpi_bsp_sdk/tree/d61e665f44ee145e5bb7cda67d64ac6be0b9097e)
at revision `d61e665f44ee145e5bb7cda67d64ac6be0b9097e` for native Z1-Mini capture.
These remain build dependencies, outside the installed camera payload.

The A8 and ZR10 flash builders use the small checked-in platform sets under
`packaging/a8/platform` and `packaging/zr10/platform`. Their READMEs identify the
source versions and each set has a checked SHA256SUMS. They contain required
kernel modules, ISP tuning and network defaults, with no vendor camera app,
Boa, sound or graphical assets. The kernel modules and tuning retain their
respective component licensing; they are separate from AP application code.

The built-in timezone name/rule table in `include/apcam/timezones.h` is derived
from the public-domain IANA tzdata 2026d release. Its source archive SHA-256 is
`0cb2aa8e333c3dc049badc42a0c61f21987b8cd44e107fa900bad764aacc7767`.
See `timezones/LICENSE` and `timezones/README.md` for the source, regeneration
procedure and current-rule coverage. No IANA implementation code is included.

The image tracker uses dlib **20.0.1** (Boost Software License 1.0), pinned by
`tools/bootstrap_dlib.sh` to release archive SHA-256
`dab5b4ec4b68bd7dc128a1fb7900723f89d2da107e44cd5def7d38fc57252a9d`.
Only the CPU correlation tracker and its FFT implementation are compiled;
there are no neural-network weights, GUI, CUDA or BLAS dependencies.
Source: <https://github.com/davisking/dlib/tree/v20.0.1>.
The bundled KISS FFT is BSD-3-Clause, copyright Mark Borgerding 2003–2010.
Both license notices are in `web/webroot/licenses`, copied into every camera
package with the web assets.

The ZT30 qualification inputs under `packaging/zt30/platform` are byte-for-byte
libraries from the tested SIYI ZT30 0.1.9 installation, pinned in that directory's
`SHA256SUMS`. They include glibc 2.30 (LGPL-2.1-or-later), the GCC 9 C++ and support
runtimes (GPL-3.0-or-later with the GCC Runtime Library Exception where applicable),
and SigmaStar MI/ISP libraries with vendor-specific licensing. They are used to
validate ELF dependencies and are not installed by `.apbundle` application updates.
These inputs remain subject to their component license terms. Vendor redistribution
permission and the availability of corresponding source for the retained runtime
inputs have not been established by this port; maintainers must confirm those terms
before distributing the inputs. This entry records provenance, not a grant of rights.

The camera Files player bundles an unmodified **mpegts.js 1.8.0** browser build
(Apache-2.0), from
<https://registry.npmjs.org/mpegts.js/-/mpegts.js-1.8.0.tgz> (`dist/mpegts.js`).
Source: <https://github.com/xqq/mpegts.js/tree/v1.8.0>.
Its SHA-256 is
`e998f7a6cec5fcab56ac14e8c22d61a42565c1ebf7174b3fda078343eda6cab0`.
Copyright and attribution include Bilibili, magicxqq / zheng qian and もにょてっく.
The upstream release contains no separate NOTICE file.

The upstream source map identifies bundled `events` (Joyent and Node
contributors), `es6-promise` (Yehuda Katz, Tom Dale, Stefan Penner and
contributors), `process` (Roman Shtylman), `webworkify-webpack` (Boris Sirota
and the xqq fork), and the webpack runtime (JS Foundation and contributors),
under MIT. It also credits Dailymotion's hls.js helpers (Apache-2.0) and a
UTF-8 helper derived from libWinTF8. This product includes software developed
by StarBrilliant; the libWinTF8 copyright and redistribution terms are retained.

The full Apache and MIT licence texts, original source copyright and
attribution notices, and libWinTF8 terms are retained in
`web/webroot/mpegts-license.html`, shipped with the web assets by the camera
package builders. No player UI link is required to retain these notices.
When updating the bundle, recheck its source map and upstream licences and
update these notices and the pinned checksum together. The bundled JavaScript
must retain its existing notices; any local source changes need an explicit
modification notice as required by Apache-2.0 section 4(b).
