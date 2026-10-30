.DEFAULT_GOAL := all
include ../tools/build.mk

CAMERA_BACKEND ?= mt11
include ../include/apcam/targets.mk
SUPPORTED_BACKENDS := mt11 a8 zr10 zt30 z1mini rpi_libcam_caddx
ifeq ($(filter $(CAMERA_BACKEND),$(SUPPORTED_BACKENDS)),)
$(error Unsupported CAMERA_BACKEND '$(CAMERA_BACKEND)'; supported backends: $(SUPPORTED_BACKENDS))
endif

ifeq ($(CAMERA_BACKEND),rpi_libcam_caddx)
# Raspberry Pi: native build on Raspberry Pi OS, linked against the system
# libcamera. Cross builds supply CROSS_COMPILE and LIBCAMERA_* flags.
CROSS_COMPILE ?=
LDFLAGS ?=
TARGET := build/rpi_libcam_caddx/camera-app
PKG_CONFIG ?= pkg-config
LIBCAMERA_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libcamera)
LIBCAMERA_LIBS ?= $(shell $(PKG_CONFIG) --libs libcamera)
# Stills are encoded with libjpeg(-turbo): Raspberry Pi OS libjpeg-dev.
LIBJPEG_LIBS ?= -ljpeg
else ifneq ($(filter $(CAMERA_BACKEND),a8 zr10 zt30 z1mini),)
# ARMv7 targets: A8/Z1-Mini glibc and ZR10 uClibc.
# Per-backend object directories keep incompatible SDKs and libcs separate.
CROSS_COMPILE ?= arm-linux-
LDFLAGS ?=
TARGET := build/$(CAMERA_BACKEND)/camera-app
else
TARGET := camera-app
CROSS_COMPILE ?= aarch64-linux-gnu-
LDFLAGS ?= -static
endif
ifeq ($(CAMERA_BACKEND),zt30)
ifeq ($(ZT30_RAM_BUILD)$(ZT30_DEV_BUILD),11)
$(error ZT30_RAM_BUILD and ZT30_DEV_BUILD are mutually exclusive)
endif
ifeq ($(ZT30_RAM_BUILD),1)
TARGET := build/zt30-ram/camera-app
else ifeq ($(ZT30_DEV_BUILD),1)
TARGET := build/zt30-dev/camera-app
else
ifneq ($(filter all $(TARGET),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
$(error ZT30 native backend is not qualified yet; use make zt30_sitl at the repository root)
endif
endif
endif
CC := $(CROSS_COMPILE)gcc
HOST_CXX ?= c++
CXX := $(CROSS_COMPILE)g++
PYTHON ?= python3
ifeq ($(shell uname -s),Darwin)
override HOST_CXX += -include $(abspath ../include/apcam/posix_compat.h) -Wno-deprecated-declarations
endif

# MAVLink C bindings are generated from the message definitions in the
# modules/mavlink submodule (git submodule update --init --recursive).
MAVLINK_ROOT ?= ../modules/mavlink
MAVLINK_DIALECT ?= all
# Per-dialect output, so switching dialects rebuilds everything that includes it.
MAVLINK_GEN_DIR ?= build/mavlink/$(MAVLINK_DIALECT)/include
MAVLINK_XML := $(MAVLINK_ROOT)/message_definitions/v1.0/$(MAVLINK_DIALECT).xml
MAVGEN := $(MAVLINK_ROOT)/pymavlink/tools/mavgen.py
MAVGEN_SOURCES := $(wildcard $(MAVLINK_ROOT)/message_definitions/v1.0/*.xml) \
	$(wildcard $(MAVLINK_ROOT)/pymavlink/generator/*.py) \
	$(wildcard $(MAVLINK_ROOT)/pymavlink/generator/C/include_v2.0/*.h)
MAVLINK_HEADER := $(MAVLINK_GEN_DIR)/$(MAVLINK_DIALECT)/mavlink.h
# Records the dialect the objects were built for; rewritten only on change.
MAVLINK_STAMP := build/$(CAMERA_BACKEND)/mavlink-dialect
MAVLINK_DEPS := $(APCAM_HEADERS) $(MAVLINK_HEADER) $(MAVLINK_STAMP) include/camera_app/mavlink.h
MAVLINK_CPPFLAGS := -I$(MAVLINK_GEN_DIR) -DCA_MAVLINK_DIALECT=$(MAVLINK_DIALECT)

# rpi_libcam_caddx combines the Raspberry Pi media backend with the CADDX gimbal.
ifeq ($(CAMERA_BACKEND),rpi_libcam_caddx)
BACKEND_DIR := src/backends/rpi_libcam
CONTROL_DIR := src/backends/caddx
else
BACKEND_DIR := src/backends/$(CAMERA_BACKEND)
CONTROL_DIR := $(BACKEND_DIR)
endif
CPPFLAGS = -Ibuild $(APCAM_CPPFLAGS) -Iinclude -I$(BACKEND_DIR) $(MAVLINK_CPPFLAGS)
# Required language/runtime settings follow user flags, including command-line overrides.
override CXXFLAGS += -std=gnu++17 -fno-exceptions -fno-rtti -Wno-missing-field-initializers
APP_CXXFLAGS = $(filter-out -std=%,$(CFLAGS)) $(CXXFLAGS)
CFLAGS ?= -O2 -pipe -Wall -Wextra -Werror -std=c11
ifeq ($(CAMERA_BACKEND),zr10)
CFLAGS += -Wno-missing-field-initializers -D_FILE_OFFSET_BITS=64
endif
ifeq ($(CAMERA_BACKEND),z1mini)
CFLAGS += -D_FILE_OFFSET_BITS=64
TARGET_LINK_FLAGS := -Wl,--wrap=fcntl,--wrap=exp,--wrap=pow
endif
# header dependency tracking, so struct layout changes rebuild every user
DEPFLAGS := -MMD -MP

SS928_MPP_ROOT ?= $(abspath ../build/deps/ss928-mpp)
MINIMP4_ROOT ?= $(abspath ../build/deps/minimp4)
MINIMP4_HEADER := build/minimp4.h
SAMPLE_COMMON := $(SS928_MPP_ROOT)/src/common
VENDOR_LIB_DIR := $(SS928_MPP_ROOT)/lib/linux/hisilicon
# Explicit RTSP_ROOT overrides are already prepared trees (Windows supplies
# its POSIX compatibility copy this way). Otherwise build a private copy with
# real dependencies, leaving the pinned download unchanged.
ifeq ($(origin RTSP_ROOT),undefined)
RTSP_SOURCE_ROOT := $(SS928_MPP_ROOT)/src/rtspserver/src
RTSP_ROOT := $(abspath build/xop)
XOP_ENUM_ROOT := $(RTSP_SOURCE_ROOT)
XOP_INPUT_FILES := $(wildcard $(RTSP_SOURCE_ROOT)/net/* $(RTSP_SOURCE_ROOT)/xop/* $(RTSP_SOURCE_ROOT)/3rdpart/md5/*)
XOP_PREPARED_FILES := $(patsubst $(RTSP_SOURCE_ROOT)/%,$(RTSP_ROOT)/%,$(XOP_INPUT_FILES))
XOP_PREPARE_STAMP := $(RTSP_ROOT)/.prepared
XOP_PREPARE = $(PYTHON) ../tools/prepare_xop.py $(if $(filter Darwin,$(shell uname -s)),,--native) $(RTSP_SOURCE_ROOT) $(RTSP_ROOT)

$(XOP_PREPARE_STAMP): $(XOP_INPUT_FILES) ../tools/prepare_xop.py ../tools/xop_warnings.patch
	$(call progress,Preparing RTSP sources)
	$(XOP_PREPARE)
	touch $@

# Re-read each file's mtime after preparation. Unchanged source/header files
# retain their timestamps, so an edit rebuilds only their actual consumers.
$(XOP_PREPARED_FILES): $(XOP_PREPARE_STAMP)
	@test -f $@ || $(XOP_PREPARE)
else
XOP_ENUM_ROOT := $(RTSP_ROOT)
endif
SCENE_ROOT := $(SS928_MPP_ROOT)/src/scene_auto
XOP_VENDOR_LIB := $(SS928_MPP_ROOT)/lib/linux/3rdparty/libxoprtsp.a
XOP_COMPAT_LIB := build/libxoprtsp_compat.a
MEDIA_DEFINES := -Dss928v100 -DOT_XXXX -DISP_V2 -DOT_RELEASE \
	-DUSER_BIT_64 -DKERNEL_BIT_64 -fsigned-char
MEDIA_CPPFLAGS := $(CPPFLAGS) $(MEDIA_DEFINES) \
	-I$(SS928_MPP_ROOT)/include/hisilicon \
	-I$(SS928_MPP_ROOT)/include/hisilicon/exp_inc -I$(SAMPLE_COMMON) \
	-I$(SCENE_ROOT)/include -I$(SCENE_ROOT)/src/sample \
	-I$(SCENE_ROOT)/src/core -I$(SCENE_ROOT)/tools/configaccess/include \
	-I$(SS928_MPP_ROOT)/include/3rdparty
ISP_LIB_NAMES := ss_ae ss_isp ot_isp ss_awb ss_dehaze ss_extend_stats ss_drc \
	ss_ldci ss_crb ss_bnr ss_calcflicker ss_ir_auto ss_acs
OTHER_SENSOR_LIB_NAMES := sns_os08a20 sns_os05a10_2l_slave sns_imx347_slave \
	sns_imx347_2l_slave sns_imx485 sns_os04a10 sns_os08b10 sns_sc233hgs \
	sns_sc233hgs_2l sns_sc450ai sns_sc450ai_2l sns_sc850sl sns_imx662 \
	sns_imx662_2l sns_imx678_2l sns_imx219_2l sns_gs1860
MEDIA_LIBS := $(VENDOR_LIB_DIR)/libss_mpi.a $(VENDOR_LIB_DIR)/libsecurec.a \
	$(addprefix $(VENDOR_LIB_DIR)/lib,$(addsuffix .a,$(ISP_LIB_NAMES))) \
	$(addprefix $(VENDOR_LIB_DIR)/lib,$(addsuffix .a,$(OTHER_SENSOR_LIB_NAMES))) \
	$(VENDOR_LIB_DIR)/libss_upvqe.a $(VENDOR_LIB_DIR)/libss_dnvqe.a \
	$(VENDOR_LIB_DIR)/libss_voice_engine.a \
	$(SS928_MPP_ROOT)/lib/linux/3rdparty/libiniparser.a \
	$(XOP_COMPAT_LIB)

MISB_SOURCES := src/recording/misb.cpp src/recording/misb_ts.cpp
COMMON_SOURCES := $(if $(filter z1mini,$(CAMERA_BACKEND)),src/protocol/xfrobot_server.cpp) src/main.cpp \
	src/config.cpp \
	src/log.cpp src/recording/binlog.cpp src/recording/system_stats.cpp src/protocol/manual_control.cpp \
	src/media/autofocus.cpp src/media/media.cpp src/media/overlay.cpp src/media/tracking.cpp \
	src/protocol/mavlink.cpp \
	src/protocol/mavlink_server.cpp src/protocol/camera_definition.cpp src/protocol/camera_ftp.cpp \
	src/protocol/support_mavlink.cpp \
	src/protocol/network.cpp \
	src/recording/network_capture.cpp \
	src/streaming/support_video.cpp \
	src/protocol/external_uart.cpp \
	src/protocol/targeting.cpp src/protocol/survey.cpp src/protocol/terrain.cpp \
	src/protocol/siyi.cpp \
	src/protocol/siyi_server.cpp \
	$(if $(filter mt11 a8 zr10 zt30,$(CAMERA_BACKEND)),src/protocol/unigcs.cpp) \
	src/protocol/private_uart.cpp \
	src/recording/recorder_state.cpp \
	src/recording/raw_thermal.cpp src/streaming/thermal_stream.cpp \
	src/recording/still.cpp \
	src/recording/metadata.cpp \
	src/recording/video_metadata.cpp $(MISB_SOURCES) \
	$(CONTROL_DIR)/control.cpp
OBJECTS := $(COMMON_SOURCES:.cpp=.o) build/mt11_media.o build/mt11_overlay.o build/mt11_pipeline.o \
	build/mt11_e5739.o build/mt11_thermal.o build/mt11_isp.o \
	build/raw_thermal_server.o \
	build/live_video_server.o \
	build/mp4_minimp4.o \
	build/rtsp_xop.o build/xop_rtsp_connection.o build/xop_rtp_connection.o \
	build/xop_buffer_writer.o build/xop_socket_util.o \
	build/mt11_imx586.o \
	build/sample_comm_sys.o \
	build/sample_comm_isp.o build/sample_comm_vi.o build/sample_comm_vpss.o \
	build/scene_ot_confaccess.o build/scene_loadparam.o \
	build/scene_ot_scene.o build/scene_ot_scene_setparam.o \
	build/scene_setparam_inner.o
TEST_TARGET := tests/test_protocol
MAVLINK_TEST_TARGET := tests/test_mavlink
VIDEO_METADATA_TEST_TARGET := tests/test_video_metadata
MP4_TEST_TARGET := tests/test_mp4
MP4_SYNC_TEST_TARGET := tests/test_mp4_sync
MP4_ROLLOVER_TEST_TARGET := tests/test_mp4_rollover
E5739_TEST_TARGET := tests/test_e5739
THERMAL_TEST_TARGET := tests/test_thermal
RAW_THERMAL_TEST_TARGET := tests/test_raw_thermal
RAW_THERMAL_SERVER_TEST_TARGET := tests/test_raw_thermal_server
LIVE_VIDEO_SERVER_TEST_TARGET := tests/test_live_video_server
STILL_TEST_TARGET := tests/test_still
AUTOFOCUS_TEST_TARGET := tests/test_autofocus
CONFIG_TEST_TARGET := tests/test_config
TARGETING_TEST_TARGET := tests/test_targeting
MT11_IMX586_TEST_TARGET := tests/test_mt11_imx586
TEST_SOURCES := tests/test_protocol.cpp src/protocol/siyi.cpp src/protocol/private_uart.cpp
HOST_TARGET := tests/camera-app-host
A8_HOST_TARGET := tests/camera-app-a8-host
A8_HOST_SOURCES := $(filter-out src/backends/mt11/control.cpp \
	src/backends/a8/control.cpp,$(COMMON_SOURCES)) \
	src/backends/a8/control.cpp src/media/stub.cpp
# RPi target with stub media, for host tests of its MAVLink behaviour.
RPI_HOST_TARGET := tests/camera-app-rpi-host
RPI_HOST_SOURCES := $(filter-out $(CONTROL_DIR)/control.cpp src/protocol/unigcs.cpp,$(COMMON_SOURCES)) \
	src/backends/caddx/control.cpp src/media/stub.cpp
SITL_VARIANT := $(if $(filter mt11,$(CAMERA_BACKEND)),sitl,$(CAMERA_BACKEND)-sitl)
SITL_TARGET ?= ../build/$(SITL_VARIANT)/camera-app
SITL_OBJDIR ?= build/$(SITL_VARIANT)
SITL_EXTRA_CPPFLAGS ?=
SITL_SOURCES := $(COMMON_SOURCES) src/media/stub.cpp src/media/sitl_terrain.cpp \
	src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp
SITL_APP_OBJECTS := $(patsubst %.cpp,$(SITL_OBJDIR)/c/%.o,$(SITL_SOURCES))
XOP_HOST_SOURCES := $(patsubst $(XOP_ENUM_ROOT)/%,$(RTSP_ROOT)/%,\
	$(filter-out $(XOP_ENUM_ROOT)/xop/RtspConnection.cpp $(XOP_ENUM_ROOT)/xop/RtpConnection.cpp,\
	$(wildcard $(XOP_ENUM_ROOT)/net/*.cpp $(XOP_ENUM_ROOT)/xop/*.cpp)))
XOP_HOST_OBJECTS := $(patsubst $(RTSP_ROOT)/%.cpp,$(SITL_OBJDIR)/xop/%.o,\
	$(XOP_HOST_SOURCES)) $(SITL_OBJDIR)/xop/xop/RtspConnection.o \
	$(SITL_OBJDIR)/xop/xop/RtpConnection.o
SITL_RTSP_OBJECTS := $(SITL_OBJDIR)/rtsp_xop.o $(XOP_HOST_OBJECTS)
# Use the prepared XOP sources on MT11 as well. Demand tracking adds atomic
# playback state, so every object must share the updated class layout.
ifeq ($(CAMERA_BACKEND),mt11)
MT11_XOP_OBJECTS := $(patsubst $(RTSP_ROOT)/%.cpp,build/mt11-xop/%.o,\
    $(filter-out $(RTSP_ROOT)/net/BufferWriter.cpp $(RTSP_ROOT)/net/SocketUtil.cpp,$(XOP_HOST_SOURCES)))
OBJECTS += $(MT11_XOP_OBJECTS)
endif
# Our adapter includes vendor headers before compiler-generated .d files exist.
build/rtsp_xop.o build/$(CAMERA_BACKEND)/rtsp_xop.o $(SITL_OBJDIR)/rtsp_xop.o: | $(XOP_PREPARE_STAMP)

# Full-depth thermal codecs are only linked into the MT11 firmware/SITL.
THERMAL_CODECS_ROOT := $(abspath ../build/deps/thermal-codecs)
ifeq ($(CAMERA_BACKEND),mt11)
THERMAL_TARGET_LIBS := $(THERMAL_CODECS_ROOT)/aarch64/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/aarch64/lib/libavutil.a
THERMAL_HOST_LIBS := $(THERMAL_CODECS_ROOT)/host/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/host/lib/libavutil.a
src/streaming/thermal_stream.o: CPPFLAGS += -DCA_THERMAL_STREAM_FFV1 -I$(THERMAL_CODECS_ROOT)/aarch64/include
src/streaming/thermal_stream.o: $(THERMAL_TARGET_LIBS)
$(SITL_OBJDIR)/c/src/streaming/thermal_stream.o: CPPFLAGS += -DCA_THERMAL_STREAM_FFV1 -I$(THERMAL_CODECS_ROOT)/host/include
$(SITL_OBJDIR)/c/src/streaming/thermal_stream.o: $(THERMAL_HOST_LIBS)
# System libraries the static libav archives need (e.g. -lbcrypt on Cygwin),
# taken from the pkg-config files ffmpeg's configure installed.
thermal_extra_libs = $(filter-out -L% -lavcodec -lavutil,$(shell sed -n 's/^Libs\(\.private\)\{0,1\}: *//p' $(THERMAL_CODECS_ROOT)/$(1)/lib/pkgconfig/libavcodec.pc $(THERMAL_CODECS_ROOT)/$(1)/lib/pkgconfig/libavutil.pc 2>/dev/null))
THERMAL_TARGET_EXTRA_LIBS = $(call thermal_extra_libs,aarch64)
THERMAL_HOST_EXTRA_LIBS = $(call thermal_extra_libs,host)
endif
$(THERMAL_CODECS_ROOT)/host/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/host/lib/libavutil.a &: ../tools/build_thermal_codecs.sh
	+MAKE='$(MAKE)' ../tools/build_thermal_codecs.sh host
$(THERMAL_CODECS_ROOT)/aarch64/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/aarch64/lib/libavutil.a &: ../tools/build_thermal_codecs.sh
	+MAKE='$(MAKE)' THERMAL_CROSS_COMPILE='$(CROSS_COMPILE)' ../tools/build_thermal_codecs.sh aarch64

SSH_HOST ?= MT11
SSH ?= ssh
RSYNC_RSH ?= ssh
RSYNC ?= rsync
REMOTE_BIN ?= /app/bin/camera-app
REMOTE_CONFIG ?= /app/camera.ini
REMOTE_WEB ?= /app/bin/mt11-web
REMOTE_WEBROOT ?= /app/webroot
REMOTE_APP_INIT ?= /app/app_init.sh
REMOTE_APP_SELECTION ?= /app/app_selection.sh
REMOTE_TIMESYNC ?= /app/bin/mt11-timesync.sh
APP_INIT ?= ../packaging/mt11/app_init.sh
APP_SELECTION ?= ../packaging/mt11/app_selection.sh
TIMESYNC ?= ../packaging/mt11/mt11-timesync.sh
WEB_PORT ?= 80

.PHONY: all clean host sitl test install install-app install-web install-init FORCE

all: $(TARGET)

build/binlog_version.h: FORCE ../tools/write_build_version.py
	$(PYTHON) ../tools/write_build_version.py $@

# Depend on the generated header from consumers, not from an unmodified source
# file: an older source mtime must not conceal a changed firmware identity.
src/recording/binlog.o $(SITL_OBJDIR)/c/src/recording/binlog.o \
 build/$(CAMERA_BACKEND)/c/src/recording/binlog.o \
 build/z1mini-host/c/src/recording/binlog.o \
 build/zt30-sitl/c/src/backends/zt30/control.o \
 build/windows-zt30/c/src/backends/zt30/control.o: build/binlog_version.h

.PHONY: generated
generated: $(MAVLINK_HEADER) $(MINIMP4_HEADER) build/xop_rtsp_connection.cpp \
 build/xop_rtp_connection.cpp build/binlog_version.h


ifeq ($(CAMERA_BACKEND):$(filter 1,$(ZT30_RAM_BUILD) $(ZT30_DEV_BUILD)),zt30:1)
include ../tools/zt30_app.mk
else ifneq ($(filter $(CAMERA_BACKEND),a8 zr10 z1mini rpi_libcam_caddx),)
A8_XOP_OBJECTS := $(patsubst $(RTSP_ROOT)/%.cpp,build/$(CAMERA_BACKEND)/xop/%.o,\
	$(XOP_HOST_SOURCES)) build/$(CAMERA_BACKEND)/xop/xop/RtspConnection.o \
	build/$(CAMERA_BACKEND)/xop/xop/RtpConnection.o
ARM_APP_OBJECTS := $(patsubst %.cpp,build/$(CAMERA_BACKEND)/c/%.o,$(COMMON_SOURCES) \
	src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp \
	$(BACKEND_DIR)/media.cpp $(BACKEND_DIR)/pipeline.cpp $(if $(filter rpi_libcam_caddx,$(CAMERA_BACKEND)),$(BACKEND_DIR)/jpeg.cpp) $(if $(filter a8 zr10,$(CAMERA_BACKEND)),src/backends/siyi/overlay.cpp) $(if $(filter a8,$(CAMERA_BACKEND)),$(BACKEND_DIR)/isp.cpp) $(if $(filter zr10,$(CAMERA_BACKEND)),src/compat/zr10_uclibc.cpp) $(if $(filter z1mini,$(CAMERA_BACKEND)),$(BACKEND_DIR)/glibc_compat.cpp $(BACKEND_DIR)/native.cpp))
OBJECTS := $(ARM_APP_OBJECTS) build/$(CAMERA_BACKEND)/rtsp_xop.o $(A8_XOP_OBJECTS)

$(TARGET): Makefile $(OBJECTS)
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(CXX) $(APP_CXXFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) -Wl,--gc-sections \
		-Wl,--no-as-needed -lpthread -lm -ldl $(TARGET_LINK_FLAGS) $(if $(filter a8 zr10 z1mini,$(CAMERA_BACKEND)),-static-libstdc++ -static-libgcc) \
		$(if $(filter rpi_libcam_caddx,$(CAMERA_BACKEND)),$(LIBCAMERA_LIBS) $(LIBJPEG_LIBS))

build/rpi_libcam_caddx/c/src/backends/rpi_libcam/pipeline.o: CPPFLAGS += $(LIBCAMERA_CFLAGS)
# Current libcamera headers use std::span, which needs C++20; the rest stays C++17.
build/rpi_libcam_caddx/c/src/backends/rpi_libcam/pipeline.o: override CXXFLAGS += -std=gnu++20

build/$(CAMERA_BACKEND)/c/%.o: %.cpp
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-unused-function \
		-Wno-error=sign-compare -Wno-error=unused-variable \
		-Wno-error=implicit-fallthrough -ffunction-sections -fdata-sections \
		-c $< -o $@

build/$(CAMERA_BACKEND)/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/%.o: $(RTSP_ROOT)/%.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-I$(RTSP_ROOT)/3rdpart -include cstdint -O2 -pipe -Wall -Wextra \
		-Wno-error -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/xop/RtspConnection.o: build/xop_rtsp_connection.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/xop/RtpConnection.o: build/xop_rtp_connection.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@
else
$(TARGET): Makefile $(OBJECTS) $(XOP_COMPAT_LIB) $(THERMAL_TARGET_LIBS)
	$(call progress,Linking $@)
	$(CXX) $(APP_CXXFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) \
		-Wl,--start-group $(MEDIA_LIBS) -Wl,--end-group \
		-Wl,--gc-sections $(THERMAL_TARGET_LIBS) $(THERMAL_TARGET_EXTRA_LIBS) -lpthread -lm -ldl
endif

%.o: %.cpp
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(APP_CXXFLAGS) -c -o $@ $<

$(MAVLINK_XML) $(MAVGEN):
	@echo "error: $@ is missing; run 'git submodule update --init --recursive'" >&2
	@exit 1

$(MAVLINK_HEADER): $(MAVLINK_XML) $(MAVGEN) $(MAVGEN_SOURCES)
	$(call progress,Generating MAVLink $(MAVLINK_DIALECT) headers)
	mkdir -p $(MAVLINK_GEN_DIR)
	$(PYTHON) $(MAVGEN) --lang C --wire-protocol 2.0 \
		--output $(MAVLINK_GEN_DIR) $(MAVLINK_XML)

$(MAVLINK_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(MAVLINK_DIALECT)' | cmp -s - $@ || echo '$(MAVLINK_DIALECT)' > $@

src/protocol/mavlink.o src/protocol/mavlink_server.o src/protocol/support_mavlink.o: $(MAVLINK_DEPS)
$(SITL_OBJDIR)/c/src/protocol/mavlink.o \
	$(SITL_OBJDIR)/c/src/protocol/mavlink_server.o \
	$(SITL_OBJDIR)/c/src/protocol/support_mavlink.o: $(MAVLINK_DEPS)
build/$(CAMERA_BACKEND)/c/src/protocol/mavlink.o \
	build/$(CAMERA_BACKEND)/c/src/protocol/mavlink_server.o \
	build/$(CAMERA_BACKEND)/c/src/protocol/support_mavlink.o: $(MAVLINK_DEPS)

$(TEST_TARGET): $(TEST_SOURCES)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(TEST_SOURCES)

$(MAVLINK_TEST_TARGET): tests/test_mavlink.cpp src/protocol/mavlink.cpp \
                        $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -lm

tests/test_mavlink_time: tests/test_mavlink_time.cpp src/protocol/mavlink_server.cpp \
                        src/protocol/mavlink.cpp $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ $< src/protocol/mavlink.cpp -lm

.PHONY: unigcs-time-test
unigcs-time-test: build/test_unigcs_time
	./build/test_unigcs_time

build/test_unigcs_time: tests/test_unigcs_time.cpp src/protocol/unigcs.cpp \
                       src/protocol/private_uart.cpp src/protocol/siyi.cpp include/camera_app/private_uart.h \
                       include/camera_app/unigcs.h src/config.cpp $(APCAM_HEADERS)
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ $< src/protocol/private_uart.cpp src/protocol/siyi.cpp src/config.cpp -lm

tests/test_telemetry_time: tests/test_telemetry_time.cpp src/protocol/mavlink_server.cpp \
                          include/camera_app/telemetry_time.h src/protocol/targeting.cpp src/protocol/mavlink.cpp $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ $< src/protocol/targeting.cpp src/protocol/mavlink.cpp -lm

.PHONY: timezone-test
timezone-test: tests/test_timezone
	./tests/test_timezone

tests/test_timezone: tests/test_timezone.cpp $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $<

$(CONFIG_TEST_TARGET): tests/test_config.cpp src/config.cpp include/camera_app/config.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^)

tests/test_rpi_libcam_caddx: tests/test_rpi_libcam_caddx.cpp src/backends/caddx/control.cpp src/backends/caddx/caddx.h src/config.cpp src/log.cpp src/recording/binlog.cpp src/recording/system_stats.cpp src/backends/rpi_libcam/pipeline.h build/binlog_version.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) -Ibuild -I../include -Iinclude -DAPCAM_TARGET=APCAM_TARGET_RPI_LIBCAM_CADDX $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -lpthread -lm

.PHONY: rpi_libcam_caddx-test
rpi_libcam_caddx-test: tests/test_rpi_libcam_caddx
	./tests/test_rpi_libcam_caddx

# Needs libjpeg development files, so it is not part of "make test".
tests/test_rpi_jpeg: tests/test_rpi_jpeg.cpp src/backends/rpi_libcam/jpeg.cpp src/backends/rpi_libcam/jpeg.h src/log.cpp $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) -I../include -Iinclude -DAPCAM_TARGET=APCAM_TARGET_RPI_LIBCAM_CADDX $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -ljpeg -lm

.PHONY: rpi_jpeg-test
rpi_jpeg-test: tests/test_rpi_jpeg
	./tests/test_rpi_jpeg

tests/test_gimbal_rate: tests/test_gimbal_rate.cpp include/camera_app/gimbal_rate.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) -I../include -Iinclude -DAPCAM_TARGET=APCAM_TARGET_A8 $(APP_CXXFLAGS) -o $@ $< -lm

