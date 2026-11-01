.DEFAULT_GOAL := all
include ../tools/build.mk

CAMERA_BACKEND ?= mt11
include ../include/apcam/targets.mk
CROSS_COMPILE ?= aarch64-linux-gnu-
CXX := $(CROSS_COMPILE)g++
HOST_CXX ?= c++
# The installed camera filesystem need not provide a matching C++ runtime.
CXX_RUNTIME_FLAGS := -static-libstdc++ -static-libgcc
MT11_VERSION ?= $(shell git tag --merged HEAD --sort=-version:refname 2>/dev/null | awk '/^v[0-9]+\.[0-9]+(\.[0-9]+)?$$/ { print; exit }')
MT11_GIT_HASH ?= $(shell git rev-parse HEAD 2>/dev/null | cut -c1-6)
export MT11_VERSION MT11_GIT_HASH
VERSION_HEADER := build/version.h
ICON_HEADER := build/icons.h
WEBROOT_HEADER := build/webroot.h
WEBROOT_FILES := $(wildcard webroot/*)
# Keep CFLAGS as a compatibility input for existing build scripts.
CFLAGS ?= -O2 -pipe
# Required warnings and C++ runtime policy cannot disappear with user overrides.
override WEB_CXXFLAGS = $(filter-out -std=%,$(CFLAGS)) $(CXXFLAGS) -Wall -Wextra -Wformat-security -Werror -std=gnu++17 -fno-exceptions -fno-rtti -Wno-missing-field-initializers
ifeq ($(CAMERA_BACKEND),z1mini)
override WEB_CXXFLAGS += -isystem ../camera_app/build/mavlink/all/include -Wno-address-of-packed-member
endif
ifeq ($(shell uname -s),Darwin)
override WEB_CXXFLAGS += -include $(abspath ../include/apcam/posix_compat.h) -Wno-deprecated-declarations
endif
LDFLAGS ?= -static
TEST_ROOT := $(CURDIR)/tests/runtime
HOST_TEST := tests/mt11-web-host
RPI_HOST_CHECK := tests/rpi-web-host
RPI_TEST_ROOT := $(CURDIR)/tests/rpi-runtime
SITL_ROOT ?= $(abspath ../build/sitl/runtime)
SITL_TARGET ?= $(abspath ../build/sitl/mt11-web)
SITL_BIN_ROOT ?= $(abspath ../build/sitl)
SITL_CAMERA_BINARY ?= camera-app
SITL_WEB_BINARY ?= mt11-web
SITL_EXTRA_DEFINES ?=
SITL_DEFINES := -DWEBROOT_PATH='"$(CURDIR)/webroot"' -DMT11_WEB_TEST -DMT11_WEB_SITL \
	-DAPP_DIR='"$(SITL_ROOT)/app"' \
	-DMEDIA_ROOT='"$(SITL_ROOT)/mnt"' \
	-DGCU_ROOT='"$(SITL_ROOT)/gcu"' \
	-DCONFIG_PATH='"$(SITL_ROOT)/app/config.ini"' \
	-DCONFIG_BACKUP_PATH='"$(SITL_ROOT)/app/config.ini.web.bak"' \
	-DREPLACEMENT_CONFIG_PATH='"$(SITL_ROOT)/app/camera.ini"' \
	-DREPLACEMENT_CONFIG_BACKUP_PATH='"$(SITL_ROOT)/app/camera.ini.web.bak"' \
	-DPASSWORD_PATH='"$(SITL_ROOT)/app/web.pass"' \
	-DAUTHORIZED_KEYS_PATH='"$(SITL_ROOT)/app/dropbear/authorized_keys"' \
	-DRUNTIME_AUTHORIZED_KEYS_PATH='"$(SITL_ROOT)/runtime-auth/authorized_keys"' \
	-DRUNTIME_AUTHORIZED_KEYS_DIR='"$(SITL_ROOT)/runtime-auth"' \
	-DUSER_LOCK_PATH='"$(SITL_ROOT)/run/users.lock"' \
	-DUPGRADE_LOCK_PATH='"$(SITL_ROOT)/run/upgrade.lock"' \
	-DTIME_SYNC_TEST_PATH='"$(SITL_ROOT)/run/time-sync"' \
	-DSOC_TSENSOR_DEVICE='"$(SITL_ROOT)/run/soc-temperature"' \
	-DSOC_TSENSOR_MAP_OFFSET=0 \
	-DVENDOR_CAMERA_PATH='"$(SITL_ROOT)/app/siyi_camera_app"' \
	-DREPLACEMENT_CAMERA_PATH='"$(SITL_BIN_ROOT)/$(SITL_CAMERA_BINARY)"' \
	-DWEB_PATH='"$(SITL_BIN_ROOT)/$(SITL_WEB_BINARY)"' \
	-DTHERMAL_SOCKET_PATH='"$(SITL_ROOT)/app/bin/thermal_socket"' \
	-DTHERMAL_SOCKET_LOG_PATH='"$(SITL_ROOT)/run/thermal_socket.log"' \
	-DAPP_SELECTION_PATH='"$(SITL_ROOT)/app/app_selection.txt"' \
	-DAPP_LOG_PATH='"$(SITL_ROOT)/run/siyi_camera_app.log"' \
	-DAPP_LOG_OLD_PATH='"$(SITL_ROOT)/run/siyi_camera_app.log.1"' \
	-DREPLACEMENT_LOG_PATH='"$(SITL_ROOT)/run/camera_app.log"' \
	-DREPLACEMENT_LOG_OLD_PATH='"$(SITL_ROOT)/run/camera_app.log.1"' \
	-DRUNTIME_DIR='"$(SITL_ROOT)/run"' \
	-DCAMERA_READY_PATH='"$(SITL_ROOT)/run/camera-app.ready"'

A8_CROSS_COMPILE ?= arm-linux-
A8_HOST_CHECK := tests/a8-web-host

.PHONY: all clean sitl test FORCE generated

generated: $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) build/zr10_upgrade.h

$(VERSION_HEADER): FORCE ../tools/write_build_version.py
	python3 ../tools/write_build_version.py $@

$(ICON_HEADER): ../tools/build_icons.py ../assets/camera-gimbal.svg ../assets/favicon.ico
	python3 ../tools/build_icons.py --output $@

$(WEBROOT_HEADER): ../tools/build_webroot.py $(WEBROOT_FILES) FORCE
	python3 ../tools/build_webroot.py webroot $@

FORCE:

all: mt11-web

mt11-web: mt11-web.cpp gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(call progress,Linking $@)
	$(CXX) $(WEB_CXXFLAGS) $(APCAM_CPPFLAGS) $(LDFLAGS) -o $@ $< -lm

# SIYI A8 mini build of the same source: ARMv7 hard-float, linked against
# the camera's glibc, without the MT11-only vendor-app, thermal and SSH pages.
a8-web: mt11-web.cpp gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(call progress,Linking $@)
	$(A8_CROSS_COMPILE)g++ $(CXX_RUNTIME_FLAGS) $(WEB_CXXFLAGS) -DAPCAM_TARGET=APCAM_TARGET_A8 -o $@ $< -lm

$(A8_HOST_CHECK): mt11-web.cpp gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -DAPCAM_TARGET=APCAM_TARGET_A8 -o $@ $< -lm

$(HOST_TEST): mt11-web.cpp gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) $(APCAM_CPPFLAGS) -DMT11_WEB_TEST -DWEBROOT_PATH='"$(TEST_ROOT)/webroot"' \
		-DAPP_DIR='"$(TEST_ROOT)/app"' \
		-DMEDIA_ROOT='"$(TEST_ROOT)/mnt"' \
		-DCONFIG_PATH='"$(TEST_ROOT)/app/config.ini"' \
		-DCONFIG_BACKUP_PATH='"$(TEST_ROOT)/app/config.ini.web.bak"' \
		-DREPLACEMENT_CONFIG_PATH='"$(TEST_ROOT)/app/camera.ini"' \
		-DREPLACEMENT_CONFIG_BACKUP_PATH='"$(TEST_ROOT)/app/camera.ini.web.bak"' \
		-DREPLACEMENT_CAMERA_PATH='"$(abspath $(HOST_TEST))"' \
		-DPASSWORD_PATH='"$(TEST_ROOT)/app/web.pass"' \
		-DAUTHORIZED_KEYS_PATH='"$(TEST_ROOT)/app/dropbear/authorized_keys"' \
		-DRUNTIME_AUTHORIZED_KEYS_PATH='"$(TEST_ROOT)/runtime-auth/authorized_keys"' \
		-DRUNTIME_AUTHORIZED_KEYS_DIR='"$(TEST_ROOT)/runtime-auth"' \
		-DUSER_LOCK_PATH='"$(TEST_ROOT)/users.lock"' \
		-DUPGRADE_LOCK_PATH='"$(TEST_ROOT)/upgrade.lock"' \
		-DTIME_SYNC_TEST_PATH='"$(TEST_ROOT)/time-sync"' \
		-DSOC_TSENSOR_DEVICE='"$(TEST_ROOT)/soc-temperature"' \
		-DSOC_TSENSOR_MAP_OFFSET=0 \
		-DSESSION_PATH='"$(TEST_ROOT)/sessions"' \
		-DCAMERA_READY_PATH='"$(TEST_ROOT)/camera-app.ready"' \
		-o $@ $< -lm

sitl: $(SITL_TARGET)

.PHONY: portable-sitl
portable-sitl: $(SITL_BIN_ROOT)/mt11-web-portable

$(SITL_BIN_ROOT)/mt11-web-portable: mt11-web.cpp gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) $(APCAM_CPPFLAGS) -DMT11_WEB_TEST -DMT11_WEB_SITL \
		-DWEB_PORTABLE_SITL -o $@ $< -lm

$(SITL_TARGET): mt11-web.cpp gcu_manifest.h Makefile $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) $(APCAM_CPPFLAGS) $(SITL_DEFINES) $(SITL_EXTRA_DEFINES) -o $@ $< -lm

tests/test_string_buffer: tests/test_string_buffer.cpp APC_StringBuffer.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -o $@ $<

tests/test_webroot: tests/test_webroot.cpp APC_WebRoot.h APC_StringBuffer.h $(WEBROOT_HEADER)
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -o $@ $<

tests/test_zt30_process: tests/test_zt30_process.cpp zt30-process.h ../include/apcam/zt30_slots.h
	$(HOST_CXX) $(WEB_CXXFLAGS) -o $@ $<

.PHONY: zt30-parameters-test
zt30-parameters-test: generated
	python3 tests/test_zt30_parameters.py

.PHONY: zt30-process-test
zt30-process-test: tests/test_zt30_process
	./tests/test_zt30_process

test: zt30-parameters-test tests/test_zt30_process $(HOST_TEST) $(A8_HOST_CHECK) $(RPI_HOST_CHECK) tests/test_string_buffer tests/test_webroot
	./tests/test_zt30_process
	./tests/test_string_buffer
	./tests/test_webroot
	python3 tests/test_integration.py ./$(HOST_TEST) '$(TEST_ROOT)'
	python3 tests/test_a8_upgrade.py
	python3 tests/test_rpi_parameters.py ./$(RPI_HOST_CHECK) '$(RPI_TEST_ROOT)'
	node tests/test_video_uri.js webroot/parameters.js
	node tests/test_recording.js webroot/recording.js

clean:
	rm -f tests/test_zt30_process tests/test_string_buffer tests/test_webroot mt11-web a8-web zr10-web z1mini-web rpi_libcam_caddx-web zt30-web zt30-dev-web $(HOST_TEST) $(A8_HOST_CHECK) $(RPI_HOST_CHECK)
	rm -rf tests/runtime tests/rpi-runtime build

ZR10_CROSS_COMPILE ?= $(abspath ../build/zr10-deps/armv7-eabihf--uclibc--stable-2018.11-1/bin/arm-linux-)
zr10-web: build/zr10_upgrade.h zr10_firmware.h mt11-web.cpp ../camera_app/src/compat/zr10_uclibc.cpp $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(call progress,Linking $@)
	$(ZR10_CROSS_COMPILE)g++ $(CXX_RUNTIME_FLAGS) $(WEB_CXXFLAGS) -Wno-unused-function -Wno-missing-field-initializers -DAPCAM_TARGET=APCAM_TARGET_ZR10 -D_FILE_OFFSET_BITS=64 -o $@ $(filter %.cpp,$^)

.PHONY: zr10-test
zr10-test: $(VERSION_HEADER) $(WEBROOT_HEADER) build/zr10_upgrade.h
	python3 tests/test_zr10.py
	python3 tests/test_zr10_service.py

build/zr10_upgrade.h: ../tools/zr10_firmware.py ../packaging/zr10/upgrade_script.txt
	python3 ../tools/zr10_firmware.py header $@

# Raspberry Pi: native build on Raspberry Pi OS, installed beside camera-app.
# The MAVLink headers are generated by the Z1-Mini rule below.
RPI_MAVLINK := ../camera_app/build/mavlink/all/include
rpi_libcam_caddx-web: mt11-web.cpp gcu_manifest.h z1mini_mavlink.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) $(RPI_MAVLINK)/all/mavlink.h Makefile
	$(call progress,Linking $@)
	$(RPI_CROSS_COMPILE)g++ $(WEB_CXXFLAGS) -Wno-unused-function -Wno-address-of-packed-member -DAPCAM_TARGET=APCAM_TARGET_RPI_LIBCAM_CADDX -isystem $(RPI_MAVLINK) -o $@ $< -lm

# The RPi web service with test paths, for the host parameters test.
$(RPI_HOST_CHECK): mt11-web.cpp gcu_manifest.h z1mini_mavlink.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) $(RPI_MAVLINK)/all/mavlink.h Makefile
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -Wno-unused-function -Wno-address-of-packed-member \
		-DAPCAM_TARGET=APCAM_TARGET_RPI_LIBCAM_CADDX -isystem $(RPI_MAVLINK) -DMT11_WEB_TEST \
		-DAPP_DIR='"$(RPI_TEST_ROOT)/app"' \
		-DAPP_SELECTION_DIR='"$(RPI_TEST_ROOT)/app"' \
		-DMEDIA_ROOT='"$(RPI_TEST_ROOT)/mnt"' \
		-DCAPTURE_ROOT='"$(RPI_TEST_ROOT)/mnt/capture"' \
		-DWEBROOT_PATH='"$(RPI_TEST_ROOT)/webroot"' \
		-DSESSION_PATH='"$(RPI_TEST_ROOT)/sessions"' \
		-DUSER_LOCK_PATH='"$(RPI_TEST_ROOT)/users.lock"' \
		-DUPGRADE_LOCK_PATH='"$(RPI_TEST_ROOT)/upgrade.lock"' \
		-DTIME_SYNC_TEST_PATH='"$(RPI_TEST_ROOT)/time-sync"' \
		-DCAMERA_READY_PATH='"$(RPI_TEST_ROOT)/camera-app.ready"' \
		-DAPP_REQUEST_PATH='"$(RPI_TEST_ROOT)/camera-app.request"' \
		-DAPP_REQUEST_LOCK_PATH='"$(RPI_TEST_ROOT)/camera-app.request.lock"' \
		-DAPP_STARTED_PATH='"$(RPI_TEST_ROOT)/camera-app.started"' \
		-DSOC_TEMPERATURE_PATH='"$(RPI_TEST_ROOT)/soc-temperature"' \
		-o $@ $< -lm

Z1MINI_CROSS_COMPILE ?= arm-none-linux-gnueabihf-
Z1MINI_MAVLINK := ../camera_app/build/mavlink/all/include
z1mini-web: mt11-web.cpp gcu_manifest.h z1mini_mavlink.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) $(Z1MINI_MAVLINK)/all/mavlink.h Makefile
	$(call progress,Linking $@)
	$(Z1MINI_CROSS_COMPILE)g++ $(CXX_RUNTIME_FLAGS) $(WEB_CXXFLAGS) -Wno-unused-function -Wno-address-of-packed-member -DAPCAM_TARGET=APCAM_TARGET_Z1_MINI -D_FILE_OFFSET_BITS=64 -I$(Z1MINI_MAVLINK) -o $@ $< -lm

ifeq ($(CAMERA_BACKEND),zr10)
$(SITL_TARGET) $(SITL_BIN_ROOT)/mt11-web-portable $(HOST_TEST): build/zr10_upgrade.h zr10_firmware.h
endif
ifeq ($(CAMERA_BACKEND),z1mini)
$(SITL_TARGET) $(SITL_BIN_ROOT)/mt11-web-portable $(HOST_TEST): $(Z1MINI_MAVLINK)/all/mavlink.h z1mini_mavlink.h
endif

$(Z1MINI_MAVLINK)/all/mavlink.h:
	$(MAKE) -C ../camera_app CAMERA_BACKEND=z1mini build/mavlink/all/include/all/mavlink.h

.PHONY: network-status-test
network-status-test: $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER)
	python3 tests/test_network_status.py

# Initial ZT30 RAM services: no video ownership or persistent camera changes.
ZT30_CROSS_COMPILE ?= arm-linux-
zt30-dev-web: mt11-web.cpp zt30-dev-paths.h gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(ZT30_CROSS_COMPILE)g++ $(CXX_RUNTIME_FLAGS) $(WEB_CXXFLAGS) -DAPCAM_TARGET=APCAM_TARGET_ZT30 -DZT30_WEB_DEV -o $@ $< -lm

# AP-only persistent ZT30 layout; initial native app has one RGB stream.
zt30-web: mt11-web.cpp ../camera_app/src/config.cpp zt30-paths.h zt30-process.h zt30-upgrade.h zt30-bundle-status.h gcu_manifest.h $(VERSION_HEADER) $(ICON_HEADER) $(WEBROOT_HEADER) $(APCAM_HEADERS) $(wildcard APC_*.h) Makefile
	$(ZT30_CROSS_COMPILE)g++ $(CXX_RUNTIME_FLAGS) $(WEB_CXXFLAGS) -DAPCAM_TARGET=APCAM_TARGET_ZT30 -DCAMERA_APP_ZT30_DEV -DZT30_WEB_NATIVE -I../include -o $@ $(filter %.cpp,$^) -lm
