CAMERA_BACKEND ?= mt11
include ../include/apcam/targets.mk
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
HOST_CC ?= cc
MT11_VERSION ?= $(shell git tag --merged HEAD --sort=-version:refname 2>/dev/null | awk '/^v[0-9]+\.[0-9]+$$/ { print; exit }')
MT11_GIT_HASH ?= $(shell git rev-parse HEAD 2>/dev/null | cut -c1-6)
export MT11_VERSION MT11_GIT_HASH
VERSION_HEADER := build/version.h
ICON_HEADER := build/icons.h
CFLAGS ?= -O2 -pipe -Wall -Wextra -Wformat-security -Werror -std=c11
ifeq ($(CAMERA_BACKEND),z1mini)
CFLAGS += -isystem ../camera_app/build/mavlink/all/include -Wno-address-of-packed-member
endif
LDFLAGS ?= -static
TEST_ROOT := $(CURDIR)/tests/runtime
HOST_TEST := tests/mt11-web-host
SITL_ROOT ?= $(abspath ../build/sitl/runtime)
SITL_TARGET ?= $(abspath ../build/sitl/mt11-web)
SITL_BIN_ROOT ?= $(abspath ../build/sitl)
SITL_CAMERA_BINARY ?= camera-app
SITL_WEB_BINARY ?= mt11-web
SITL_EXTRA_DEFINES ?=
SITL_DEFINES := -DMT11_WEB_TEST -DMT11_WEB_SITL \
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

.PHONY: all clean sitl test a8-web FORCE

$(VERSION_HEADER): FORCE ../tools/write_build_version.py
	python3 ../tools/write_build_version.py $@

$(ICON_HEADER): ../tools/build_icons.py ../assets/camera-gimbal.svg ../assets/favicon.ico
	python3 ../tools/build_icons.py --output $@

FORCE:

.DEFAULT_GOAL := all

all: mt11-web

mt11-web: mt11-web.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	$(CC) $(CFLAGS) $(APCAM_CPPFLAGS) $(LDFLAGS) -o $@ $< -lm

# SIYI A8 mini build of the same source: ARMv7 hard-float, linked against
# the camera's glibc, without the MT11-only vendor-app, thermal and SSH pages.
a8-web: mt11-web.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	$(A8_CROSS_COMPILE)gcc $(CFLAGS) -DAPCAM_TARGET=APCAM_TARGET_A8 -o $@ $< -lm

$(A8_HOST_CHECK): mt11-web.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	$(HOST_CC) $(CFLAGS) -DAPCAM_TARGET=APCAM_TARGET_A8 -o $@ $< -lm

$(HOST_TEST): mt11-web.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	$(HOST_CC) $(CFLAGS) $(APCAM_CPPFLAGS) -DMT11_WEB_TEST \
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

$(SITL_BIN_ROOT)/mt11-web-portable: mt11-web.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) $(APCAM_CPPFLAGS) -DMT11_WEB_TEST -DMT11_WEB_SITL \
		-DWEB_PORTABLE_SITL -o $@ $< -lm

$(SITL_TARGET): mt11-web.c Makefile $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	mkdir -p $(dir $@)
	$(HOST_CC) $(CFLAGS) $(APCAM_CPPFLAGS) $(SITL_DEFINES) $(SITL_EXTRA_DEFINES) -o $@ $< -lm

test: $(HOST_TEST) $(A8_HOST_CHECK)
	python3 tests/test_integration.py ./$(HOST_TEST) '$(TEST_ROOT)'
	python3 tests/test_a8_upgrade.py

clean:
	rm -f mt11-web a8-web zr10-web z1mini-web $(HOST_TEST) $(A8_HOST_CHECK)
	rm -rf tests/runtime

ZR10_CROSS_COMPILE ?= $(abspath ../build/zr10-deps/armv7-eabihf--uclibc--stable-2018.11-1/bin/arm-linux-)
zr10-web: build/zr10_upgrade.h zr10_firmware.h mt11-web.c ../camera_app/src/compat/zr10_uclibc.c $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS)
	$(ZR10_CROSS_COMPILE)gcc $(CFLAGS) -Wno-unused-function -Wno-missing-field-initializers -DAPCAM_TARGET=APCAM_TARGET_ZR10 -D_FILE_OFFSET_BITS=64 -o $@ $(filter %.c,$^)

.PHONY: zr10-test
zr10-test: $(VERSION_HEADER) build/zr10_upgrade.h
	python3 tests/test_zr10.py
	python3 tests/test_zr10_service.py

build/zr10_upgrade.h: ../tools/zr10_firmware.py ../packaging/zr10/upgrade_script.txt
	python3 ../tools/zr10_firmware.py header $@

Z1MINI_CROSS_COMPILE ?= arm-none-linux-gnueabihf-
Z1MINI_MAVLINK := ../camera_app/build/mavlink/all/include
z1mini-web: mt11-web.c z1mini_mavlink.h $(VERSION_HEADER) $(ICON_HEADER) $(APCAM_HEADERS) $(Z1MINI_MAVLINK)/all/mavlink.h
	$(Z1MINI_CROSS_COMPILE)gcc $(CFLAGS) -Wno-unused-function -Wno-address-of-packed-member -DAPCAM_TARGET=APCAM_TARGET_Z1_MINI -D_FILE_OFFSET_BITS=64 -I$(Z1MINI_MAVLINK) -o $@ $< -lm

$(Z1MINI_MAVLINK)/all/mavlink.h:
	$(MAKE) -C ../camera_app CAMERA_BACKEND=z1mini build/mavlink/all/include/all/mavlink.h

.PHONY: network-status-test
network-status-test: $(VERSION_HEADER) $(ICON_HEADER)
	python3 tests/test_network_status.py