$(TARGETING_TEST_TARGET): tests/test_targeting.cpp src/protocol/targeting.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lpthread -lm

$(STILL_TEST_TARGET): tests/test_still.cpp src/recording/still.cpp \
		src/recording/metadata.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lpthread -lm

$(AUTOFOCUS_TEST_TARGET): tests/test_autofocus.cpp src/media/autofocus.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^

$(HOST_TARGET): build/binlog_version.h $(COMMON_SOURCES) src/media/stub.cpp $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -DCAMERA_APP_HOST -o $@ $(filter %.cpp %.o,$^) -lm -lpthread

$(A8_HOST_TARGET): CAMERA_BACKEND=a8
$(A8_HOST_TARGET): build/binlog_version.h $(A8_HOST_SOURCES) $(MAVLINK_DEPS) src/backends/siyi/control.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) \
		-o $@ $(filter-out src/backends/siyi/control.cpp,$(filter %.cpp %.o,$^)) -lm -lpthread

$(RPI_HOST_TARGET): CAMERA_BACKEND=rpi_libcam_caddx
$(RPI_HOST_TARGET): build/binlog_version.h $(RPI_HOST_SOURCES) $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter %.cpp %.o,$^) -lm -lpthread

build/mt11_overlay.o: src/backends/mt11/overlay.cpp
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(APP_CXXFLAGS) -c $< -o $@

