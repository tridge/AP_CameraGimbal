.DEFAULT_GOAL := all
include tools/build.mk

# Written by tools/install_build_environment.py; command-line overrides win.
-include build/environment.mk

# Check before any recipes run, including parallel release builds. Host tests,
# SITL, source downloads and clean targets do not need hardware toolchains.
HARDWARE_BUILD_GOALS := all release mt11_package _mt11_package \
	a8_package zt30_package zr10 zr10_dependencies zr10_package zr10_firmware \
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

.PHONY: all dependencies mt11_tools mt11_package a8_package sitl sitl-run sitl-kill sitl-test sitl-mavlink-test a8_sitl-mavlink-test \
	sitl-video-telemetry-test a8_sitl-video-telemetry-test \
	sitl-armed-recording-test a8_sitl-armed-recording-test \
	recording-recovery-test sitl-supportproxy-test a8_sitl-supportproxy-test \
	a8_sitl a8_sitl-run a8_sitl-test \
	ardupilot-siyi-test ardupilot-mavlink-test mavproxy-camera-test \
	sitl-clean clean _mt11_package

.PHONY: generated camera-app web-app
# Finish shared generators before backend submakes start. A multi-camera
# invocation must have one owner for every generated header and patched source.
generated: build-dependencies
	$(MAKE) -C camera_app generated
	$(MAKE) -C web generated

all: camera-app web-app
camera-app: generated
	$(MAKE) -C camera_app
web-app: generated
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

.PHONY: survey-runtime-test
survey-runtime-test:
	python3 tests/test_survey_runtime.py

release: camera-definitions
	+python3 tools/build_release.py --make '$(MAKE)' --version '$(MT11_VERSION)' \
		--output '$(RELEASE_ROOT)' --targets $(RELEASE_TARGETS)

release-test:
	python3 tests/test_cpp_build.py
	python3 tests/test_release.py
	python3 tests/test_build_dependencies.py
	python3 tests/test_parallel_build.py
	python3 tests/test_prebuilt_tools.py

.PHONY: platform-test
platform-test:
	python3 tests/test_siyi_uuid.py
	python3 tests/test_zr10_install.py
	python3 tests/test_platform_packages.py '$(RELEASE_ROOT)/$(MT11_VERSION)'

.PHONY: build-dependencies mavlink-dependencies bootstrap-dependencies
mavlink-dependencies:
	@if test ! -f modules/mavlink/message_definitions/v1.0/all.xml || \
		test ! -f modules/mavlink/pymavlink/tools/mavgen.py; then \
		git submodule update --init --recursive; \
	fi

build-dependencies: mavlink-dependencies $(if $(BOOTSTRAP_DEPENDENCIES),bootstrap-dependencies)

dependencies: bootstrap-dependencies

bootstrap-dependencies: mavlink-dependencies
	tools/bootstrap_dependencies.sh '$(DEPS_ROOT)'

# Verify on every invocation: also repair deleted/corrupt cached binaries.
# No cross compiler, source download or build-environment setup is needed.
mt11_tools:
	python3 tools/prebuilt_mt11_tools.py --output '$(MT11_TOOLS_ROOT)'

sitl: generated $(SITL_BUILD)/rgb.h264 $(SITL_BUILD)/thermal.h264 \
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

sitl-run: sitl
	sh sitl/run.sh

sitl-kill:
	python3 sitl/kill.py $(CURDIR) $(abspath $(SITL_BUILD)/runtime)

sitl-test: sitl
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation upright
	python3 sitl/test_unigcs.py $(SITL_BUILD)/camera-app --video-codec h264
	python3 sitl/test_unigcs.py $(SITL_BUILD)/camera-app --video-codec h265
	python3 sitl/test_sitl.py $(SITL_BUILD)/camera-app \
		$(SITL_BUILD)/mt11-web sitl/gimbal_sim.py \
		$(abspath $(SITL_BUILD)/runtime) --backend mt11 --orientation inverted

sitl-mavlink-test: sitl
	python3 sitl/test_mavlink_parameters.py --backend mt11 --build $(SITL_BUILD)
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD)
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD) --jitter
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD) --jitter --rate
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

.PHONY: sitl-misb-telemetry-test
sitl-misb-telemetry-test: sitl a8_sitl zr10_sitl z1mini_sitl zt30_sitl
	$(SITL_VIDEO_PYTHON) sitl/test_misb_telemetry.py
	$(SITL_VIDEO_PYTHON) sitl/test_misb_telemetry.py --codec h265

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
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --raw-thermal --reconnect \
		$(if $(MAVPROXY_REPO),--mavproxy $(MAVPROXY_REPO))

a8_sitl-supportproxy-test: a8_sitl
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --backend a8 --build $(A8_SITL_BUILD)

recording-recovery-test:
	+python3 web/tests/test_recording_download.py

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

