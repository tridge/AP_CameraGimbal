.DEFAULT_GOAL := help
include tools/build.mk

# Written by tools/install_build_environment.py; command-line overrides win.
-include build/environment.mk

# Only these are offered by tab completion. Internal steps start with '_';
# tests, probes and other developer targets are in tools/dev.mk.
CAMERA_TARGETS := a8 mt11 zr10 zt30 z1mini
PUBLIC_TARGETS := help $(CAMERA_TARGETS) rpi_libcam_caddx release dependencies clean \
	sitl sitl-run sitl-test sitl-kill
.PHONY: $(PUBLIC_TARGETS)

# Check before any recipes run, including parallel release builds. Host tests,
# SITL, source downloads and clean targets do not need hardware toolchains.
HARDWARE_BUILD_GOALS := release $(CAMERA_TARGETS) _mt11-binaries _zr10_dependencies \
	_z1mini-binaries z1mini_retained_isp_package packaging/mt11/thermal_socket
ifneq ($(filter $(HARDWARE_BUILD_GOALS),$(MAKECMDGOALS)),)
ifeq ($(wildcard build/environment.mk),)
$(error Build environment is not configured in this checkout. Run 'python3 tools/install_build_environment.py' first)
endif
endif

CROSS_COMPILE ?= aarch64-linux-gnu-
DEPS_ROOT ?= $(CURDIR)/build/deps
SS928_MPP_ROOT ?= $(abspath $(DEPS_ROOT)/ss928-mpp)
MINIMP4_ROOT ?= $(abspath $(DEPS_ROOT)/minimp4)
DLIB_ROOT ?= $(abspath $(DEPS_ROOT)/dlib)
MT11_KERNEL ?= packaging/mt11/base/kernel
MT11_ROOTFS ?= packaging/mt11/base/rootfs
MT11_UPDATE_CONFIG ?= packaging/mt11/base/config.json
MT11_VERSION ?= $(shell git tag --merged HEAD --sort=-version:refname 2>/dev/null | awk '/^v[0-9]+\.[0-9]+(\.[0-9]+)?$$/ { print; exit }')
MT11_GIT_HASH ?= $(shell git rev-parse HEAD 2>/dev/null | cut -c1-6)
export MT11_VERSION MT11_GIT_HASH
SITL_VIDEO_PYTHON ?= $(if $(wildcard $(CURDIR)/build/terrain-venv/bin/python),$(CURDIR)/build/terrain-venv/bin/python,python3)
export CAMERA_GIMBAL_SITL_PYTHON ?= $(SITL_VIDEO_PYTHON)
# Versioned, user-facing packages. Each camera build uses the inherited
# jobserver; the release publisher installs completed packages one at a time.
RELEASE_ROOT ?= release
RELEASE_TARGETS ?= A8 MT11 ZR10 ZT30 Z1-Mini
ZT30_RUNTIME_ROOT ?= $(CURDIR)/packaging/zt30/platform
ZT30_PACKAGE_OUT ?= build/ZT30_AP_$(MT11_VERSION)_$(MT11_GIT_HASH).apbundle
ZT30_FIRMWARE_OUT ?= build/SIYI_ZT30_UpgradeSD.bin
ZT30_RELEASE_TOOLS ?= build/deps/zt30-release-tools
MT11_PACKAGE_OUT ?= build/MT11_FW_ArduPilot_$(MT11_VERSION)_$(MT11_GIT_HASH).bin
MT11_WEB_PASSWORD ?= ardupilot
MT11_ROOT_PASSWORD ?= ardupilot
MT11_ROOT_PASSWORD_HASH_FILE ?=
MT11_TOOLS_ROOT ?= $(DEPS_ROOT)/mt11-tools
MT11_RSYNC := $(MT11_TOOLS_ROOT)/bin/rsync
MT11_STRACE := $(MT11_TOOLS_ROOT)/bin/strace
MT11_TCPDUMP := $(MT11_TOOLS_ROOT)/bin/tcpdump
MT11_LTRACE := $(MT11_TOOLS_ROOT)/bin/ltrace
MT11_DROPBEAR := $(MT11_TOOLS_ROOT)/bin/dropbear
MT11_DROPBEARKEY := $(MT11_TOOLS_ROOT)/bin/dropbearkey
A8_CROSS_COMPILE ?= arm-linux-
# Root of the ARMv7 hard-float glibc 2.30 toolchain (its bin/ is added to
# PATH); alternatively give A8_CROSS_COMPILE as a full path prefix.
A8_TOOLCHAIN_DIR ?=
ifneq ($(A8_TOOLCHAIN_DIR),)
export PATH := $(A8_TOOLCHAIN_DIR)/bin:$(PATH)
endif
A8_PLATFORM_DIR ?= packaging/a8/platform
A8_PACKAGE_OUT ?= build/A8_FW_ArduPilot_$(MT11_VERSION)_$(MT11_GIT_HASH).bin
A8_WEB_PASSWORD ?= ardupilot
export A8_WEB_PASSWORD
SUPPORTPROXY_ROOT ?= $(abspath ../SupportProxy)
SITL_BUILD := build/sitl
A8_SITL_BUILD := build/a8-sitl
ZT30_SITL_BUILD := build/zt30-sitl
export MT11_WEB_PASSWORD
export MT11_ROOT_PASSWORD
export MT11_ROOT_PASSWORD_HASH_FILE
export SS928_MPP_ROOT
export MINIMP4_ROOT
export DLIB_ROOT