build/mt11_media.o: $(BACKEND_DIR)/media.cpp $(BACKEND_DIR)/pipeline.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(APP_CXXFLAGS) -Wno-unused-function -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections -c $< -o $@

build/mt11_pipeline.o: $(BACKEND_DIR)/pipeline.cpp $(BACKEND_DIR)/pipeline.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(APP_CXXFLAGS) -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections -c $< -o $@

build/mt11_isp.o: $(BACKEND_DIR)/isp.cpp $(BACKEND_DIR)/isp.h
	mkdir -p build
	$(call progress,Linking $@)
	$(CXX) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(APP_CXXFLAGS) -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections \
		-c $< -o $@

build/mt11_e5739.o: $(BACKEND_DIR)/e5739.cpp $(BACKEND_DIR)/e5739.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/mt11_thermal.o: $(BACKEND_DIR)/thermal.cpp $(BACKEND_DIR)/thermal.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/raw_thermal_server.o: src/streaming/raw_thermal_server.cpp \
                              include/camera_app/raw_thermal_server.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/live_video_server.o: src/streaming/live_video_server.cpp \
                              include/camera_app/live_video_server.h \
                              include/camera_app/mp4.h
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(MINIMP4_HEADER): $(MINIMP4_ROOT)/minimp4.h src/recording/minimp4_metadata.patch ../tools/patch_source.py
	$(call progress,Generating $@)
	$(PYTHON) ../tools/patch_source.py --minimp4 $< src/recording/minimp4_metadata.patch $@