a8_sitl: generated $(A8_SITL_BUILD)/main.h264 $(A8_SITL_BUILD)/sub.h264 \
	$(A8_SITL_BUILD)/photo.jpg
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
	python3 sitl/test_unigcs.py $(A8_SITL_BUILD)/camera-app --backend a8 --video-codec h265
	python3 sitl/test_unigcs.py $(A8_SITL_BUILD)/camera-app --backend a8 --orientation inverted
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation upright
	python3 sitl/test_sitl.py $(A8_SITL_BUILD)/camera-app \
		$(A8_SITL_BUILD)/a8-web sitl/gimbal_sim.py \
		$(abspath $(A8_SITL_BUILD)/runtime) --backend a8 --orientation inverted

ardupilot-siyi-test:
	+tests/run_ardupilot_siyi_test.sh

ardupilot-mavlink-test:
	+tests/run_ardupilot_mavlink_test.sh

mavproxy-camera-test:
	@test -n "$(MAVPROXY_REPO)" || { \
		echo 'Set MAVPROXY_REPO to the MAVProxy worktree' >&2; exit 2; \
	}
	@test -f "$(MAVPROXY_REPO)/MAVProxy/modules/mavproxy_camera/__init__.py"
	+MAVPROXY_REPO="$(abspath $(MAVPROXY_REPO))" \
		tests/run_ardupilot_mavlink_test.sh

sitl-clean:
	$(call progress,Cleaning SITL builds)
	rm -rf $(SITL_BUILD) $(A8_SITL_BUILD) $(ZR10_SITL_BUILD) $(Z1MINI_SITL_BUILD)
	rm -rf camera_app/build/sitl camera_app/build/a8-sitl \
		camera_app/build/zr10-sitl camera_app/build/z1mini-sitl

packaging/mt11/thermal_socket: packaging/mt11/thermal_socket.cpp
	$(call progress,Linking $@)
	$(CROSS_COMPILE)g++ -static -O2 -pipe -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers \
		-o $@ $<

mt11_package: _mt11_package

