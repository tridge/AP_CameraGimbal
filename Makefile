# Written by tools/install_build_environment.py; command-line overrides win.
-include build/environment.mk

# Check before any recipes run, including parallel release builds. Host tests,
# SITL, source downloads and clean targets do not need hardware toolchains.
HARDWARE_BUILD_GOALS := all release mt11_package _mt11_package \
	a8_package zr10 zr10_dependencies zr10_package zr10_firmware \
	z1mini z1mini_package z1mini_native_package z1mini_retained_isp_package packaging/mt11/thermal_socket
ifneq ($(filter $(HARDWARE_BUILD_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
ifeq ($(wildcard build/environment.mk),)
$(error Build environment is not configured in this checkout. Run 'python3 tools/install_build_environment.py' first)
endif
endif

CROSS_COMPILE ?= aarch64-linux-gnu-
DEPS_ROOT ?= $(CURDIR)/build/deps
SS928_MPP_ROOT ?= $(abspath $(DEPS_ROOT)/ss928-mpp)
MINIMP4_ROOT ?= $(abspath $(DEPS_ROOT)/minimp4)
MT11_KERNEL ?= packaging/mt11/base/kernel
MT11_ROOTFS ?= packaging/mt11/base/rootfs
MT11_UPDATE_CONFIG ?= packaging/mt11/base/config.json
MT11_VERSION ?= $(shell git tag --merged HEAD --sort=-version:refname 2>/dev/null | awk '/^v[0-9]+\.[0-9]+$$/ { print; exit }')
MT11_GIT_HASH ?= $(shell git rev-parse HEAD 2>/dev/null | cut -c1-6)
export MT11_VERSION MT11_GIT_HASH
SITL_VIDEO_PYTHON ?= $(if $(wildcard $(CURDIR)/build/terrain-venv/bin/python),$(CURDIR)/build/terrain-venv/bin/python,python3)
export CAMERA_GIMBAL_SITL_PYTHON ?= $(SITL_VIDEO_PYTHON)
# Versioned, user-facing packages. Build cameras serially because their
# native builds share generated MAVLink headers and some intermediate files.
RELEASE_ROOT ?= release
RELEASE_TARGETS ?= A8 MT11 ZR10 Z1-Mini
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
export MT11_WEB_PASSWORD
export MT11_ROOT_PASSWORD
export MT11_ROOT_PASSWORD_HASH_FILE
export SS928_MPP_ROOT
export MINIMP4_ROOT

# Recursive Make inherits exported roots as environment variables. Compare
# paths, not variable origins, so release builds still fetch default sources.
# Callers supplying both custom roots manage those source trees themselves.
BOOTSTRAP_DEPENDENCIES := $(filter $(abspath $(DEPS_ROOT)/ss928-mpp) $(abspath $(DEPS_ROOT)/minimp4),\
	$(abspath $(SS928_MPP_ROOT)) $(abspath $(MINIMP4_ROOT)))

.PHONY: all dependencies mt11_tools mt11_package a8_package sitl sitl-run sitl-kill sitl-test sitl-mavlink-test a8_sitl-mavlink-test \
	sitl-video-telemetry-test a8_sitl-video-telemetry-test \
	sitl-armed-recording-test a8_sitl-armed-recording-test \
	recording-recovery-test sitl-supportproxy-test a8_sitl-supportproxy-test \
	a8_sitl a8_sitl-run a8_sitl-test \
	ardupilot-siyi-test ardupilot-mavlink-test mavproxy-camera-test \
	sitl-clean clean _mt11_package

all: build-dependencies
	$(MAKE) -C camera_app
	$(MAKE) -C web

.PHONY: release release-test
.PHONY: camera-definitions camera-definition-test
.PHONY: safe-tar-test
safe-tar-test:
	python3 tests/test_safe_tar.py

camera-definitions:
	python3 tools/build_camera_definitions.py

camera-definition-test: camera-definitions
	python3 tests/test_camera_definition.py

release: camera-definitions
	+python3 tools/build_release.py --make '$(MAKE)' --version '$(MT11_VERSION)' \
		--output '$(RELEASE_ROOT)' --targets $(RELEASE_TARGETS)

release-test:
	python3 tests/test_release.py
	python3 tests/test_build_dependencies.py
	python3 tests/test_prebuilt_tools.py

.PHONY: platform-test
platform-test:
	python3 tests/test_siyi_uuid.py
	python3 tests/test_zr10_install.py
	python3 tests/test_platform_packages.py '$(RELEASE_ROOT)/$(MT11_VERSION)'

.PHONY: build-dependencies mavlink-dependencies
mavlink-dependencies:
	@if test ! -f modules/mavlink/message_definitions/v1.0/all.xml || \
		test ! -f modules/mavlink/pymavlink/tools/mavgen.py; then \
		git submodule update --init --recursive; \
	fi

build-dependencies: mavlink-dependencies
	$(if $(BOOTSTRAP_DEPENDENCIES),tools/bootstrap_dependencies.sh '$(DEPS_ROOT)',:)

dependencies: mavlink-dependencies
	tools/bootstrap_dependencies.sh '$(DEPS_ROOT)'

# Verify on every invocation: also repair deleted/corrupt cached binaries.
# No cross compiler, source download or build-environment setup is needed.
mt11_tools:
	python3 tools/prebuilt_mt11_tools.py --output '$(MT11_TOOLS_ROOT)'

sitl: $(SITL_BUILD)/rgb.h264 $(SITL_BUILD)/thermal.h264 \
	$(SITL_BUILD)/photo.jpg
	$(MAKE) build-dependencies
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

sitl-run: sitl
	sh sitl/run.sh

sitl-kill:
	python3 sitl/kill.py $(CURDIR) $(abspath $(SITL_BUILD)/runtime)

sitl-test: sitl
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation upright
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation inverted

sitl-mavlink-test: sitl
	python3 sitl/test_mavlink_parameters.py --backend mt11 --build $(SITL_BUILD)
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --build $(SITL_BUILD)

a8_sitl-mavlink-test: a8_sitl
	python3 sitl/test_mavlink_parameters.py --backend a8 --build $(A8_SITL_BUILD)

.PHONY: a8_sitl-rate-test
a8_sitl-rate-test: a8_sitl
	$(MAKE) -C camera_app tests/test_gimbal_rate
	./camera_app/tests/test_gimbal_rate
	$(SITL_VIDEO_PYTHON) sitl/test_rate_tracking.py --output $(A8_SITL_BUILD)/rate-test

.PHONY: sitl-angle-hold-test
.PHONY: sitl-live-tracking-test
sitl-live-tracking-test: sitl
	$(MAKE) -C web portable-sitl SITL_BIN_ROOT=$(abspath $(SITL_BUILD))
	$(MAKE) -C camera_app thermal-monitor-test
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_rates.py
	$(SITL_VIDEO_PYTHON) sitl/test_live_tracking.py --build $(SITL_BUILD)

sitl-angle-hold-test: sitl a8_sitl zr10_sitl
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend mt11 --build $(SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend mt11 --build $(SITL_BUILD) --orientation inverted
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend a8 --build $(A8_SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend a8 --build $(A8_SITL_BUILD) --orientation inverted
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend zr10 --build $(ZR10_SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend zr10 --build $(ZR10_SITL_BUILD) --orientation inverted

sitl-video-telemetry-test: sitl
	python3 sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --check-stale
	python3 sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --codec hevc

.PHONY: sitl-buffering-test
sitl-buffering-test: sitl
	python3 sitl/test_buffered_terrain.py --build $(SITL_BUILD)

.PHONY: sitl-terrain-test a8_sitl-terrain-test
SITL_TERRAIN_PYTHON ?= $(SITL_VIDEO_PYTHON)

.PHONY: sitl-image-controls-test
sitl-image-controls-test: sitl a8_sitl zr10_sitl z1mini_sitl
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend mt11
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend a8
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend zr10
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend z1mini
sitl-terrain-test: sitl
	$(SITL_TERRAIN_PYTHON) sitl/test_terrain_video.py
	$(SITL_TERRAIN_PYTHON) sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --terrain

a8_sitl-terrain-test: a8_sitl
	$(SITL_TERRAIN_PYTHON) sitl/test_terrain_video.py
	$(SITL_TERRAIN_PYTHON) sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD) --terrain

a8_sitl-video-telemetry-test: a8_sitl
	python3 sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD)
	python3 sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD) --codec hevc

sitl-armed-recording-test: sitl
	python3 sitl/test_armed_recording.py --backend mt11 --build $(SITL_BUILD)

a8_sitl-armed-recording-test: a8_sitl
	python3 sitl/test_armed_recording.py --backend a8 --build $(A8_SITL_BUILD)

sitl-supportproxy-test: sitl
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case signed --reconnect
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case session
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case hevc
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case disabled
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case video-only
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case single-video

a8_sitl-supportproxy-test: a8_sitl
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --backend a8 --build $(A8_SITL_BUILD)

recording-recovery-test:
	python3 web/tests/test_recording_download.py

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

a8_sitl: $(A8_SITL_BUILD)/main.h264 $(A8_SITL_BUILD)/sub.h264 \
	$(A8_SITL_BUILD)/photo.jpg
	$(MAKE) build-dependencies
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=a8 \
		SITL_OBJDIR=build/a8-sitl \
		SITL_TARGET=$(abspath $(A8_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(A8_SITL_BUILD)/a8-web) \
		SITL_ROOT=$(abspath $(A8_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(A8_SITL_BUILD)) SITL_WEB_BINARY=a8-web \
		CAMERA_BACKEND=a8
	python3 sitl/prepare_runtime.py $(A8_SITL_BUILD)/runtime packaging/a8/camera.ini
	@echo "A8 SITL built. Run 'make a8_sitl-run' to start it."

a8_sitl-run: a8_sitl
	CAMERA_GIMBAL_SITL_BACKEND=a8 \
		CAMERA_GIMBAL_SITL_BUILD=$(abspath $(A8_SITL_BUILD)) sh sitl/run.sh

a8_sitl-test: a8_sitl
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation upright
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation inverted

ardupilot-siyi-test:
	tests/run_ardupilot_siyi_test.sh

ardupilot-mavlink-test:
	tests/run_ardupilot_mavlink_test.sh

mavproxy-camera-test:
	@test -n "$(MAVPROXY_REPO)" || { \
		echo 'Set MAVPROXY_REPO to the MAVProxy worktree' >&2; exit 2; \
	}
	@test -f "$(MAVPROXY_REPO)/MAVProxy/modules/mavproxy_camera/__init__.py"
	MAVPROXY_REPO="$(abspath $(MAVPROXY_REPO))" \
		tests/run_ardupilot_mavlink_test.sh

sitl-clean:
	rm -rf build

packaging/mt11/thermal_socket: packaging/mt11/thermal_socket.c
	$(CROSS_COMPILE)gcc -static -O2 -pipe -Wall -Wextra -Werror -std=c11 \
		-o $@ $<

mt11_package:
	@printf '%s\n' '$(MT11_VERSION)' | grep -Eq '^v[0-9]+\.[0-9]+$$' || { \
		echo 'No reachable Git tag of the form vX.y; cannot name package' >&2; exit 1; \
	}
	@printf '%s\n' '$(MT11_GIT_HASH)' | grep -Eq '^[0-9a-f]{6}$$' || { \
		echo 'Cannot determine the six-character Git commit hash' >&2; exit 1; \
	}
	$(MAKE) build-dependencies
	$(MAKE) _mt11_package

_mt11_package: all packaging/mt11/thermal_socket \
	packaging/mt11/mt11-timesync.sh packaging/mt11/app_init.sh \
	packaging/mt11/app_selection.sh packaging/mt11/start-dropbear.sh \
	mt11_tools
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
a8_package:
	@printf '%s\n' '$(MT11_VERSION)' | grep -Eq '^v[0-9]+\.[0-9]+$$' || { \
		echo 'No reachable Git tag of the form vX.y; cannot name package' >&2; exit 1; \
	}
	@'$(A8_CROSS_COMPILE)gcc' -dumpmachine >/dev/null 2>&1 || { \
		echo '$(A8_CROSS_COMPILE)gcc not found: run python3 tools/install_build_environment.py --targets a8' >&2; \
		echo 'or run make a8_package A8_TOOLCHAIN_DIR=<toolchain root> (or A8_CROSS_COMPILE=<dir>/arm-linux-)' >&2; \
		exit 1; \
	}
	$(MAKE) build-dependencies
	$(MAKE) -C camera_app CAMERA_BACKEND=a8 CROSS_COMPILE=$(A8_CROSS_COMPILE)
	$(MAKE) -C web a8-web A8_CROSS_COMPILE=$(A8_CROSS_COMPILE)
	$(A8_CROSS_COMPILE)gcc -O2 -pipe -Wall -Wextra -Werror -std=c11 \
		-o build/a8-uuid packaging/a8/a8-uuid.c -ldl
	CROSS_COMPILE=$(A8_CROSS_COMPILE) tools/build_a8_package.sh \
		'$(A8_PLATFORM_DIR)' '$(A8_PACKAGE_OUT)' \
		camera_app/build/a8/camera-app build/a8-uuid web/a8-web \
		packaging/a8/app_init.sh packaging/a8/camera.ini \
		packaging/a8/upgrade_script.txt '$(MT11_VERSION)-$(MT11_GIT_HASH)'

clean:
	$(MAKE) -C camera_app clean
	$(MAKE) -C web clean
	rm -f packaging/mt11/thermal_socket
	rm -f '$(MT11_PACKAGE_OUT)' '$(A8_PACKAGE_OUT)' '$(A8_PACKAGE_OUT).sha256'
	rm -rf $(SITL_BUILD)

# ZR10 application bundle: SD-based installation, no partition image.
ZR10_CROSS_COMPILE ?= $(CURDIR)/build/zr10-deps/armv7-eabihf--uclibc--stable-2018.11-1/bin/arm-linux-
ZR10_WEB_PASSWORD ?= ardupilot
ZR10_BUILD_HASH := $(MT11_GIT_HASH)$(shell git diff --quiet HEAD -- || printf -- -dirty)
ZR10_PACKAGE_OUT ?= build/ZR10_APP_ArduPilot_$(MT11_VERSION)_$(ZR10_BUILD_HASH).tar.gz
export ZR10_WEB_PASSWORD
.PHONY: zr10 zr10_package zr10_dependencies
zr10_dependencies:
	tools/bootstrap_zr10_toolchain.sh
	$(MAKE) build-dependencies
zr10: zr10_dependencies
	$(MAKE) -C camera_app CAMERA_BACKEND=zr10 CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'
	$(MAKE) -C web zr10-web ZR10_CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'
	$(ZR10_CROSS_COMPILE)gcc -O2 -Wall -Wextra -Werror -std=c11 \
		-o build/zr10-uuid packaging/zr10/zr10-uuid.c -ldl
zr10_package: zr10
	ZR10_CROSS_COMPILE='$(ZR10_CROSS_COMPILE)' tools/build_zr10_package.sh \
		'$(ZR10_PACKAGE_OUT)' camera_app/build/zr10/camera-app web/zr10-web \
		'$(MT11_VERSION)-$(ZR10_BUILD_HASH)-experimental' build/zr10-uuid

# Flashable customer partition, retaining required vendor platform assets.
ZR10_PLATFORM_DIR ?= $(CURDIR)/packaging/zr10/platform
ZR10_FIRMWARE_OUT ?= build/ZR10_FW_ArduPilot_$(MT11_VERSION)_$(ZR10_BUILD_HASH).bin
.PHONY: zr10_firmware
zr10_firmware: zr10_package
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
.PHONY: zr10_sitl zr10_sitl-run
zr10_sitl: $(ZR10_SITL_BUILD)/main.h264 $(ZR10_SITL_BUILD)/sub.h264 $(ZR10_SITL_BUILD)/photo.jpg
	$(MAKE) build-dependencies
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=zr10 \
		SITL_OBJDIR=build/zr10-sitl \
		SITL_TARGET=$(abspath $(ZR10_SITL_BUILD)/camera-app)
	python3 tools/zr10_firmware.py header web/build/zr10_upgrade.h
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(ZR10_SITL_BUILD)/zr10-web) \
		SITL_ROOT=$(abspath $(ZR10_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(ZR10_SITL_BUILD)) SITL_WEB_BINARY=zr10-web \
		CAMERA_BACKEND=zr10
	python3 sitl/prepare_runtime.py $(ZR10_SITL_BUILD)/runtime sitl/zr10.ini
	@echo "ZR10 SITL built. Run 'make zr10_sitl-run' to start it."
zr10_sitl-run: zr10_sitl
	CAMERA_GIMBAL_SITL_BACKEND=zr10 \
		CAMERA_GIMBAL_SITL_BUILD=$(abspath $(ZR10_SITL_BUILD)) sh sitl/run.sh
.PHONY: zr10_sitl-test
zr10_sitl-test: zr10_sitl
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
.PHONY: z1mini z1mini_package z1mini_retained_isp_package z1mini-test z1mini-test-headers
z1mini: build-dependencies
	$(MAKE) -C camera_app CAMERA_BACKEND=z1mini CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'
	$(MAKE) -C web z1mini-web Z1MINI_CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'
z1mini_retained_isp_package: z1mini
	python3 tools/build_z1mini_package.py '$(Z1MINI_RETAINED_ISP_PACKAGE_OUT)'
z1mini-test-headers:
	$(MAKE) -C web build/version.h build/icons.h
z1mini-test: z1mini-test-headers
	python3 tests/test_z1mini.py
	python3 web/tests/test_z1mini_upgrade.py
	python3 tests/test_z1mini_4k.py
	python3 tests/test_z1mini_service.py
	python3 tests/test_z1mini_service.py --native

# Optional native package: independent 1080p live video and 4K recording.
Z1MINI_NATIVE_PACKAGE_OUT ?= build/Z1Mini_AP_native_$(MT11_VERSION)_$(Z1MINI_BUILD_HASH).gcu
.PHONY: z1mini-overlay-test
z1mini-overlay-test:
	$(MAKE) -C camera_app z1mini-overlay-test Z1MINI_AX_SDK_INCLUDE='$(Z1MINI_AX_SDK_INCLUDE)'

.PHONY: z1mini_native_package
z1mini_native_package: z1mini
	$(MAKE) -C camera_app CAMERA_BACKEND=z1mini CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)' z1mini-capture Z1MINI_AX_SDK_INCLUDE='$(Z1MINI_AX_SDK_INCLUDE)'
	python3 tools/build_z1mini_package.py --native-capture camera_app/build/z1mini/ax-capture '$(Z1MINI_NATIVE_PACKAGE_OUT)'

# The self-contained native-capture variant is the deployable package.
z1mini_package: z1mini_native_package

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
.PHONY: z1mini_sitl z1mini_sitl-run
z1mini_sitl: $(Z1MINI_SITL_BUILD)/main.h264 $(Z1MINI_SITL_BUILD)/sub.h264 $(Z1MINI_SITL_BUILD)/photo.jpg
	$(MAKE) build-dependencies
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=z1mini \
		SITL_OBJDIR=build/z1mini-sitl \
		SITL_TARGET=$(abspath $(Z1MINI_SITL_BUILD)/camera-app)
	$(MAKE) -C web sitl SITL_TARGET=$(abspath $(Z1MINI_SITL_BUILD)/z1mini-web) \
		SITL_ROOT=$(abspath $(Z1MINI_SITL_BUILD)/runtime) \
		SITL_BIN_ROOT=$(abspath $(Z1MINI_SITL_BUILD)) SITL_WEB_BINARY=z1mini-web \
		CAMERA_BACKEND=z1mini
	python3 sitl/prepare_runtime.py $(Z1MINI_SITL_BUILD)/runtime packaging/z1mini/camera.ini
	@echo "Z1MINI SITL built. Run 'make z1mini_sitl-run' to start it."
z1mini_sitl-run: z1mini_sitl
	CAMERA_GIMBAL_SITL_BACKEND=z1mini \
		CAMERA_GIMBAL_SITL_BUILD=$(abspath $(Z1MINI_SITL_BUILD)) sh sitl/run.sh

.PHONY: targets
targets:
	python3 tools/export_targets.py

sitl a8_sitl zr10_sitl z1mini_sitl: targets

.PHONY: targets-test
targets-test: targets
	python3 tests/test_targets.py

.PHONY: z1mini_sitl-test
z1mini_sitl-test: z1mini_sitl
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation upright
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation inverted