# Only the muxer includes minimp4; patch changes should not rebuild the
# otherwise independent camera control and media translation units.
build/$(CAMERA_BACKEND)/c/src/recording/mp4_minimp4.o \
 $(SITL_OBJDIR)/c/src/recording/mp4_minimp4.o \
 build/z1mini-host/c/src/recording/mp4_minimp4.o: $(MINIMP4_HEADER)

build/mp4_minimp4.o: src/recording/mp4_minimp4.cpp $(MINIMP4_HEADER)
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-unused-function \
		-Wno-error=sign-compare -Wno-error=unused-variable \
		-Wno-error=implicit-fallthrough -c $< -o $@

build/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/xop_rtsp_connection.cpp: $(RTSP_ROOT)/xop/RtspConnection.cpp src/streaming/xop_aggregate_setup.patch ../tools/patch_source.py
	$(call progress,Generating $@)
	$(PYTHON) ../tools/patch_source.py $< src/streaming/xop_aggregate_setup.patch $@

build/xop_rtsp_connection.o: build/xop_rtsp_connection.cpp
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/xop_rtp_connection.cpp: $(RTSP_ROOT)/xop/RtpConnection.cpp src/streaming/xop_satip_payload.patch ../tools/patch_source.py
	$(call progress,Generating $@)
	$(PYTHON) ../tools/patch_source.py $< src/streaming/xop_satip_payload.patch $@

build/xop_rtp_connection.o: build/xop_rtp_connection.cpp
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

# Replace the vendor archive's scalar-delete buffer and no-op TCP_NODELAY
# implementation with the same corrected sources used by SITL/ARM cameras.
build/mt11-xop/%.o: $(RTSP_ROOT)/%.cpp | $(XOP_PREPARE_STAMP)
	@mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-I$(RTSP_ROOT)/3rdpart -include cstdint -O2 -pipe -Wall -Wextra -Wno-error -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/xop_buffer_writer.o: $(RTSP_ROOT)/net/BufferWriter.cpp
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -O2 -pipe -Wall -Wextra -Werror -std=c++11 -c $< -o $@

build/xop_socket_util.o: $(RTSP_ROOT)/net/SocketUtil.cpp
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -O2 -pipe -Wall -Wextra -Werror -std=c++11 -c $< -o $@

$(XOP_COMPAT_LIB): $(XOP_VENDOR_LIB) Makefile
	mkdir -p build
	cp $< $@
	$(CROSS_COMPILE)ar d $@ RtspConnection.o
	$(CROSS_COMPILE)ar d $@ RtpConnection.o BufferWriter.o SocketUtil.o

build/mt11_imx586.o: $(BACKEND_DIR)/imx586.cpp $(BACKEND_DIR)/imx586.h \
		$(BACKEND_DIR)/imx586_mode0_registers.inc
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CXX) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections \
		-DMT11_IMX586_EXPORT_IMX678_ALIAS -c $< -o $@

build/sample_comm_%.o: $(SAMPLE_COMMON)/sample_comm_%.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -Wno-error -fno-aggressive-loop-optimizations \
		-ffunction-sections -fdata-sections $(if $(filter sample_comm_isp.c,$(notdir $<)),,-D_GNU_SOURCE) -c $< -o $@

build/scene_ot_confaccess.o: $(SCENE_ROOT)/tools/configaccess/src/ot_confaccess.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_loadparam.o: $(SCENE_ROOT)/src/sample/scene_loadparam.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_ot_scene.o: $(SCENE_ROOT)/src/core/ot_scene.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_ot_scene_setparam.o: $(SCENE_ROOT)/src/core/ot_scene_setparam.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_setparam_inner.o: $(SCENE_ROOT)/src/core/scene_setparam_inner.c
	mkdir -p build
	$(call progress,Compiling $(notdir $<))
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