# Recursive Make inherits exported roots as environment variables. Compare
# paths, not variable origins, so release builds still fetch default sources.
# Callers supplying both custom roots manage those source trees themselves.
BOOTSTRAP_DEPENDENCIES := $(filter $(abspath $(DEPS_ROOT)/ss928-mpp) $(abspath $(DEPS_ROOT)/minimp4),\
	$(abspath $(SS928_MPP_ROOT)) $(abspath $(MINIMP4_ROOT)))

help:
	@echo 'Camera firmware, written to build/ (the same files as "make release"):'
	@echo '  make a8 | mt11 | zr10 | zt30 | z1mini'
	@echo '  make rpi_libcam_caddx   native Raspberry Pi build'
	@echo '  make release            all cameras, into $(RELEASE_ROOT)/<version>/'
	@echo 'Simulators, CAMERA=mt11|a8|zr10|z1mini|zt30 (default mt11):'
	@echo '  make sitl               build; CAMERA may be a list or "all"'
	@echo '  make sitl-run           build and run one simulator'
	@echo '  make sitl-test          build and test'
	@echo '  make sitl-kill          stop simulators left running from this checkout'
	@echo 'Other:'
	@echo '  make dependencies       fetch pinned build sources'
	@echo '  make clean'
	@echo 'Tests and developer tools: make -f tools/dev.mk <target>'

.PHONY: _generated _mt11-binaries _mt11-camera-app _mt11-web-app
# Finish shared generators before backend submakes start. A multi-camera
# invocation must have one owner for every generated header and patched source.
_generated: _build-dependencies
	$(MAKE) -C camera_app generated
	$(MAKE) -C web generated

_mt11-binaries: _mt11-camera-app _mt11-web-app
_mt11-camera-app: _generated
	$(MAKE) -C camera_app
_mt11-web-app: _generated
	$(MAKE) -C web

.PHONY: _camera-definitions
_camera-definitions:
	python3 tools/build_camera_definitions.py

release: _camera-definitions
	+python3 tools/build_release.py --make '$(MAKE)' --version '$(MT11_VERSION)' \
		--output '$(RELEASE_ROOT)' --targets $(RELEASE_TARGETS)

.PHONY: _build-dependencies _mavlink-dependencies _bootstrap-dependencies _mt11_tools
_mavlink-dependencies:
	@if test ! -f modules/mavlink/message_definitions/v1.0/all.xml || \
		test ! -f modules/mavlink/pymavlink/tools/mavgen.py; then \
		git submodule update --init --recursive; \
	fi

_build-dependencies: _mavlink-dependencies $(if $(BOOTSTRAP_DEPENDENCIES),_bootstrap-dependencies)

