# Optional VPN on ZT30 and MT11

In **Parameters → Network**, select **VPN enable**, keep **ZeroTier** as the
provider, enter the 16-digit network ID and save. Authorize the displayed node
ID in ZeroTier Central. The status shows connection progress and assigned IPs.
An Internet gateway must already be available; VPN does not configure the LAN.

VPN defaults to off. Save with VPN disabled to stop the client and remove its
interface and routes, without restarting camera-app. If disabling through the
VPN, reconnect to the LAN address. The camera's identity is retained for the
next enable. Changing the network ID replaces the previous membership. Only
managed private routes are accepted; default, global and DNS routes are disabled.
The provider setting currently accepts `zerotier`; future providers can add their
own settings without changing the enable switch.

```ini
[network]
vpn_enabled=false
vpn_provider=zerotier
vpn_network_id=
```

The authenticated web service supervises the VPN independently of camera-app.
Its worker owns and reaps the ZeroTier child, stops it on disable or web exit,
and restarts it after a client crash. The web listener replaces a failed worker;
Linux parent-death signals stop clients if their worker is killed. A singleton
lock prevents concurrent owners.
SITL/host tests never start a real client or change the host network.

The web executable embeds the checked-in client and module, so ordinary release
packages and ZT30 application bundles carry the complete feature. Enable extracts
them into the private runtime directory (`/run/vpn` on MT11,
`/tmp/apcg-zt30/vpn` on ZT30). No extra persistent copy of those binaries is
created. Only identity, membership and local settings persist under
`/app/vpn/state` or `/customer/camera-app/vpn/state`. Secrets stay private to root
and are never returned by the status endpoint. Disabled VPN does not extract the
runtime. Logs on tmpfs are bounded to 1 MiB.

| Platform | ZeroTier client | TUN module | Embedded payload |
| --- | ---: | ---: | ---: |
| ZT30 | 1,251,520 bytes | 31,636 bytes | 1,283,156 bytes |
| MT11 | 1,580,512 bytes | 59,088 bytes | 1,639,600 bytes |

These are uncompressed bytes in each web executable, before filesystem/package
compression, plus the small supervisor and UI change. An active and retained
ZT30 slot each carry their own web executable. Running VPN also uses tmpfs for
the extracted tools and the client's normal memory.

## Qualified kernels

The ZT30 module targets Linux 4.9.227 #112 (ARMv7 Thumb2); the MT11 module targets
Linux 4.19.90 #1, 13 June 2025 (ARM64). A manifest pins the measured kernel region
by SHA-256. Before loading a missing driver, the worker checks that region and
refuses incompatible kernels, showing an error in the Network tab. It never
replaces a kernel or forces a module load. A built-in/already registered TUN
implementation needs no external module.

The MT11 check hashes the Linux Image inside the vendor wrapper, excluding
unused flash padding. It matches `packaging/mt11/base/kernel` as well as the
captured camera. The ZT30 check covers its captured kernel partition. These
modules are not advertised for other vendor kernel builds.

Both are built from pinned upstream sources with reconstructed configurations,
measured ABI assertions and explicit target export allowlists. The compatibility
patches avoid vendor-private task structures. MT11 uses the generic SKB allocator
in place of the task-fragment fast path, and reserves the measured vendor device
tail in `net_device`. TUN and TAP were tested in isolated network namespaces:
300 IPv4/ICMP round trips each, payloads 32/1400/2700 bytes, ARP, owner checks,
multiple queues and async notifications. `tun_smoke.c` is the common probe.

## Rebuilding the pinned tools

ZeroTier One 1.16.2 source:
<https://github.com/zerotier/ZeroTierOne/tree/1.16.2>
The build script pins the source tarball SHA-256 and disables the controller and
SSO. C++/GCC runtimes are bundled; libc uses the camera's installed ABI. Tested
with the project ARMv7 glibc 2.30 toolchain for ZT30 and Linaro AArch64 GCC 7.5 for
MT11. Building a fully static glibc client is unsuitable on ZT30.

```sh
tools/build_zerotier.sh zt30 build/vpn/zt30 /path/to/arm-linux-
tools/build_zerotier.sh mt11 build/vpn/mt11 /path/to/aarch64-linux-gnu-
CROSS_COMPILE=arm-linux-gnueabihf- packaging/vpn/zt30/build.sh /path/to/zt30-kernel-build
CROSS_COMPILE=/path/to/aarch64-linux-gnu- packaging/vpn/mt11/build.sh /path/to/mt11-kernel-build
```

Module sources are upstream Linux 4.9.227 and 4.19.90 from kernel.org. Their
archives are SHA-256 pinned by the scripts. ZT30 was built using GCC 10.5;
MT11 using Linaro GCC 7.5. Preserve the checked-in defconfig, patches, assertions
and exported-interface checks. Requalify changed modules on the matching camera
before updating the compressed artifact and its size/hash in `manifest.json`.
`tools/build_vpn_resources.py` verifies both artifacts before embedding them.

Client licensing is MPL-2.0 with third-party notices under `licenses/`; the TUN
driver and compatibility patches are GPL-2.0. License texts are included here.
The pinned upstream tarball provides the complete corresponding client sources;
the kernel tarballs plus the local patches provide the module sources.