host: $(HOST_TARGET)

.PHONY: thermal-mavlink-test
thermal-mavlink-test: $(HOST_TARGET) $(A8_HOST_TARGET)
	$(PYTHON) tests/test_thermal_mavlink.py ./$(HOST_TARGET) ../sitl/gimbal_sim.py thermal
	$(PYTHON) tests/test_thermal_mavlink.py ./$(A8_HOST_TARGET) ../sitl/gimbal_sim.py rgb

sitl: $(SITL_TARGET)

$(SITL_TARGET): $(SITL_APP_OBJECTS) $(SITL_RTSP_OBJECTS) $(THERMAL_HOST_LIBS) Makefile
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) -o $@ $(SITL_APP_OBJECTS) $(SITL_RTSP_OBJECTS) $(THERMAL_HOST_LIBS) $(THERMAL_HOST_EXTRA_LIBS) -lpthread -lm

$(SITL_OBJDIR)/c/%.o: %.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-error \
		-DCAMERA_APP_SITL -DCA_SITL_VIDEO_SCRIPT='"$(abspath ../sitl/terrain_video.py)"' -c $< -o $@

$(SITL_OBJDIR)/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/%.o: $(RTSP_ROOT)/%.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-I$(RTSP_ROOT)/3rdpart -include cstdint \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/xop/RtspConnection.o: build/xop_rtsp_connection.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/xop/RtpConnection.o: build/xop_rtp_connection.cpp
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

tests/test_support_mavlink: tests/test_support_mavlink.cpp src/protocol/support_mavlink.cpp \
                           src/protocol/mavlink.cpp src/config.cpp src/log.cpp $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -lpthread -lm

$(VIDEO_METADATA_TEST_TARGET): tests/test_video_metadata.cpp src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lpthread -lm

tests/test_misb: tests/test_misb.cpp src/recording/misb.cpp src/recording/misb_ts.cpp src/recording/video_metadata.cpp src/recording/metadata.cpp
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lpthread -lm

$(MP4_TEST_TARGET): tests/test_mp4.cpp src/recording/mp4_minimp4.cpp \
                    src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp \
                    $(MINIMP4_HEADER)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-error \
		-o $@ $(filter %.cpp,$^) -lpthread -lm

$(E5739_TEST_TARGET): tests/test_e5739.cpp $(BACKEND_DIR)/e5739.cpp src/log.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lm

$(MP4_SYNC_TEST_TARGET): tests/test_mp4_sync.cpp src/recording/mp4_minimp4.cpp \
                    src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp \
                    $(MINIMP4_HEADER)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-error \
		-o $@ $(filter %.cpp,$^) -Wl,--wrap=fdatasync -Wl,--wrap=fstatfs64 -lpthread -lm

$(MP4_ROLLOVER_TEST_TARGET): tests/test_mp4_rollover.cpp src/recording/mp4_minimp4.cpp \
                    src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp \
                    $(MINIMP4_HEADER)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-error \
		-o $@ $(filter %.cpp,$^) -Wl,--wrap=fstatfs64 -Wl,--wrap=pwrite64 -lpthread -lm

.PHONY: recording-test
recording-test: $(MP4_TEST_TARGET) $(MP4_SYNC_TEST_TARGET) $(MP4_ROLLOVER_TEST_TARGET)
	python3 tests/test_mp4_rollover.py --large

$(THERMAL_TEST_TARGET): tests/test_thermal.cpp $(BACKEND_DIR)/thermal.cpp src/log.cpp include/camera_app/thermal.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter %.cpp,$^) -lpthread

$(MT11_IMX586_TEST_TARGET): tests/test_mt11_imx586.cpp $(BACKEND_DIR)/imx586.cpp \
		$(BACKEND_DIR)/imx586.h $(BACKEND_DIR)/imx586_mode0_registers.inc
	$(call progress,Linking $@)
	$(HOST_CXX) $(MEDIA_CPPFLAGS) $(filter-out -static,$(APP_CXXFLAGS)) \
		-DMT11_IMX586_TEST_API -o $@ \
		$(BACKEND_DIR)/imx586.cpp tests/test_mt11_imx586.cpp

$(RAW_THERMAL_TEST_TARGET): tests/test_raw_thermal.cpp src/recording/raw_thermal.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^

$(RAW_THERMAL_SERVER_TEST_TARGET): tests/test_raw_thermal_server.cpp \
                                   src/streaming/raw_thermal_server.cpp src/log.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $^ -lpthread

$(LIVE_VIDEO_SERVER_TEST_TARGET): tests/test_live_video_server.cpp \
                                  src/streaming/live_video_server.cpp \
                                  src/recording/mp4_minimp4.cpp src/log.cpp \
                                  src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp \
                                  $(MINIMP4_HEADER)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-error \
		-o $@ tests/test_live_video_server.cpp \
		src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp \
		src/log.cpp src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp -lpthread -lm

test: tests/test_misb survey-test rpi_libcam_caddx-test timezone-test system-stats-test thermal-mavlink-test media-overlay-test z1-overlay-race-test manual-control-test tests/test_gimbal_rate tests/test_mavlink_time tests/test_telemetry_time tests/test_support_mavlink $(VIDEO_METADATA_TEST_TARGET) $(TEST_TARGET) $(MAVLINK_TEST_TARGET) $(MP4_TEST_TARGET) $(E5739_TEST_TARGET) \
	$(MP4_SYNC_TEST_TARGET) \
	$(MP4_ROLLOVER_TEST_TARGET) \
	$(THERMAL_TEST_TARGET) $(RAW_THERMAL_TEST_TARGET) \
	$(RAW_THERMAL_SERVER_TEST_TARGET) $(STILL_TEST_TARGET) \
	$(LIVE_VIDEO_SERVER_TEST_TARGET) \
	$(AUTOFOCUS_TEST_TARGET) $(CONFIG_TEST_TARGET) $(TARGETING_TEST_TARGET) \
	$(MT11_IMX586_TEST_TARGET) \
	$(HOST_TARGET) $(A8_HOST_TARGET) $(RPI_HOST_TARGET)
	./$(TEST_TARGET)
	./$(MAVLINK_TEST_TARGET)
	./tests/test_telemetry_time
	./tests/test_mavlink_time
	./tests/test_gimbal_rate
	./tests/test_support_mavlink
	python3 ../sitl/test_support_video_queue.py
	python3 ../tests/test_targets.py
	python3 ../tests/test_a8_service.py
	./$(VIDEO_METADATA_TEST_TARGET)
	./tests/test_misb
	./$(CONFIG_TEST_TARGET) camera.ini
	./$(TARGETING_TEST_TARGET)
	./$(E5739_TEST_TARGET)
	./$(THERMAL_TEST_TARGET)
	./$(RAW_THERMAL_TEST_TARGET)
	./$(RAW_THERMAL_SERVER_TEST_TARGET)
	./$(STILL_TEST_TARGET)
	./$(AUTOFOCUS_TEST_TARGET)
	./$(MT11_IMX586_TEST_TARGET)
	python3 tests/test_integration.py ./$(HOST_TARGET)
	python3 tests/test_a8_attitude.py ./$(A8_HOST_TARGET)
	python3 tests/test_rpi_video_uri.py ./$(RPI_HOST_TARGET)
	python3 tests/test_a8_delegated.py ./$(A8_HOST_TARGET)
	ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=10 \
		-t 2.3 -c:v libx264 -x264-params aud=1:bframes=0:slices=4 \
		-pix_fmt yuv420p -f h264 -y tests/test.h264
	./$(LIVE_VIDEO_SERVER_TEST_TARGET) tests/test.h264
	./$(MP4_TEST_TARGET) tests/test.h264 tests/test.mp4
	./$(MP4_SYNC_TEST_TARGET) tests/test.h264
	./$(MP4_SYNC_TEST_TARGET) tests/test.h264 misb
	python3 tests/test_mp4_rollover.py
	grep -aq 'tfdt' tests/test.mp4.fragmented
	ffprobe -v error -select_streams v:0 -count_frames \
		-show_entries stream=codec_name,width,height,nb_read_frames \
		-of default=noprint_wrappers=1 tests/test.mp4 \
		2>tests/test.ffprobe.err | grep -qx 'codec_name=h264'
	test ! -s tests/test.ffprobe.err
	ffprobe -v error -select_streams v:0 -count_frames \
		-show_entries stream=codec_name,width,height,nb_read_frames \
		-of default=noprint_wrappers=1 tests/test.mp4.fragmented \
		2>tests/test.fragmented.ffprobe.err | grep -qx 'codec_name=h264'
	test ! -s tests/test.fragmented.ffprobe.err
	rm -f tests/test.h264 tests/test.mp4 tests/test.mp4.fragmented \
		tests/test.ffprobe.err tests/test.fragmented.ffprobe.err

install: all test install-app install-web install-init