dependencies: _bootstrap-dependencies

_bootstrap-dependencies: _mavlink-dependencies
	tools/bootstrap_dependencies.sh '$(DEPS_ROOT)'

# Verify on every invocation: also repair deleted/corrupt cached binaries.
# No cross compiler, source download or build-environment setup is needed.
_mt11_tools:
	python3 tools/prebuilt_mt11_tools.py --output '$(MT11_TOOLS_ROOT)'

# Simulators. Every camera's simulator has its own build directory.
SITL_CAMERAS := mt11 a8 zr10 z1mini zt30
CAMERA ?= mt11
SITL_SELECTED := $(if $(filter-out all,$(CAMERA)),$(CAMERA),$(if $(CAMERA),$(SITL_CAMERAS)))
ifneq ($(filter sitl sitl-run sitl-test,$(MAKECMDGOALS)),)
ifneq ($(if $(SITL_SELECTED),,empty)$(filter-out $(SITL_CAMERAS),$(SITL_SELECTED)),)
$(error Unknown CAMERA '$(CAMERA)': use one or more of $(SITL_CAMERAS), or all)
endif
ifneq ($(filter sitl-run,$(MAKECMDGOALS)),)
ifneq ($(words $(SITL_SELECTED)),1)
$(error sitl-run needs exactly one CAMERA)
endif
endif
endif
.PHONY: $(addprefix _sitl-,$(SITL_CAMERAS)) $(addprefix _sitl-test-,$(SITL_CAMERAS)) _targets

sitl: $(addprefix _sitl-,$(SITL_SELECTED))

sitl-test: $(addprefix _sitl-test-,$(SITL_SELECTED))

sitl-run: _sitl-$(firstword $(SITL_SELECTED))
	$(SITL_RUN_ENV_$(firstword $(SITL_SELECTED))) sh sitl/run.sh

SITL_RUN_ENV_mt11 = CAMERA_GIMBAL_SITL_BACKEND=mt11 CAMERA_GIMBAL_SITL_BUILD=$(abspath $(SITL_BUILD))

sitl-kill:
	python3 sitl/kill.py $(CURDIR) $(abspath $(SITL_BUILD)/runtime)

_targets:
	python3 tools/export_targets.py

$(addprefix _sitl-,$(SITL_CAMERAS)): _targets