_mt11_package: all packaging/mt11/thermal_socket \
	packaging/mt11/mt11-timesync.sh packaging/mt11/app_init.sh \
	packaging/mt11/app_selection.sh packaging/mt11/start-dropbear.sh \
	mt11_tools
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
a8_package: generated build/a8-uuid
	@printf '%s\n' '$(MT11_VERSION)' | grep -Eq '^v[0-9]+\.[0-9]+(\.[0-9]+)?$$' || { \
		echo 'No reachable Git tag of the form vX.y or vX.y.z; cannot name package' >&2; exit 1; \
	}
	@'$(A8_CROSS_COMPILE)gcc' -dumpmachine >/dev/null 2>&1 || { \
		echo '$(A8_CROSS_COMPILE)gcc not found: run python3 tools/install_build_environment.py --targets a8' >&2; \
		echo 'or run make a8_package A8_TOOLCHAIN_DIR=<toolchain root> (or A8_CROSS_COMPILE=<dir>/arm-linux-)' >&2; \
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
.PHONY: zr10 zr10_package zr10_dependencies
zr10_dependencies: build-dependencies
	tools/bootstrap_zr10_toolchain.sh
zr10: zr10_dependencies generated build/zr10-uuid
	$(MAKE) -C camera_app CAMERA_BACKEND=zr10 CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'
	$(MAKE) -C web zr10-web ZR10_CROSS_COMPILE='$(ZR10_CROSS_COMPILE)'

build/zr10-uuid: packaging/zr10/zr10-uuid.cpp Makefile | zr10_dependencies
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(ZR10_CROSS_COMPILE)g++ -static-libstdc++ -static-libgcc -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers \
		-o $@ $< -ldl

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
zr10_sitl: generated $(ZR10_SITL_BUILD)/main.h264 $(ZR10_SITL_BUILD)/sub.h264 $(ZR10_SITL_BUILD)/photo.jpg
	$(MAKE) -C camera_app sitl CAMERA_BACKEND=zr10 \
		SITL_OBJDIR=build/zr10-sitl \
		SITL_TARGET=$(abspath $(ZR10_SITL_BUILD)/camera-app)
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
.PHONY: z1mini z1mini_package z1mini_retained_isp_package z1mini-test z1mini-test-headers
z1mini: generated
	$(MAKE) -C camera_app CAMERA_BACKEND=z1mini CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'
	$(MAKE) -C web z1mini-web Z1MINI_CROSS_COMPILE='$(Z1MINI_CROSS_COMPILE)'
z1mini_retained_isp_package: z1mini
	python3 tools/build_z1mini_package.py '$(Z1MINI_RETAINED_ISP_PACKAGE_OUT)'
z1mini-test-headers:
	$(MAKE) -C web build/version.h build/icons.h
z1mini-test: z1mini-test-headers
	+python3 tests/test_z1mini.py
	+python3 web/tests/test_z1mini_upgrade.py
	python3 tests/test_z1mini_4k.py
	python3 tests/test_z1mini_4k.py --orientation inverted
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
.PHONY: z1mini_sitl z1mini_sitl-run
z1mini_sitl: generated $(Z1MINI_SITL_BUILD)/main.h264 $(Z1MINI_SITL_BUILD)/sub.h264 $(Z1MINI_SITL_BUILD)/photo.jpg
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

sitl a8_sitl zr10_sitl z1mini_sitl zt30_sitl: targets

.PHONY: targets-test
targets-test: targets
	python3 tests/test_targets.py

.PHONY: z1mini_sitl-test
z1mini_sitl-test: z1mini_sitl
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation upright
	python3 sitl/test_z1mini_sitl.py --build $(Z1MINI_SITL_BUILD) --orientation inverted

.PHONY: zt30_sitl zt30_sitl-run zt30_sitl-test zt30_package
zt30_sitl: generated $(ZT30_SITL_BUILD)/rgb.h264 $(ZT30_SITL_BUILD)/thermal.h264 $(ZT30_SITL_BUILD)/photo.jpg
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

zt30_sitl-run: zt30_sitl
	CAMERA_GIMBAL_SITL_BACKEND=zt30 CAMERA_GIMBAL_SITL_BUILD=$(abspath $(ZT30_SITL_BUILD)) sh sitl/run.sh

zt30_sitl-test: zt30_sitl camera-definitions
	python3 sitl/test_zt30.py --build $(ZT30_SITL_BUILD)
	python3 sitl/test_zt30.py --build $(ZT30_SITL_BUILD) --orientation upright --no-version-reply

# Web upgrade for cameras with the stable AP launchers already installed.
# Runtime libraries are build inputs and pins; the bundle never replaces them.
zt30_package: generated
	cd '$(ZT30_RUNTIME_ROOT)' && sha256sum --check SHA256SUMS
	$(MAKE) zt30_dev_camera
	$(MAKE) -C web zt30-web ZT30_CROSS_COMPILE=$(ZT30_CROSS_COMPILE)
	python3 tools/build_zt30_app_bundle.py --runtime-root '$(ZT30_RUNTIME_ROOT)' \
		--readelf '$(ZT30_CROSS_COMPILE)readelf' \
		--version '$(MT11_VERSION)_$(MT11_GIT_HASH)' --output '$(ZT30_PACKAGE_OUT)'

# ABI observation probe; no explicit pipeline Init/Exit/bind/buffer calls.
ZT30_CROSS_COMPILE ?= $(A8_CROSS_COMPILE)
.PHONY: zt30_abi_probe zt30_abi_probe-test
zt30_abi_probe: build/zt30/abi-probe
build/zt30/abi-probe: tools/zt30_abi_probe.cpp tools/check_zt30_elf.py $(wildcard camera_app/src/backends/a8/mi/star/*.h) camera_app/src/backends/a8/mi/symbols.h camera_app/src/backends/a8/mi/types.h camera_app/src/backends/a8/mi/macros.h
	mkdir -p $(dir $@)
	$(ZT30_CROSS_COMPILE)g++ -O2 -std=gnu++17 -Wall -Wextra -Werror -Wno-unused-function -I. $< -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_abi_probe-test:
	python3 tests/test_zt30_abi_probe.py
	python3 tests/test_zt30_elf.py

.PHONY: zt30_ram_watchdog-test
zt30_ram_watchdog-test:
	python3 tests/test_zt30_vendor_exec.py
	python3 tests/test_zt30_ram_watchdog.py

.PHONY: zt30_vendor_exec
zt30_vendor_exec: build/zt30/vendor-exec
build/zt30/vendor-exec: tools/zt30_vendor_exec.c tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=c99 -O2 -Wall -Wextra -Werror $< -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf

# Diagnostic UART observation library; never included in installed firmware.
.PHONY: zt30_uart_trace zt30_uart_trace-test
zt30_uart_trace: build/zt30/uart-trace.so
build/zt30/uart-trace.so: tools/zt30_uart_trace.c tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared $< -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf --shared-library
zt30_uart_trace-test:
	python3 tests/test_zt30_uart_trace.py

.PHONY: zt30_control_probe zt30_control_probe-test
zt30_control_probe: build/zt30/control-probe
build/zt30/control-probe: tools/zt30_control_probe.cpp camera_app/src/protocol/siyi.cpp camera_app/src/protocol/private_uart.cpp tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -Icamera_app/include $(filter %.cpp,$^) -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_control_probe-test:
	python3 tests/test_zt30_control_probe.py

ZT30_HOST_CXX ?= c++
.PHONY: zt30_focus-test
zt30_focus-test:
	@mkdir -p build/tests
	$(ZT30_HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -I. tests/test_zt30_focus.cpp -o build/tests/zt30-focus-test
	build/tests/zt30-focus-test

.PHONY: zt30_owned_sys_probe zt30_owned_sys_probe-test
zt30_owned_sys_probe: build/zt30/owned-sys-probe
build/zt30/owned-sys-probe: tools/zt30_owned_sys_probe.cpp tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror $< -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_owned_sys_probe-test:
	python3 tests/test_zt30_owned_sys_probe.py

.PHONY: zt30_kmsg_capture zt30_kmsg_capture-test
zt30_kmsg_capture: build/zt30/kmsg-capture
build/zt30/kmsg-capture: tools/zt30_kmsg_capture.c tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_kmsg_capture-test:
	python3 tests/test_zt30_kmsg_capture.py

.PHONY: zt30_ioctl_trace zt30_ioctl_trace-test
zt30_ioctl_trace: build/zt30/ioctl-trace.so
build/zt30/ioctl-trace.so: tools/zt30_ioctl_trace.c tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared $< -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf --shared-library
zt30_ioctl_trace-test:
	python3 tests/test_zt30_ioctl_trace.py

# RAM-only USB observer; does not enable the camera's thermal backend.
.PHONY: zt30_usb_trace zt30_usb_trace-test
zt30_usb_trace: build/zt30/usb-trace.so
build/zt30/usb-trace.so: tools/zt30_usb_trace.c tools/zt30_ioctl_trace.c tools/zt30_trace_output.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared $< -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf --shared-library
zt30_usb_trace-test:
	python3 tests/test_zt30_usb_trace.py

# Two one-shot USB startup header observations; never reads image buffers.
.PHONY: zt30_usb_header_trace zt30_usb_header_trace-test
zt30_usb_header_trace: build/zt30/usb_header-trace.so
build/zt30/usb_header-trace.so: tools/zt30_usb_header_trace.c tools/zt30_usb_header_trace.h tools/zt30_usb_memory_guard.h tools/zt30_usb_urb_snapshot.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/zt30_ioctl_trace.c tools/zt30_trace_output.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -fPIC -c $< -o $@.o
	$(ZT30_CROSS_COMPILE)gcc -shared $@.o -ldl -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf --shared-library
zt30_usb_header_trace-test:
	python3 tests/test_zt30_usb_header_trace.py

# Claimed-handle startup UVC negotiation observer; no thermal image reads.
.PHONY: zt30_usb_setup_trace zt30_usb_setup_trace-test
zt30_usb_setup_trace: build/zt30/usb_setup-trace.so
build/zt30/usb_setup-trace.so: tools/zt30_usb_setup_trace.c tools/zt30_usb_setup_trace.h tools/zt30_usb_control_snapshot.h tools/zt30_usb_memory_guard.h tools/zt30_usb_urb_snapshot.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/zt30_ioctl_trace.c tools/zt30_trace_output.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -fPIC -c $< -o $@.o
	$(ZT30_CROSS_COMPILE)gcc -shared $@.o -ldl -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf --shared-library
zt30_usb_setup_trace-test:
	python3 tests/test_zt30_usb_setup_trace.py

# Private UVC OUT metadata qualification; no vendor preload or thermal backend.
.PHONY: zt30_usb_control_probe zt30_usb_control-test
zt30_usb_control_probe: build/zt30/usb-control-probe
build/zt30/usb-control-probe: tests/zt30_usb_control_fixture.c tools/zt30_usb_control_snapshot.h tools/zt30_usb_memory_guard.h tools/zt30_usb_urb_snapshot.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -Itools -c $< -o $@.o
	$(ZT30_CROSS_COMPILE)gcc $@.o -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_usb_control-test:
	python3 tests/test_zt30_usb_control_snapshot.py

# Private no-peripheral qualification; not a vendor preload or thermal backend.
.PHONY: zt30_usb_memory_probe zt30_usb_memory-test
zt30_usb_memory_probe: build/zt30/usb-memory-probe
build/zt30/usb-memory-probe: tests/zt30_usb_memory_fixture.c tools/zt30_usb_memory_guard.h tools/zt30_usb_urb_snapshot.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -Itools -c $< -o $@.o
	$(ZT30_CROSS_COMPILE)gcc $@.o -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_usb_memory-test:
	python3 tests/test_zt30_usb_memory.py

.PHONY: zt30_self_read_probe zt30_self_read_probe-test
zt30_self_read_probe: build/zt30/self-read-probe
build/zt30/self-read-probe: tools/zt30_self_read_probe.c tools/zt30_self_read.h tools/zt30_stack_guard.h tools/check_zt30_elf.py
	@mkdir -p $(dir $@)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror $< -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_self_read_probe-test:
	python3 tests/test_zt30_self_read_probe.py
	python3 tests/test_zt30_stack_guard.py

.PHONY: zt30_payload_probe zt30_payload_probe-test
zt30_payload_probe: build/zt30/payload-probe
build/zt30/payload-probe: tools/zt30_payload_probe.c tools/zt30_ioctl_payload.h tools/zt30_ioctl_registry.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/check_zt30_elf.py
	@mkdir -p $(dir $@)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror $< -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_payload_probe-test:
	python3 tests/test_zt30_payload_probe.py

.PHONY: zt30_ioctl_payload_trace
zt30_ioctl_payload_trace: build/zt30/ioctl-payload-trace.so
build/zt30/ioctl-payload-trace.so: tools/zt30_ioctl_payload_trace.c tools/zt30_trace_output.h tools/zt30_ioctl_trace.c tools/zt30_ioctl_payload.h tools/zt30_ioctl_registry.h tools/zt30_self_read.h tools/zt30_stack_guard.h tools/check_zt30_elf.py
	@mkdir -p $(dir $@)
	$(ZT30_CROSS_COMPILE)gcc -std=gnu11 -O2 -Wall -Wextra -Werror -shared -fPIC $< -ldl -lpthread -o $@
	python3 tools/check_zt30_elf.py $@ --shared-library --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: zt30_ioctl_payload_trace-test
zt30_ioctl_payload_trace-test:
	python3 tests/test_zt30_ioctl_payload_trace.py

.PHONY: zt30_media_abi-test
zt30_media_abi-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_media_abi.cpp -o build/tests/zt30-media-abi-test
	build/tests/zt30-media-abi-test

.PHONY: zt30_rgb_probe zt30_rgb_owner-test
zt30_rgb_probe: build/zt30/rgb-probe build/zt30/rgb-sdk.so
build/zt30/rgb-probe: tools/zt30_rgb_probe.cpp camera_app/src/backends/zt30/rgb_owner.cpp camera_app/src/backends/zt30/rgb_owner.h camera_app/src/backends/zt30/media_graph.h camera_app/src/backends/zt30/mi/abi.h camera_app/src/backends/zt30/mi/api.h camera_app/src/backends/zt30/mi/encoder_abi.h camera_app/src/backends/zt30/mi/native_sdk.h
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror $(filter %.cpp,$^) -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_rgb_owner-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_rgb_owner.cpp camera_app/src/backends/zt30/rgb_owner.cpp -o build/tests/zt30-rgb-owner-test
	build/tests/zt30-rgb-owner-test

ZT30_SDK_LIBDIR ?= $(ZT30_RUNTIME_ROOT)/config/lib
ZT30_SDK_LIBRARIES := $(wildcard $(ZT30_SDK_LIBDIR)/*.so)
build/zt30/rgb-sdk.so: tools/zt30_rgb_sdk.c $(ZT30_SDK_LIBRARIES)
	@test -n '$(ZT30_SDK_LIBDIR)' || { echo 'ZT30_SDK_LIBDIR must point to the captured ZT30 /config/lib' >&2; exit 1; }
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared $< -L'$(ZT30_SDK_LIBDIR)' -Wl,--no-as-needed -lcam_os_wrapper -lmi_sys -lmi_sensor -lmi_vif -lispalgo -lcus3a -lmi_isp -lmi_scl -lm -lpthread -Wl,--as-needed -o $@
	python3 tools/check_zt30_elf.py $@ --shared-library --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: zt30_rgb_sdk-test
zt30_rgb_sdk-test:
	python3 tests/test_zt30_rgb_sdk.py

.PHONY: zt30_encoder_owner-test
zt30_encoder_owner-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_encoder_owner.cpp camera_app/src/backends/zt30/encoder_owner.cpp -o build/tests/zt30-encoder-owner-test
	build/tests/zt30-encoder-owner-test

.PHONY: zt30_encoder_probe
zt30_encoder_probe: build/zt30/encoder-probe build/zt30/encoder-sdk.so
build/zt30/encoder-probe: tools/zt30_rgb_probe.cpp camera_app/src/backends/zt30/rgb_owner.cpp camera_app/src/backends/zt30/encoder_owner.cpp camera_app/src/backends/zt30/rgb_owner.h camera_app/src/backends/zt30/encoder_owner.h camera_app/src/backends/zt30/media_graph.h camera_app/src/backends/zt30/mi/abi.h camera_app/src/backends/zt30/mi/api.h camera_app/src/backends/zt30/mi/encoder_abi.h camera_app/src/backends/zt30/mi/native_sdk.h
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -DZT30_ENCODER_PROBE $(filter %.cpp,$^) -ldl -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
build/zt30/encoder-sdk.so: tools/zt30_rgb_sdk.c $(ZT30_SDK_LIBRARIES)
	@test -n '$(ZT30_SDK_LIBDIR)' || { echo 'ZT30_SDK_LIBDIR must point to the captured ZT30 /config/lib' >&2; exit 1; }
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared $< -L'$(ZT30_SDK_LIBDIR)' -Wl,--no-as-needed -lcam_os_wrapper -lmi_sys -lmi_sensor -lmi_vif -lispalgo -lcus3a -lmi_isp -lmi_scl -lmi_venc -lmi_rgn -lm -lpthread -Wl,--as-needed -o $@
	python3 tools/check_zt30_elf.py $@ --shared-library --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: zt30_pts_clock-test
zt30_pts_clock-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_pts_clock.cpp -o build/tests/zt30-pts-clock-test
	build/tests/zt30-pts-clock-test

include tools/zt30_stream.mk

.PHONY: zt30_vui_sdk-test
zt30_vui_sdk-test:
	python3 tests/test_zt30_vui_sdk.py

.PHONY: zt30_media_backend-test
zt30_media_backend-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror -Icamera_app/include -Iinclude tests/test_zt30_media_backend.cpp camera_app/src/backends/zt30/media_backend.cpp camera_app/src/backends/zt30/rgb_owner.cpp camera_app/src/backends/zt30/encoder_owner.cpp -lpthread -Wl,--wrap=pthread_create -o build/tests/zt30-media-backend-test
	build/tests/zt30-media-backend-test

# CI can compile/link without the private libraries loaded only at runtime.
.PHONY: zt30_ram_camera zt30_ram_camera_build
zt30_ram_camera: build/zt30/encoder-sdk.so zt30_ram_camera_build
zt30_ram_camera_build: generated
	$(MAKE) -C camera_app CAMERA_BACKEND=zt30 ZT30_RAM_BUILD=1 CROSS_COMPILE=$(ZT30_CROSS_COMPILE) all

# Development application: continuous RGB, normal frontends, no flash installer.
.PHONY: zt30_dev_camera zt30_dev_camera_build
zt30_dev_camera: build/zt30/encoder-sdk.so zt30_dev_camera_build
zt30_dev_camera_build: generated
	$(MAKE) -C camera_app CAMERA_BACKEND=zt30 ZT30_DEV_BUILD=1 CROSS_COMPILE=$(ZT30_CROSS_COMPILE) all

.PHONY: zt30_ram_frontend-test
zt30_ram_frontend-test:
	$(MAKE) -C camera_app build/host/tracking_dlib.o
	mkdir -p build/tests
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -DCAMERA_APP_ZT30_RAM -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Icamera_app/include -Iinclude -ffunction-sections -fdata-sections -Wl,--gc-sections tests/test_zt30_ram_frontend.cpp camera_app/src/media/media.cpp camera_app/src/media/tracking.cpp camera_app/build/host/tracking_dlib.o -lpthread -lm -o build/tests/zt30-ram-frontend-test
	build/tests/zt30-ram-frontend-test

# Owned ISO primitives use injected USB operations in host/private ARM tests.
.PHONY: zt30_usb_iso-test zt30_usb_iso_private_probe
ZT30_USB_ISO_SOURCES := camera_app/src/backends/zt30/usb_iso.cpp camera_app/src/backends/zt30/uvc_frame.cpp
zt30_usb_iso-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_usb_iso.cpp $(ZT30_USB_ISO_SOURCES) -o build/tests/zt30-usb-iso-test
	build/tests/zt30-usb-iso-test
zt30_usb_iso_private_probe: build/zt30/usb-iso-private-probe
build/zt30/usb-iso-private-probe: tests/test_zt30_usb_iso.cpp $(ZT30_USB_ISO_SOURCES) camera_app/src/backends/zt30/usb_iso.h camera_app/src/backends/zt30/uvc_frame.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tests/test_zt30_usb_iso.cpp -o $@.test.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/usb_iso.cpp -o $@.usb.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/uvc_frame.cpp -o $@.frame.o
	$(ZT30_CROSS_COMPILE)g++ $@.test.o $@.usb.o $@.frame.o -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: zt30_usb_iso_probe zt30_usb_iso_probe-test
zt30_usb_iso_probe: build/zt30/usb-iso-probe
build/zt30/usb-iso-probe: tools/zt30_usb_iso_probe.cpp $(ZT30_USB_ISO_SOURCES) camera_app/src/backends/zt30/usb_iso.h camera_app/src/backends/zt30/uvc_frame.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tools/zt30_usb_iso_probe.cpp -o $@.probe.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/usb_iso.cpp -o $@.usb.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/uvc_frame.cpp -o $@.frame.o
	$(ZT30_CROSS_COMPILE)g++ $@.probe.o $@.usb.o $@.frame.o -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
zt30_usb_iso_probe-test:
	python3 tests/test_zt30_usb_iso_probe.py

.PHONY: zt30_thermal_pixels-test zt30_thermal_pixels_private_probe
zt30_thermal_pixels-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_thermal_pixels.cpp camera_app/src/backends/zt30/thermal_pixels.cpp -o build/tests/zt30-thermal-pixels-test
	./build/tests/zt30-thermal-pixels-test
zt30_thermal_pixels_private_probe: build/zt30/thermal-pixels-private-probe
build/zt30/thermal-pixels-private-probe: tests/test_zt30_thermal_pixels.cpp camera_app/src/backends/zt30/thermal_pixels.cpp camera_app/src/backends/zt30/thermal_pixels.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tests/test_zt30_thermal_pixels.cpp -o $@.test.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/thermal_pixels.cpp -o $@.pixels.o
	$(ZT30_CROSS_COMPILE)g++ $@.test.o $@.pixels.o -o $@
	python3 tools/check_zt30_elf.py $@

.PHONY: zt30_buffer_abi-test zt30_buffer_abi_private_probe
zt30_buffer_abi-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_buffer_abi.cpp -o build/tests/zt30-buffer-abi-test
	./build/tests/zt30-buffer-abi-test
zt30_buffer_abi_private_probe: build/zt30/buffer-abi-private-probe
build/zt30/buffer-abi-private-probe: tests/test_zt30_buffer_abi.cpp camera_app/src/backends/zt30/mi/buffer_abi.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tests/test_zt30_buffer_abi.cpp -o $@.test.o
	$(ZT30_CROSS_COMPILE)g++ $@.test.o -o $@
	python3 tools/check_zt30_elf.py $@

.PHONY: zt30_input_buffer-test zt30_input_buffer_private_probe
zt30_input_buffer-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_input_buffer.cpp camera_app/src/backends/zt30/input_buffer.cpp -o build/tests/zt30-input-buffer-test
	./build/tests/zt30-input-buffer-test
zt30_input_buffer_private_probe: build/zt30/input-buffer-private-probe
build/zt30/input-buffer-private-probe: tests/test_zt30_input_buffer.cpp camera_app/src/backends/zt30/input_buffer.cpp camera_app/src/backends/zt30/input_buffer.h camera_app/src/backends/zt30/mi/buffer_abi.h tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tests/test_zt30_input_buffer.cpp -o $@.test.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c camera_app/src/backends/zt30/input_buffer.cpp -o $@.owner.o
	$(ZT30_CROSS_COMPILE)g++ $@.test.o $@.owner.o -o $@
	python3 tools/check_zt30_elf.py $@

.PHONY: zt30_input_metadata-test zt30_input_metadata_private_probe zt30_input_metadata_probe
ZT30_INPUT_METADATA_SOURCE = camera_app/src/backends/zt30/input_metadata.cpp
ZT30_INPUT_METADATA_HEADERS = camera_app/src/backends/zt30/input_metadata.h camera_app/src/backends/zt30/mi/buffer_abi.h camera_app/src/backends/zt30/mi/encoder_abi.h camera_app/src/backends/zt30/mi/abi.h camera_app/src/backends/zt30/media_graph.h
zt30_input_metadata-test:
	@mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_input_metadata.cpp $(ZT30_INPUT_METADATA_SOURCE) -o build/tests/zt30-input-metadata-test
	./build/tests/zt30-input-metadata-test
	python3 tests/test_zt30_input_metadata_probe.py
zt30_input_metadata_private_probe: build/zt30/input-metadata-private-probe
build/zt30/input-metadata-private-probe: tests/test_zt30_input_metadata.cpp $(ZT30_INPUT_METADATA_SOURCE) $(ZT30_INPUT_METADATA_HEADERS) tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tests/test_zt30_input_metadata.cpp -o $@.test.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c $(ZT30_INPUT_METADATA_SOURCE) -o $@.owner.o
	$(ZT30_CROSS_COMPILE)g++ $@.test.o $@.owner.o -o $@
	python3 tools/check_zt30_elf.py $@
zt30_input_metadata_probe: build/zt30/input-metadata-probe build/zt30/input-sdk.so
build/zt30/input-metadata-probe: tools/zt30_input_metadata_probe.cpp $(ZT30_INPUT_METADATA_SOURCE) $(ZT30_INPUT_METADATA_HEADERS) tools/check_zt30_elf.py
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c tools/zt30_input_metadata_probe.cpp -o $@.probe.o
	$(ZT30_CROSS_COMPILE)g++ -std=c++11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=8192 -fstack-usage -c $(ZT30_INPUT_METADATA_SOURCE) -o $@.owner.o
	$(ZT30_CROSS_COMPILE)g++ $@.probe.o $@.owner.o -ldl -o $@
	python3 tools/check_zt30_elf.py $@
build/zt30/input-sdk.so: tools/zt30_input_sdk.c tools/check_zt30_elf.py
	@test -n '$(ZT30_SDK_LIBDIR)' || { echo 'ZT30_SDK_LIBDIR must point to captured ZT30 /config/lib' >&2; exit 1; }
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)gcc -std=c99 -O2 -Wall -Wextra -Werror -fPIC -shared $< -L'$(ZT30_SDK_LIBDIR)' -Wl,--no-as-needed -lcam_os_wrapper -lmi_sys -lmi_venc -lm -lpthread -Wl,--as-needed -o $@
	python3 tools/check_zt30_elf.py $@ --shared-library

# Offline, device-specific SD prototype, separate from web upgrade bundles.
ZT30_CUSTOMER_ROOT ?=
ZT30_DEV_PACKAGE_OUT ?= build/zt30-dev/SIYI_ZT30_UpgradeSD.bin.candidate
ZT30_DEV_TOOLS ?= build/deps/zt30-dev-tools/bin
.PHONY: zt30_dev_package-test zt30_dev_package
zt30_dev_package-test:
	python3 tests/test_zt30_dev_package.py
zt30_dev_package: zt30_dev_camera build/zt30/zt30-slot
	@test -n '$(ZT30_CUSTOMER_ROOT)' || { echo 'Set ZT30_CUSTOMER_ROOT to this device captured customer filesystem' >&2; exit 1; }
	$(MAKE) -C web zt30-web ZT30_CROSS_COMPILE=$(ZT30_CROSS_COMPILE)
	python3 tools/build_zt30_dev_package.py build --customer-root '$(ZT30_CUSTOMER_ROOT)' --camera-app camera_app/build/zt30-dev/camera-app --web-app web/zt30-web --sdk build/zt30/encoder-sdk.so --dropbear '$(ZT30_DEV_TOOLS)/dropbear' --dropbearkey '$(ZT30_DEV_TOOLS)/dropbearkey' --output '$(ZT30_DEV_PACKAGE_OUT)'

.PHONY: zt30_native_control-test
zt30_native_control-test: generated
	mkdir -p build/tests
	$(ZT30_HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -Icamera_app/include -Icamera_app/build tests/test_zt30_native_control.cpp camera_app/src/protocol/siyi.cpp camera_app/src/protocol/private_uart.cpp -lutil -Wl,--wrap=write -o build/tests/zt30-native-control-test
	build/tests/zt30-native-control-test

# Compile the native protocol policy without the hardware media factory.
.PHONY: zt30_camera_information-test
zt30_camera_information-test: generated
	mkdir -p build/tests
	$(ZT30_HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -Icamera_app/include -Icamera_app/build/mavlink/all/include -ffunction-sections -fdata-sections -Wl,--gc-sections tests/test_zt30_camera_information.cpp camera_app/src/protocol/mavlink.cpp camera_app/src/config.cpp -lm -o build/tests/zt30-camera-information-test
	build/tests/zt30-camera-information-test

.PHONY: zt30_input_mapping-test
zt30_input_mapping-test:
	mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_input_mapping.cpp camera_app/src/backends/zt30/input_mapping.cpp -o build/tests/zt30-input-mapping-test
	build/tests/zt30-input-mapping-test

.PHONY: zt30_thermal_start_budget-test
zt30_thermal_start_budget-test:
	mkdir -p build/tests
	$(CXX) -std=c++11 -O2 -Wall -Wextra -Werror tests/test_zt30_thermal_start_budget.cpp -o build/tests/zt30-thermal-start-budget-test
	build/tests/zt30-thermal-start-budget-test

.PHONY: zt30_jpeg_owner-test
zt30_jpeg_owner-test:
	mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror tests/test_zt30_jpeg_owner.cpp camera_app/src/backends/zt30/encoder_owner.cpp -o build/tests/zt30-jpeg-owner-test
	build/tests/zt30-jpeg-owner-test

.PHONY: zt30_slots-test
zt30_slots-test:
	python3 tests/test_zt30_slots.py

build/zt30/zt30-slot: tools/zt30_slots.cpp include/apcam/zt30_slots.h include/apcam/zt30_bundle.h include/apcam/zt30_bundle_elf.h include/apcam/sha256.h include/apcam/json_reader.h include/apcam/zt30_update.h include/apcam/zt30_trial.h include/apcam/zt30_services.h include/apcam/zt30_process.h include/apcam/live_video_probe.h tools/check_zt30_elf.py
	mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -static-libstdc++ -static-libgcc -Iinclude $< -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf

.PHONY: zt30_bundle-test
zt30_bundle-test:
	python3 tests/test_zt30_bundle.py

.PHONY: zt30_update-test
zt30_update-test: generated
	python3 tests/test_zt30_update.py
	python3 tests/test_zt30_services.py
	python3 tests/test_live_video_probe.py
	python3 web/tests/test_zt30_upgrade.py
	python3 web/tests/test_zt30_upgrade_browser.py

.PHONY: zt30_image-test
zt30_image-test:
	mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -I. -Icamera_app/include -Iinclude tests/test_zt30_image.cpp camera_app/src/backends/zt30/isp.cpp camera_app/src/log.cpp -lpthread -o build/tests/zt30-image-test
	build/tests/zt30-image-test

.PHONY: zt30_recording-test
zt30_recording-test:
	mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -I. -Icamera_app/include -Iinclude tests/test_zt30_recording.cpp camera_app/src/backends/zt30/recording.cpp camera_app/src/log.cpp -lpthread -o build/tests/zt30-recording-test
	python3 -c 'import subprocess,tempfile; t=tempfile.TemporaryDirectory(); subprocess.run(["build/tests/zt30-recording-test",t.name],check=True)'

.PHONY: zt30_thermal_controls-test
zt30_thermal_controls-test:
	@mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -I. tests/test_zt30_thermal_controls.cpp camera_app/src/backends/zt30/thermal_controls.cpp -lpthread -o build/tests/zt30-thermal-controls-test
	build/tests/zt30-thermal-controls-test

.PHONY: zt30_overlay-test
zt30_overlay-test:
	@mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -no-pie -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -I. -Iinclude -Icamera_app/include tests/test_zt30_overlay.cpp camera_app/src/backends/zt30/overlay.cpp camera_app/src/media/overlay.cpp camera_app/src/log.cpp -ldl -lpthread -o build/tests/zt30-overlay-test
	build/tests/zt30-overlay-test

.PHONY: zt30_unigcs_encoder-test
zt30_unigcs_encoder-test:
	@mkdir -p build/tests
	$(ZT30_HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-missing-field-initializers -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -Icamera_app/include -Icamera_app/build -ffunction-sections -fdata-sections -Wl,--gc-sections tests/test_zt30_unigcs_encoder.cpp -o build/tests/zt30-unigcs-encoder-test
	build/tests/zt30-unigcs-encoder-test