install-app: $(TARGET)
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(TARGET) $(SSH_HOST):$(REMOTE_BIN).new
	$(SSH) $(SSH_HOST) 'set -eu; chmod 0755 $(REMOTE_BIN).new; mv $(REMOTE_BIN).new $(REMOTE_BIN)'
	rsync -av --ignore-existing --chmod=F644 -e '$(RSYNC_RSH)' camera.ini $(SSH_HOST):$(REMOTE_CONFIG)

install-web:
	$(MAKE) -C ../web
	$(RSYNC) -av --delay-updates --delete-delay --chmod=D755,F644 -e '$(RSYNC_RSH)' ../web/webroot/ $(SSH_HOST):$(REMOTE_WEBROOT)/
	$(RSYNC) -av --chmod=F755 -e '$(RSYNC_RSH)' ../web/mt11-web $(SSH_HOST):$(REMOTE_WEB).new
	$(SSH) $(SSH_HOST) 'set -eu; chmod 0755 $(REMOTE_WEB).new; mv $(REMOTE_WEB).new $(REMOTE_WEB); sed -i "s/mt11-web -p 8080/mt11-web -p $(WEB_PORT)/g" /app/app_init.sh; pids=$$(ps | awk '\''/mt11-web -p (80|8080)/ && !/awk/ {print $$1}'\''); for pid in $$pids; do kill -TERM "$$pid"; done; sleep 1; $(REMOTE_WEB) -p $(WEB_PORT) >/run/mt11-web.log 2>&1 &'

install-init: $(APP_INIT) $(APP_SELECTION) $(TIMESYNC)
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(TIMESYNC) $(SSH_HOST):$(REMOTE_TIMESYNC).new
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(APP_SELECTION) $(SSH_HOST):$(REMOTE_APP_SELECTION).new
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(APP_INIT) $(SSH_HOST):$(REMOTE_APP_INIT).new
	$(SSH) $(SSH_HOST) 'set -eu; chmod 0755 $(REMOTE_TIMESYNC).new $(REMOTE_APP_SELECTION).new $(REMOTE_APP_INIT).new; /bin/fsync $(REMOTE_TIMESYNC).new; /bin/fsync $(REMOTE_APP_SELECTION).new; /bin/fsync $(REMOTE_APP_INIT).new; mv $(REMOTE_TIMESYNC).new $(REMOTE_TIMESYNC); /bin/fsync /app/bin; test -x $(REMOTE_TIMESYNC); mv $(REMOTE_APP_SELECTION).new $(REMOTE_APP_SELECTION); /bin/fsync /app; test -x $(REMOTE_APP_SELECTION); mv $(REMOTE_APP_INIT).new $(REMOTE_APP_INIT); /bin/fsync /app; test -x $(REMOTE_APP_INIT); sha256sum $(REMOTE_TIMESYNC) $(REMOTE_APP_SELECTION) $(REMOTE_APP_INIT)'

clean:
	rm -f tests/test_misb
	rm -f tests/test_gimbal_rate
	rm -f tests/test_telemetry_time tests/test_mavlink_time
	rm -f tests/test_support_mavlink tests/test_thermal_monitor $(MP4_SYNC_TEST_TARGET) $(MP4_ROLLOVER_TEST_TARGET)
	rm -f tests/test_system_stats tests/test_binlog_sys tests/test_manual_control $(TARGET) $(COMMON_SOURCES:.cpp=.o) $(COMMON_SOURCES:.cpp=.d) $(TEST_TARGET) $(MAVLINK_TEST_TARGET) $(MP4_TEST_TARGET) $(E5739_TEST_TARGET) $(THERMAL_TEST_TARGET) $(RAW_THERMAL_TEST_TARGET) $(RAW_THERMAL_SERVER_TEST_TARGET) $(LIVE_VIDEO_SERVER_TEST_TARGET) $(VIDEO_METADATA_TEST_TARGET) $(STILL_TEST_TARGET) $(AUTOFOCUS_TEST_TARGET) $(CONFIG_TEST_TARGET) $(TARGETING_TEST_TARGET) $(MT11_IMX586_TEST_TARGET) $(HOST_TARGET) $(A8_HOST_TARGET) $(RPI_HOST_TARGET)
	rm -f tests/test.h264 tests/test.mp4 tests/test.mp4.fragmented \
		tests/test.ffprobe.err tests/test.fragmented.ffprobe.err
	rm -rf build

-include $(OBJECTS:.o=.d) $(SITL_APP_OBJECTS:.o=.d) $(SITL_RTSP_OBJECTS:.o=.d)

# Backend wrapper includes the common SIYI control implementation.
src/backends/a8/control.o src/backends/zr10/control.o src/backends/zt30/control.o: src/backends/siyi/control.cpp