_sitl-mt11: _generated $(SITL_BUILD)/rgb.h264 $(SITL_BUILD)/thermal.h264 \
	$(SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl SITL_TARGET=$(abspath $(SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(SITL_BUILD)/mt11-web) \
		SITL_ROOT=$(abspath $(SITL_BUILD)/runtime) SITL_BIN_ROOT=$(abspath $(SITL_BUILD))
	python3 sitl/prepare_runtime.py $(SITL_BUILD)/runtime camera_app/camera.ini
	@echo "MT11 SITL built. Run 'make sitl-run' to start it."

$(SITL_BUILD)/rgb.h264: Makefile
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi \
		-i testsrc2=size=1920x1080:rate=10 -t 2 -an -c:v libx264 \
		-preset ultrafast -tune zerolatency -pix_fmt yuv420p \
		-x264-params keyint=10:min-keyint=10:scenecut=0:repeat-headers=1:aud=1 \
		-f h264 -y $@

$(SITL_BUILD)/thermal.h264: Makefile
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi \
		-i testsrc=size=1280x720:rate=10 -vf format=gray -t 2 -an \
		-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
		-x264-params keyint=10:min-keyint=10:scenecut=0:repeat-headers=1:aud=1 \
		-f h264 -y $@

$(SITL_BUILD)/photo.jpg:
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi \
		-i testsrc2=size=640x360:rate=1 -frames:v 1 -y $@

_sitl-test-mt11: _sitl-mt11
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation upright
	python3 sitl/test_unigcs.py $(SITL_BUILD)/camera-app --video-codec h264
	python3 sitl/test_unigcs.py $(SITL_BUILD)/camera-app --video-codec h265
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation inverted

$(A8_SITL_BUILD)/photo.jpg: $(SITL_BUILD)/photo.jpg
	mkdir -p $(dir $@)
	cp $< $@

$(A8_SITL_BUILD)/main.h264:
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi \
		-i testsrc2=size=1920x1080:rate=25 -t 2 -an -c:v libx264 \
		-preset ultrafast -tune zerolatency -pix_fmt yuv420p \
		-x264-params keyint=25:min-keyint=25:scenecut=0:repeat-headers=1:aud=1 \
		-f h264 -y $@

$(A8_SITL_BUILD)/sub.h264:
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi \
		-i testsrc2=size=1280x720:rate=25 -t 2 -an -c:v libx264 \
		-preset ultrafast -tune zerolatency -pix_fmt yuv420p \
		-x264-params keyint=25:min-keyint=25:scenecut=0:repeat-headers=1:aud=1 \
		-f h264 -y $@

_sitl-a8: _generated $(A8_SITL_BUILD)/main.h264 $(A8_SITL_BUILD)/sub.h264 \
	$(A8_SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=a8 \
		SITL_OBJDIR=build/a8-sitl \
		SITL_TARGET=$(abspath $(A8_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(A8_SITL_BUILD)/a8-web) \
		SITL_ROOT=$(abspath $(A8_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(A8_SITL_BUILD)) SITL_WEB_BINARY=a8-web \
		CAMERA_BACKEND=a8
	python3 sitl/prepare_runtime.py $(A8_SITL_BUILD)/runtime packaging/a8/camera.ini
	@echo "A8 SITL built. Run 'make sitl-run CAMERA=a8' to start it."
SITL_RUN_ENV_a8 = CAMERA_GIMBAL_SITL_BACKEND=a8 CAMERA_GIMBAL_SITL_BUILD=$(abspath $(A8_SITL_BUILD))

_sitl-test-a8: _sitl-a8
	python3 sitl/test_unigcs.py $(A8_SITL_BUILD)/camera-app --backend a8 --video-codec h265
	python3 sitl/test_unigcs.py $(A8_SITL_BUILD)/camera-app --backend a8 --orientation inverted
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation upright
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation inverted

packaging/mt11/thermal_socket: packaging/mt11/thermal_socket.cpp
	$(call progress,Linking $@)
	$(CROSS_COMPILE)g++ -static -O2 -pipe -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers \
		-o $@ $<

mt11: _mt11-binaries packaging/mt11/thermal_socket \
	packaging/mt11/mt11-timesync.sh packaging/mt11/app_init.sh \
	packaging/mt11/app_selection.sh packaging/mt11/start-dropbear.sh \
	_mt11_tools
	@printf '%s\n' '$(MT11_VERSION)' | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$$' || { \
		echo 'No reachable Git tag of the form vX.y or vX.y.z; cannot name package' >&2; exit 1; \
	}
	@printf '%s\n' '$(MT11_GIT_HASH)' | grep -Eq '^[0-9a-f]{6}$$' || { \
		echo 'Cannot determine the six-character Git commit hash' >&2; exit 1; \
	}
	tools/build_mt11_package.sh \
		'$(MT11_KERNEL)' '$(MT11_ROOTFS)' '$(MT11_UPDATE_CONFIG)' \
		'$(MT11_PACKAGE_OUT)' \
		camera_app/camera-app web/mt11-web \
		packaging/mt11/thermal_socket packaging/mt11/mt11-timesync.sh \
		packaging/mt11/app_init.sh packaging/mt11/app_selection.sh \
		camera_app/camera.ini \
		'$(MT11_RSYNC)' '$(MT11_STRACE)' '$(MT11_TCPDUMP)' \
		'$(MT11_LTRACE)' \
		'$(MT11_DROPBEAR)' '$(MT11_DROPBEARKEY)' \
		packaging/mt11/start-dropbear.sh

# SIYI A8 mini SD-card update: camera-app replaces the vendor application
# partition. Needs the arm-linux- (armv7 hard-float, glibc <= 2.30) toolchain
# on PATH and mkfs.jffs2. Platform assets are checked in separately.
a8: _generated build/a8-uuid
	@printf '%s\n' '$(MT11_VERSION)' | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$$' || { \
		echo 'No reachable Git tag of the form vX.y or vX.y.z; cannot name package' >&2; exit 1; \
	}
	@'$(A8_CROSS_COMPILE)gcc' -dumpmachine >/dev/null 2>&1 || { \
		echo '$(A8_CROSS_COMPILE)gcc not found: run python3 tools/install_build_environment.py --targets a8' >&2; \
		echo 'or run make a8 A8_TOOLCHAIN_DIR=<toolchain root> (or A8_CROSS_COMPILE=<dir>/arm-linux-)' >&2; \
		exit 1; \
	}
	$(MAKE) -C camera_app CAMERA_BACKEND=a8 CROSS_COMPILE=$(A8_CROSS_COMPILE)
	$(MAKE) -C web a8-web A8_CROSS_COMPILE=$(A8_CROSS_COMPILE)
	CROSS_COMPILE=$(A8_CROSS_COMPILE) tools/build_a8_package.sh \
		'$(A8_PLATFORM_DIR)' '$(A8_PACKAGE_OUT)' \
		camera_app/build/a8/camera-app build/a8-uuid web/a8-web \
		packaging/a8/app_init.sh packaging/a8/camera.ini \
		packaging/a8/upgrade_script.txt '$(MT11_VERSION)-$(MT11_GIT_HASH)'

build/a8-uuid: packaging/a8/a8-uuid.cpp Makefile
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(A8_CROSS_COMPILE)g++ -static-libstdc++ -static-libgcc -O2 -pipe -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers \
		-o $@ $< -ldl

clean:
	$(MAKE) -C camera_app clean
	$(MAKE) -C web clean
	rm -f packaging/mt11/thermal_socket
	rm -f '$(MT11_PACKAGE_OUT)' '$(A8_PACKAGE_OUT)' '$(A8_PACKAGE_OUT).sha256'
	rm -rf $(SITL_BUILD) $(A8_SITL_BUILD) $(ZR10_SITL_BUILD) $(Z1MINI_SITL_BUILD)

# ZR10 application bundle: SD-based installation, no partition image.
ZR10_CROSS_COMPILE ?= $(CURDIR)/build/zr10-deps/armv7-eabihf--uclibc--stable-2018.11-1/bin/arm-linux-
ZR10_WEB_PASSWORD ?= ardupilot
ZR10_BUILD_HASH := $(MT11_GIT_HASH)$(shell git diff --quiet HEAD -- || printf -- -dirty)
ZR10_PACKAGE_OUT ?= build/ZR10_APP_ArduPilot_$(MT11_VERSION)_$(ZR10_BUILD_HASH).tar.gz
export ZR10_WEB_PASSWORD
ZR10_PLATFORM_DIR ?= $(CURDIR)/packaging/zr10/platform
ZR10_FIRMWARE_OUT ?= build/ZR10_FW_ArduPilot_$(MT11_VERSION)_$(ZR10_BUILD_HASH).bin
.PHONY: _zr10_dependencies
_zr10_dependencies: _build-dependencies
	tools/bootstrap_zr10_toolchain.sh

build/zr10-uuid: packaging/zr10/zr10-uuid.cpp Makefile | _zr10_dependencies
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(ZR10_CROSS_COMPILE)g++ -static-libstdc++ -static-libgcc -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers \
		-o $@ $< -ldl

# Application bundle for SD installation, then the flashable customer
# partition retaining required vendor platform assets.
zr10: _zr10_dependencies _generated build/zr10-uuid
	$(MAKE) -C camera_app CAMERA_BACKEND=zr10 CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'
	$(MAKE) -C web zr10-web ZR10_CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'
	ZR10_CROSS_COMPILE='$(ZR10_CROSS_COMPILE)' tools/build_zr10_package.sh \
		'$(ZR10_PACKAGE_OUT)' camera_app/build/zr10/camera-app web/zr10-web \
		'$(MT11_VERSION)-$(ZR10_BUILD_HASH)-experimental' build/zr10-uuid
	tools/build_zr10_firmware.sh '$(ZR10_PLATFORM_DIR)' '$(ZR10_PACKAGE_OUT)' '$(ZR10_FIRMWARE_OUT)'

# ZR10 shares the A8 MCU protocol but uses its own control routing and optics.
ZR10_SITL_BUILD ?= build/zr10-sitl
$(ZR10_SITL_BUILD)/main.h264: $(A8_SITL_BUILD)/main.h264
	mkdir -p $(dir $@)
	cp $< $@
$(ZR10_SITL_BUILD)/sub.h264: $(A8_SITL_BUILD)/sub.h264
	mkdir -p $(dir $@)
	cp $< $@
$(ZR10_SITL_BUILD)/photo.jpg: $(A8_SITL_BUILD)/photo.jpg
	mkdir -p $(dir $@)
	cp $< $@
_sitl-zr10: _generated $(ZR10_SITL_BUILD)/main.h264 $(ZR10_SITL_BUILD)/sub.h264 $(ZR10_SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=zr10 \
		SITL_OBJDIR=build/zr10-sitl \
		SITL_TARGET=$(abspath $(ZR10_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(ZR10_SITL_BUILD)/zr10-web) \
		SITL_ROOT=$(abspath $(ZR10_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(ZR10_SITL_BUILD)) SITL_WEB_BINARY=zr10-web \
		CAMERA_BACKEND=zr10
	python3 sitl/prepare_runtime.py $(ZR10_SITL_BUILD)/runtime sitl/zr10.ini
	@echo "ZR10 SITL built. Run 'make sitl-run CAMERA=zr10' to start it."
SITL_RUN_ENV_zr10 = CAMERA_GIMBAL_SITL_BACKEND=zr10 CAMERA_GIMBAL_SITL_BUILD=$(abspath $(ZR10_SITL_BUILD))
_sitl-test-zr10: _sitl-zr10
	python3 sitl/test_unigcs.py $(ZR10_SITL_BUILD)/camera-app --backend zr10 --video-codec h265
	python3 sitl/test_sitl.py $(ZR10_SITL_BUILD)/camera-app \
		$(ZR10_SITL_BUILD)/zr10-web sitl/gimbal_sim.py \
		$(abspath $(ZR10_SITL_BUILD)/runtime) --backend zr10 --orientation upright
	python3 sitl/test_sitl.py $(ZR10_SITL_BUILD)/camera-app \
		$(ZR10_SITL_BUILD)/zr10-web sitl/gimbal_sim.py \
		$(abspath $(ZR10_SITL_BUILD)/runtime) --backend zr10 --orientation inverted

# XFRobot Z1-Mini application overlay (AX620A ARMv7 hard-float/glibc).
# Build/package only: these targets never contact or install on a camera.
Z1MINI_CROSS_COMPILE ?= arm-none-linux-gnueabihf-
Z1MINI_BUILD_HASH := $(MT11_GIT_HASH)$(shell git diff --quiet HEAD -- || printf -- -dirty)
Z1MINI_PACKAGE_OUT ?= build/Z1Mini_AP_$(MT11_VERSION)_$(Z1MINI_BUILD_HASH).gcu
Z1MINI_RETAINED_ISP_PACKAGE_OUT ?= $(Z1MINI_PACKAGE_OUT)
# Optional native capture: independent 1080p live video and 4K recording.
Z1MINI_NATIVE_PACKAGE_OUT ?= build/Z1Mini_AP_native_$(MT11_VERSION)_$(Z1MINI_BUILD_HASH).gcu
.PHONY: _z1mini-binaries
_z1mini-binaries: _generated
	$(MAKE) -C camera_app CAMERA_BACKEND=z1mini CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'
	$(MAKE) -C web z1mini-web Z1MINI_CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'

# The self-contained native-capture variant is the deployable package.
z1mini: _z1mini-binaries
	$(MAKE) -C camera_app CAMERA_BACKEND=z1mini CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)' z1mini-capture Z1MINI_AX_SDK_INCLUDE='$(Z1MINI_AX_SDK_INCLUDE)'
	python3 tools/build_z1mini_package.py --native-capture camera_app/build/z1mini/ax-capture '$(Z1MINI_NATIVE_PACKAGE_OUT)'

# Raspberry Pi + libcamera + CADDX gimbal: native build on Raspberry Pi OS
# against the system libcamera; see packaging/rpi_libcam_caddx/README.md.
RPI_CROSS_COMPILE ?=
.PHONY: rpi_libcam_caddx
rpi_libcam_caddx:
	$(MAKE) -C camera_app CAMERA_BACKEND=rpi_libcam_caddx CROSS_COMPILE='$(RPI_CROSS_COMPILE)'
	$(MAKE) -C web CAMERA_BACKEND=rpi_libcam_caddx RPI_CROSS_COMPILE='$(RPI_CROSS_COMPILE)' rpi_libcam_caddx-web

Z1MINI_SITL_BUILD ?= build/z1mini-sitl
$(Z1MINI_SITL_BUILD)/main.h264: $(A8_SITL_BUILD)/main.h264
	mkdir -p $(dir $@)
	cp $< $@
$(Z1MINI_SITL_BUILD)/sub.h264: $(A8_SITL_BUILD)/sub.h264
	mkdir -p $(dir $@)
	cp $< $@
$(Z1MINI_SITL_BUILD)/photo.jpg: $(A8_SITL_BUILD)/photo.jpg
	mkdir -p $(dir $@)
	cp $< $@
_sitl-z1mini: _generated $(Z1MINI_SITL_BUILD)/main.h264 $(Z1MINI_SITL_BUILD)/sub.h264 $(Z1MINI_SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=z1mini \
		SITL_OBJDIR=build/z1mini-sitl \
		SITL_TARGET=$(abspath $(Z1MINI_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(Z1MINI_SITL_BUILD)/z1mini-web) \
		SITL_ROOT=$(abspath $(Z1MINI_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(Z1MINI_SITL_BUILD)) SITL_WEB_BINARY=z1mini-web \
		CAMERA_BACKEND=z1mini
	python3 sitl/prepare_runtime.py $(Z1MINI_SITL_BUILD)/runtime packaging/z1mini/camera.ini
	@echo "Z1MINI SITL built. Run 'make sitl-run CAMERA=z1mini' to start it."
SITL_RUN_ENV_z1mini = CAMERA_GIMBAL_SITL_BACKEND=z1mini CAMERA_GIMBAL_SITL_BUILD=$(abspath $(Z1MINI_SITL_BUILD))
_sitl-test-z1mini: _sitl-z1mini
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation upright
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation inverted

_sitl-zt30: _generated $(ZT30_SITL_BUILD)/rgb.h264 $(ZT30_SITL_BUILD)/thermal.h264 $(ZT30_SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=zt30 SITL_OBJDIR=build/zt30-sitl SITL_TARGET=$(abspath $(ZT30_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl CAMERA_BACKEND=zt30 SITL_TARGET=$(abspath $(ZT30_SITL_BUILD)/zt30-web) SITL_WEB_BINARY=zt30-web SITL_ROOT=$(abspath $(ZT30_SITL_BUILD)/runtime) SITL_BIN_ROOT=$(abspath $(ZT30_SITL_BUILD))
	python3 sitl/prepare_runtime.py $(ZT30_SITL_BUILD)/runtime packaging/zt30/camera.ini

$(ZT30_SITL_BUILD)/rgb.h264: $(SITL_BUILD)/rgb.h264
	mkdir -p $(dir $@)
	cp $< $@

$(ZT30_SITL_BUILD)/photo.jpg: $(SITL_BUILD)/photo.jpg
	mkdir -p $(dir $@)
	cp $< $@

$(ZT30_SITL_BUILD)/thermal.h264:
	mkdir -p $(dir $@)
	ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=640x512:rate=15 -vf format=gray -t 2 -an -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -x264-params keyint=15:min-keyint=15:scenecut=0:repeat-headers=1:aud=1 -f h264 -y $@

SITL_RUN_ENV_zt30 = CAMERA_GIMBAL_SITL_BACKEND=zt30 CAMERA_GIMBAL_SITL_BUILD=$(abspath $(ZT30_SITL_BUILD))

_sitl-test-zt30: _sitl-zt30 _camera-definitions
	python3 sitl/test_zt30.py --build $(ZT30_SITL_BUILD)
	python3 sitl/test_zt30.py --build $(ZT30_SITL_BUILD) --orientation upright --no-version-reply

ZT30_CROSS_COMPILE ?= $(A8_CROSS_COMPILE)
ZT30_SDK_LIBDIR ?= $(ZT30_RUNTIME_ROOT)/config/lib
ZT30_SDK_LIBRARIES := $(wildcard $(ZT30_SDK_LIBDIR)/*.so)
build/zt30/encoder-sdk.so: tools/zt30_rgb_sdk.c $(ZT30_SDK_LIBRARIES)
	@test -n '$(ZT30_SDK_LIBDIR)' || { echo 'ZT30_SDK_LIBDIR must point to the captured ZT30 /config/lib' >&2; exit 1; }
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared $< -L'$(ZT30_SDK_LIBDIR)' -Wl,--no-as-needed -lcam_os_wrapper -lmi_sys -lmi_sensor -lmi_vif -lispalgo -lcus3a -lmi_isp -lmi_scl -lmi_venc -lmi_rgn -lm -lpthread -Wl,--as-needed -o $@
	python3 tools/check_zt30_elf.py $@ --shared-library --readelf $(ZT30_CROSS_COMPILE)readelf

# Development application: continuous RGB, normal frontends, no flash installer.
.PHONY: _zt30-dev-camera _zt30-dev-camera-build
_zt30-dev-camera: build/zt30/encoder-sdk.so _zt30-dev-camera-build
_zt30-dev-camera-build: _generated
	$(MAKE) -C camera_app CAMERA_BACKEND=zt30 ZT30_DEV_BUILD=1 CROSS_COMPILE=$(ZT30_CROSS_COMPILE) all

build/zt30/zt30-slot: tools/zt30_slots.cpp include/apcam/zt30_slots.h include/apcam/zt30_bundle.h include/apcam/zt30_bundle_elf.h include/apcam/sha256.h include/apcam/json_reader.h include/apcam/zt30_update.h include/apcam/zt30_trial.h include/apcam/zt30_services.h include/apcam/zt30_process.h include/apcam/live_video_probe.h tools/check_zt30_elf.py
	mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ -static-libgcc -Iinclude $< -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: _zt30_release_tools
_zt30_release_tools:
	+sh tools/build_zt30_release_tools.sh '$(ZT30_RELEASE_TOOLS)' '$(ZT30_CROSS_COMPILE)'

# The firmware file supports stock microSD boot and the AP web slot updater.
# The bundle inside it is also written to ZT30_PACKAGE_OUT, as the bridge for
# web versions predating this container. Runtime libraries are build inputs
# and pins; the bundle never replaces them.
zt30: _generated build/zt30/zt30-slot _zt30_release_tools
	cd '$(ZT30_RUNTIME_ROOT)' && sha256sum --check SHA256SUMS
	$(MAKE) _zt30-dev-camera
	$(MAKE) -C web zt30-web ZT30_CROSS_COMPILE=$(ZT30_CROSS_COMPILE)
	python3 tools/build_zt30_app_bundle.py --runtime-root '$(ZT30_RUNTIME_ROOT)' \
		--readelf '$(ZT30_CROSS_COMPILE)readelf' \
		--version '$(MT11_VERSION)_$(MT11_GIT_HASH)' --output '$(ZT30_PACKAGE_OUT)'
	python3 tools/build_zt30_firmware.py --bundle '$(ZT30_PACKAGE_OUT)' \
		--slot-tool build/zt30/zt30-slot --dropbear '$(ZT30_RELEASE_TOOLS)/bin/dropbear' \
		--dropbearkey '$(ZT30_RELEASE_TOOLS)/bin/dropbearkey' \
		--runtime-root '$(ZT30_RUNTIME_ROOT)' --readelf '$(ZT30_CROSS_COMPILE)readelf' \
		--output '$(ZT30_FIRMWARE_OUT)'