ZR10_HOST_TARGET := tests/camera-app-zr10-host
$(ZR10_HOST_TARGET): CAMERA_BACKEND=zr10
$(ZR10_HOST_TARGET): build/binlog_version.h $(filter-out src/backends/mt11/control.cpp src/backends/a8/control.cpp src/backends/zr10/control.cpp,$(COMMON_SOURCES)) src/backends/zr10/control.cpp src/backends/siyi/control.cpp src/media/stub.cpp $(MAVLINK_DEPS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $(filter-out src/backends/siyi/control.cpp,$(filter %.cpp %.o,$^)) -lm -lpthread

.PHONY: zr10-test
zr10-test: $(ZR10_HOST_TARGET)
	python3 tests/test_zr10.py ./$(ZR10_HOST_TARGET)
	python3 tests/test_zr10_pipeline.py

.PHONY: mcu-uart-test
mcu-uart-test: $(HOST_TARGET) $(A8_HOST_TARGET) $(ZR10_HOST_TARGET)
	$(PYTHON) tests/test_mcu_uart.py ./$(A8_HOST_TARGET) a8
	$(PYTHON) tests/test_mcu_uart.py ./$(ZR10_HOST_TARGET) zr10
	$(PYTHON) tests/test_mcu_uart.py ./$(HOST_TARGET) mt11

test: mcu-uart-test

# Optional target-runtime verification using a separately extracted stock rootfs.
ZR10_QEMU ?= qemu-arm
ZR10_ROOTFS ?=
.PHONY: zr10-runtime-test
zr10-runtime-test: $(TARGET)
	test '$(CAMERA_BACKEND)' = zr10
	test -n '$(ZR10_ROOTFS)'
	$(call progress,Linking $@)
	$(CXX) $(APP_CXXFLAGS) tests/test_zr10_ctype.cpp src/compat/zr10_uclibc.cpp -o build/zr10/test_ctype
	$(ZR10_QEMU) -L '$(ZR10_ROOTFS)' -E LD_BIND_NOW=1 build/zr10/test_ctype
	$(ZR10_QEMU) -L '$(ZR10_ROOTFS)' -E LD_BIND_NOW=1 $(TARGET) --help

ifeq ($(CAMERA_BACKEND),z1mini)
# Exercise the real Z1 media receiver with a local RTSP fixture and PTY MCU.
Z1_HOST_TARGET := build/z1mini-host/camera-app
Z1_HOST_SOURCES := $(COMMON_SOURCES) src/backends/z1mini/media.cpp \
	src/backends/z1mini/pipeline.cpp src/backends/z1mini/native.cpp src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp
Z1_HOST_OBJECTS := $(patsubst %.cpp,build/z1mini-host/c/%.o,$(Z1_HOST_SOURCES))
.PHONY: z1mini-host
z1mini-host: $(Z1_HOST_TARGET)
$(Z1_HOST_TARGET): $(Z1_HOST_OBJECTS) $(SITL_RTSP_OBJECTS)
	$(call progress,Linking $@)
	$(HOST_CXX) -o $@ $^ -lpthread -lm
build/z1mini-host/c/%.o: %.cpp $(MAVLINK_DEPS)
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(APP_CXXFLAGS) -Wno-unused-function -Wno-error=sign-compare -Wno-error=unused-variable -Wno-error=implicit-fallthrough -DCA_Z1_TEST -DCAMERA_APP_HOST -c $< -o $@
-include $(Z1_HOST_OBJECTS:.o=.d)

endif

# Native Z1 capture uses the public AX620A BSP headers at build time, and the
# camera's installed SDK shared libraries at runtime. Headers are not packaged.
Z1MINI_AX_SDK_INCLUDE ?=
.PHONY: z1mini-capture
z1mini-capture: build/z1mini/ax-capture
.PHONY: z1mini-overlay-test
z1mini-overlay-test: media-overlay-test z1-overlay-race-test
	@test -n "$(Z1MINI_AX_SDK_INCLUDE)" || { echo 'Set Z1MINI_AX_SDK_INCLUDE to AX620A msp/out/include' >&2; exit 1; }
	mkdir -p build/tests
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -ffunction-sections -fdata-sections -Wl,--gc-sections -I../include -DAPCAM_TARGET=APCAM_TARGET_Z1_MINI -Iinclude -I'$(Z1MINI_AX_SDK_INCLUDE)' -Isrc/backends/z1mini tests/test_z1mini_overlay.cpp src/media/overlay.cpp -o build/tests/test_z1mini_overlay -ldl -pthread -lm
	./build/tests/test_z1mini_overlay

build/z1mini/ax-capture: src/backends/z1mini/ax_capture.cpp Makefile src/media/overlay.cpp include/camera_app/overlay.h src/backends/z1mini/ax_config.h src/backends/z1mini/native.h include/camera_app/exposure.h $(APCAM_HEADERS)
	@test -n "$(Z1MINI_AX_SDK_INCLUDE)" || { echo 'Set Z1MINI_AX_SDK_INCLUDE to AX620A msp/out/include' >&2; exit 1; }
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(CXX) -O2 -g -Wall -Wextra -Werror $(CXXFLAGS) $(APCAM_CPPFLAGS) -Iinclude -I'$(Z1MINI_AX_SDK_INCLUDE)' -Isrc/backends/z1mini $< src/media/overlay.cpp -o $@ -static-libstdc++ -static-libgcc -ldl -pthread -lm

.PHONY: thermal-monitor-test
thermal-monitor-test: tests/test_thermal_monitor
	$(PYTHON) -c 'import subprocess; subprocess.run(["./tests/test_thermal_monitor"], check=True, timeout=10)'

tests/test_thermal_monitor: src/media/tracking.cpp build/host/tracking_dlib.o tests/test_thermal_monitor.cpp src/media/media.cpp include/camera_app/media.h include/camera_app/binlog.h include/camera_app/exposure.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tests/test_thermal_monitor.cpp src/media/media.cpp src/media/tracking.cpp build/host/tracking_dlib.o -lpthread -lm

.PHONY: exposure-test
exposure-test: a8-exposure-test thermal-monitor-test tests/test_exposure tests/test_exposure_native
	./tests/test_exposure
	$(PYTHON) -c 'import subprocess; subprocess.run(["./tests/test_exposure_native"], check=True, timeout=10)'

tests/test_exposure: tests/test_exposure.cpp include/camera_app/sigmastar_exposure.h include/camera_app/exposure.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ $< -lm

tests/test_exposure_native: tests/test_z1mini_native.cpp src/backends/z1mini/native.cpp src/backends/z1mini/native.h include/camera_app/exposure.h src/log.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -o $@ tests/test_z1mini_native.cpp src/backends/z1mini/native.cpp src/log.cpp -lpthread -lm

.PHONY: manual-control-test
manual-control-test: tests/test_manual_control
	./tests/test_manual_control

.PHONY: mavlink-components-test
mavlink-components-test: host
	$(PYTHON) tests/test_mavlink_components.py ./$(HOST_TARGET) ../sitl/gimbal_sim.py

tests/test_manual_control: tests/test_manual_control.cpp src/protocol/manual_control.cpp include/camera_app/manual_control.h ../include/apcam/manual_control.h include/camera_app/backend.h
	$(call progress,Linking $@)
	$(HOST_CXX) -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -DAPCAM_TARGET=APCAM_TARGET_MT11 -I../include -Iinclude -o $@ $< -lm

.PHONY: media-overlay-test
media-overlay-test: build/host/tracking_dlib.o
	mkdir -p build/tests
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -ffunction-sections -fdata-sections -Wl,--gc-sections -I../include -DAPCAM_TARGET=APCAM_TARGET_MT11 -Iinclude tests/test_media_overlay.cpp src/media/tracking.cpp build/host/tracking_dlib.o -o build/tests/test_media_overlay -pthread -lm
	./build/tests/test_media_overlay

.PHONY: z1-overlay-race-test
z1-overlay-race-test: build/tests/test_z1_overlay_race
	$(PYTHON) -c 'import subprocess; subprocess.run(["./build/tests/test_z1_overlay_race"], check=True, timeout=10)'

build/tests/test_z1_overlay_race: tests/test_z1_overlay_race.cpp src/backends/z1mini/native.cpp src/backends/z1mini/native.h
	mkdir -p $(dir $@)
	# Check the production translation unit without headers supplied by the harness.
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -I../include -Iinclude -fsyntax-only src/backends/z1mini/native.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -I../include -Iinclude -o $@ $< -lpthread

.PHONY: a8-zoom-test
test: a8-zoom-test
a8-zoom-test: build/tests/test_a8_zoom
	./build/tests/test_a8_zoom

build/tests/test_a8_zoom: tests/test_a8_zoom.cpp src/backends/a8/pipeline.cpp src/backends/a8/pipeline.h src/backends/siyi/overlay.cpp $(wildcard src/backends/a8/mi/star/*.h) $(wildcard ../include/apcam/*.h)
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -Wno-unused-function -Wno-sign-compare -Wno-unused-variable -ffunction-sections -fdata-sections -DAPCAM_TARGET=APCAM_TARGET_A8 -I../include -Iinclude -Isrc/backends/a8 -o $@ $< -Wl,--gc-sections -ldl

.PHONY: a8-exposure-test
a8-exposure-test: build/tests/test_a8_exposure
	./build/tests/test_a8_exposure

build/tests/test_a8_exposure: tests/test_a8_exposure.cpp src/backends/a8/isp.cpp src/backends/a8/isp.h include/camera_app/config.h
	mkdir -p $(dir $@)
	$(call progress,Compiling $(notdir $<))
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -I../include -Iinclude -fsyntax-only src/backends/a8/isp.cpp
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -Wall -Wextra -Werror -std=gnu++17 -Wno-missing-field-initializers -I../include -Iinclude -o $@ $< -ldl

.PHONY: system-stats-test
.PHONY: network-capture-test
network-capture-test:
	$(PYTHON) tests/test_network_capture.py --no-permission
	sudo unshare -n $(PYTHON) tests/test_network_capture.py

.PHONY: mavlink-binlog-test
mavlink-binlog-test: $(HOST_TARGET)
	$(PYTHON) tests/test_mavlink_binlog.py ./$(HOST_TARGET) ../sitl/gimbal_sim.py

.PHONY: vendor-binlog-test
vendor-binlog-test: $(HOST_TARGET) $(A8_HOST_TARGET) $(ZR10_HOST_TARGET) build/test_packet_binlog
	$(MAKE) CAMERA_BACKEND=z1mini HOST_TARGET=build/vendor-test-z1mini host
	$(PYTHON) tests/test_vendor_packets.py --probe ./build/test_packet_binlog --mt11 ./$(HOST_TARGET) --a8 ./$(A8_HOST_TARGET) --zr10 ./$(ZR10_HOST_TARGET) --z1mini ./build/vendor-test-z1mini

build/test_packet_binlog: tests/test_packet_binlog.cpp src/recording/binlog.cpp src/recording/system_stats.cpp src/config.cpp src/protocol/siyi.cpp src/protocol/private_uart.cpp build/binlog_version.h include/camera_app/binlog.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -DCAMERA_APP_HOST -o $@ $(filter %.cpp,$^) -lpthread -lm

system-stats-test: tests/test_system_stats tests/test_binlog_sys
	./tests/test_system_stats
	$(PYTHON) tests/test_binlog_sys.py ./tests/test_binlog_sys

tests/test_system_stats: tests/test_system_stats.cpp src/recording/system_stats.cpp include/camera_app/system_stats.h include/camera_app/binlog.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) -I../include -DAPCAM_TARGET=APCAM_TARGET_MT11 -Iinclude $(APP_CXXFLAGS) -o $@ $< -lm

tests/test_binlog_sys: tests/test_binlog_sys.cpp src/recording/binlog.cpp src/recording/system_stats.cpp src/config.cpp build/binlog_version.h include/camera_app/system_stats.h include/camera_app/binlog.h $(APCAM_HEADERS)
	$(call progress,Linking $@)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -DCAMERA_APP_HOST -o $@ $(filter %.cpp,$^) -lpthread -lm

# Shared schema and C++ ownership changes must invalidate host regression binaries.
$(TEST_TARGET) tests/test_config tests/test_thermal_monitor build/tests/test_a8_exposure: $(wildcard ../include/apcam/*.h) $(wildcard include/camera_app/APC_*.h)

# Standalone probes: safe to run without owning the MT11 camera hardware.
.PHONY: thermal-codec-bench
thermal-codec-bench: build/thermal-codec-bench build/mt11/thermal-codec-bench
build/thermal-codec-bench: tests/thermal_codec_bench.cpp src/streaming/thermal_codec.h $(THERMAL_HOST_LIBS)
	mkdir -p $(@D)
	$(call progress,Linking $@)
	$(HOST_CXX) -O2 -std=c++17 -Wall -Wextra -Werror -I$(THERMAL_CODECS_ROOT)/host/include -o $@ $< $(THERMAL_HOST_LIBS) $(THERMAL_HOST_EXTRA_LIBS) -lpthread -lm
build/mt11/thermal-codec-bench: tests/thermal_codec_bench.cpp src/streaming/thermal_codec.h $(THERMAL_TARGET_LIBS)
	mkdir -p $(@D)
	$(call progress,Linking $@)
	aarch64-linux-gnu-g++ -O2 -std=c++17 -Wall -Wextra -Werror -static -I$(THERMAL_CODECS_ROOT)/aarch64/include -o $@ $< $(THERMAL_TARGET_LIBS) $(THERMAL_TARGET_EXTRA_LIBS) -lpthread -lm

.PHONY: rtsp-demand-test
rtsp-demand-test: build/tests/test_rtsp_demand
	./build/tests/test_rtsp_demand

build/tests/test_rtsp_demand: tests/test_rtsp_demand.cpp src/streaming/rtsp_xop.cpp \
 $(XOP_HOST_OBJECTS) src/recording/video_metadata.cpp $(MISB_SOURCES) src/recording/metadata.cpp \
 src/streaming/support_video.cpp src/log.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -std=gnu++17 -o $@ $(filter-out src/streaming/rtsp_xop.cpp,$^) -lpthread -lm

test: rtsp-demand-test

# ASan detects the vendor's old new[]/scalar-delete mismatch on both queue
# draining and connection shutdown; TCP_NODELAY verifies the socket fix.
.PHONY: xop-test
xop-test: build/tests/test_xop_buffer
	./build/tests/test_xop_buffer
	$(PYTHON) ../tests/test_xop_preparation.py

build/tests/test_xop_buffer: tests/test_xop_buffer.cpp $(RTSP_ROOT)/net/BufferWriter.cpp $(RTSP_ROOT)/net/SocketUtil.cpp $(RTSP_ROOT)/net/BufferWriter.h $(RTSP_ROOT)/net/SocketUtil.h $(RTSP_ROOT)/net/Socket.h
	mkdir -p $(dir $@)
	$(call progress,Linking $@)
	$(HOST_CXX) -O1 -g -fsanitize=address -fno-omit-frame-pointer -std=c++11 -Wall -Wextra -Werror -I$(RTSP_ROOT) $(filter %.cpp,$^) -o $@

# Isolate dlib exceptions from the rest of the firmware. Compile only its
# correlation tracker and FFT, without optional GUI/BLAS/CUDA dependencies.
DLIB_ROOT ?= $(abspath ../build/deps/dlib)
DLIB_HEADER := $(DLIB_ROOT)/dlib/image_processing/correlation_tracker.h
# This belongs to a verified dependency tree, not Make's generated outputs.
.PRECIOUS: $(DLIB_HEADER)
generated: $(DLIB_HEADER)
$(DLIB_HEADER):
	sh ../tools/bootstrap_dlib.sh $(DLIB_ROOT)
DLIB_FLAGS = -O2 -std=gnu++17 -fexceptions -fno-rtti -DDLIB_NO_GUI_SUPPORT -DDLIB_DISABLE_ASSERTS -Iinclude -isystem $(DLIB_ROOT)
build/host/tracking_dlib.o: src/media/tracking_dlib.cpp include/camera_app/image_tracker.h $(DLIB_HEADER)
	mkdir -p $(@D)
	$(HOST_CXX) $(DEPFLAGS) $(DLIB_FLAGS) -c $< -o $@
build/$(CAMERA_BACKEND)/tracking_dlib.o: src/media/tracking_dlib.cpp include/camera_app/image_tracker.h $(DLIB_HEADER)
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(DLIB_FLAGS) -c $< -o $@
ifneq ($(CAMERA_BACKEND):$(filter 1,$(ZT30_RAM_BUILD) $(ZT30_DEV_BUILD)),zt30:1)
OBJECTS += build/$(CAMERA_BACKEND)/tracking_dlib.o
$(TARGET): build/$(CAMERA_BACKEND)/tracking_dlib.o
endif
$(SITL_OBJDIR)/tracking_dlib.o: src/media/tracking_dlib.cpp include/camera_app/image_tracker.h $(DLIB_HEADER)
	mkdir -p $(@D)
	$(HOST_CXX) $(DEPFLAGS) $(DLIB_FLAGS) -c $< -o $@
SITL_APP_OBJECTS += $(SITL_OBJDIR)/tracking_dlib.o
$(SITL_TARGET): $(SITL_OBJDIR)/tracking_dlib.o
$(HOST_TARGET) $(A8_HOST_TARGET) $(ZR10_HOST_TARGET) $(RPI_HOST_TARGET): build/host/tracking_dlib.o
Z1_HOST_OBJECTS += build/host/tracking_dlib.o
$(Z1_HOST_TARGET): build/host/tracking_dlib.o
-include build/host/tracking_dlib.d build/$(CAMERA_BACKEND)/tracking_dlib.d $(SITL_OBJDIR)/tracking_dlib.d

.PHONY: image-tracking-test
image-tracking-test: build/tests/test_image_tracker
	./build/tests/test_image_tracker
image-tracking-test: tracking-lifetime-test mt11-overlay-test
test: image-tracking-test
build/tests/test_image_tracker: tests/test_image_tracker.cpp include/camera_app/image_tracker.h include/camera_app/tracking_pose.h include/camera_app/tracking_rate.h build/host/tracking_dlib.o
	mkdir -p $(@D)
	$(HOST_CXX) -O2 -std=gnu++17 -Wall -Wextra -Werror -Iinclude -o $@ $< build/host/tracking_dlib.o -lpthread -lm

.PHONY: tracking-lifetime-test mt11-overlay-test
tracking-lifetime-test: build/tests/test_tracking_lifetime
	./build/tests/test_tracking_lifetime
build/tests/test_tracking_lifetime: tests/test_tracking_lifetime.cpp tests/test_media_backend.h src/media/tracking.cpp include/camera_app/APC_Tracking.h build/host/tracking_dlib.o
	mkdir -p $(@D)
	$(HOST_CXX) -O1 -g -fsanitize=address -fno-omit-frame-pointer -std=gnu++17 -Wall -Wextra -Werror -Iinclude -I../include -DAPCAM_TARGET=APCAM_TARGET_MT11 -Wl,--wrap=pthread_create -o $@ $(filter %.cpp %.o,$^) -lpthread -lm
mt11-overlay-test: build/tests/test_mt11_overlay
	./build/tests/test_mt11_overlay
build/tests/test_mt11_overlay: tests/test_mt11_overlay.cpp src/backends/mt11/overlay.cpp src/media/overlay.cpp include/camera_app/overlay.h
	mkdir -p $(@D)
	$(HOST_CXX) -O2 -std=gnu++17 -Wall -Wextra -Werror -Wno-missing-field-initializers -Iinclude -I../include -I$(SS928_MPP_ROOT)/include/hisilicon -DAPCAM_TARGET=APCAM_TARGET_MT11 -o $@ $(filter %.cpp,$^)

tests/test_survey: tests/test_survey.cpp src/protocol/survey.cpp src/protocol/targeting.cpp src/config.cpp include/camera_app/survey.h
	$(HOST_CXX) $(APCAM_CPPFLAGS) -Iinclude -std=gnu++17 -Wall -Wextra -Werror $(filter %.cpp,$^) -o $@ -pthread

survey-test: tests/test_survey
	./tests/test_survey

.PHONY: survey-lifecycle-test
survey-lifecycle-test: build/tests/test_survey_lifecycle
	$(PYTHON) -c 'import subprocess; subprocess.run(["./build/tests/test_survey_lifecycle"], check=True, timeout=15)'

build/tests/test_survey_lifecycle: tests/test_survey_lifecycle.cpp tests/test_media_backend.h src/media/media.cpp src/media/tracking.cpp src/config.cpp build/host/tracking_dlib.o $(APCAM_HEADERS) $(wildcard include/camera_app/APC_*.h)
	mkdir -p $(@D)
	$(HOST_CXX) $(CPPFLAGS) $(APP_CXXFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $(filter %.cpp %.o,$^) -lpthread -lm

test: survey-lifecycle-test

.PHONY: metadata-history-test
metadata-history-test: tests/test_metadata_history
	./tests/test_metadata_history

tests/test_metadata_history: tests/test_metadata_history.cpp src/recording/metadata.cpp include/camera_app/metadata.h
	$(HOST_CXX) -Iinclude -std=gnu++17 -Wall -Wextra -Werror -Wno-missing-field-initializers $< -o $@ -pthread

test: metadata-history-test

tests/test_terrain: tests/test_terrain.cpp src/protocol/terrain.cpp include/camera_app/terrain.h ../include/apcam/terrain_format.h
	$(HOST_CXX) $(APP_CXXFLAGS) -Iinclude -I../include $< src/protocol/terrain.cpp -pthread -o $@

.PHONY: terrain-test
terrain-test: tests/test_terrain
	python3 tests/test_terrain.py ./tests/test_terrain
test: terrain-test

.PHONY: zt30-thermal-metadata-test
zt30-thermal-metadata-test: $(THERMAL_CODECS_ROOT)/host/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/host/lib/libavutil.a
	@mkdir -p build/tests
	$(HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -DCA_THERMAL_STREAM_FFV1 -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -I../include -I$(THERMAL_CODECS_ROOT)/host/include -ffunction-sections -fdata-sections -Wl,--gc-sections ../tests/test_zt30_thermal_metadata.cpp $(THERMAL_CODECS_ROOT)/host/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/host/lib/libavutil.a $(call thermal_extra_libs,host) -pthread -lm -o build/tests/zt30-thermal-metadata-test
	./build/tests/zt30-thermal-metadata-test
