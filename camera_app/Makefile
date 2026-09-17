CAMERA_BACKEND ?= mt11
include ../include/apcam/targets.mk
SUPPORTED_BACKENDS := mt11 a8 zr10 z1mini
ifeq ($(filter $(CAMERA_BACKEND),$(SUPPORTED_BACKENDS)),)
$(error Unsupported CAMERA_BACKEND '$(CAMERA_BACKEND)'; supported backends: $(SUPPORTED_BACKENDS))
endif

ifneq ($(filter $(CAMERA_BACKEND),a8 zr10 z1mini),)
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
CC := $(CROSS_COMPILE)gcc
HOST_CC ?= cc
HOST_CPP ?= c++
CPP := $(CROSS_COMPILE)g++
PYTHON ?= python3

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
MAVLINK_STAMP := build/mavlink/dialect
MAVLINK_DEPS := $(APCAM_HEADERS) $(MAVLINK_HEADER) $(MAVLINK_STAMP) include/camera_app/mavlink.h
MAVLINK_CPPFLAGS := -I$(MAVLINK_GEN_DIR) -DCA_MAVLINK_DIALECT=$(MAVLINK_DIALECT)

BACKEND_DIR := src/backends/$(CAMERA_BACKEND)
CPPFLAGS = -Ibuild $(APCAM_CPPFLAGS) -Iinclude -I$(BACKEND_DIR) $(MAVLINK_CPPFLAGS)
CFLAGS ?= -O2 -pipe -Wall -Wextra -Werror -std=c11
ifeq ($(CAMERA_BACKEND),zr10)
CFLAGS += -Wno-missing-field-initializers -D_FILE_OFFSET_BITS=64
endif
ifeq ($(CAMERA_BACKEND),z1mini)
CFLAGS += -D_FILE_OFFSET_BITS=64
TARGET_LINK_FLAGS := -Wl,--wrap=fcntl
endif
# header dependency tracking, so struct layout changes rebuild every user
DEPFLAGS := -MMD -MP

SS928_MPP_ROOT ?= $(abspath ../build/deps/ss928-mpp)
MINIMP4_ROOT ?= $(abspath ../build/deps/minimp4)
MINIMP4_HEADER := build/minimp4.h
SAMPLE_COMMON := $(SS928_MPP_ROOT)/src/common
VENDOR_LIB_DIR := $(SS928_MPP_ROOT)/lib/linux/hisilicon
RTSP_ROOT := $(SS928_MPP_ROOT)/src/rtspserver/src
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

COMMON_SOURCES := $(if $(filter z1mini,$(CAMERA_BACKEND)),src/protocol/xfrobot_server.c) src/main.c \
	src/config.c \
	src/log.c src/recording/binlog.c src/protocol/manual_control.c \
	src/media/autofocus.c src/media/media.c src/media/overlay.c \
	src/protocol/mavlink.c \
	src/protocol/mavlink_server.c src/protocol/camera_definition.c src/protocol/camera_ftp.c \
	src/protocol/support_mavlink.c \
	src/protocol/network.c \
	src/streaming/support_video.c \
	src/protocol/external_uart.c \
	src/protocol/targeting.c \
	src/protocol/siyi.c \
	src/protocol/siyi_server.c \
	src/protocol/private_uart.c \
	src/recording/recorder_state.c \
	src/recording/raw_thermal.c \
	src/recording/still.c \
	src/recording/metadata.c \
	src/recording/video_metadata.c \
	$(BACKEND_DIR)/control.c
OBJECTS := $(COMMON_SOURCES:.c=.o) build/mt11_media.o build/mt11_overlay.o build/mt11_pipeline.o \
	build/mt11_e5739.o build/mt11_thermal.o build/mt11_isp.o \
	build/raw_thermal_server.o \
	build/live_video_server.o \
	build/mp4_minimp4.o \
	build/rtsp_xop.o build/xop_rtsp_connection.o build/xop_rtp_connection.o \
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
TEST_SOURCES := tests/test_protocol.c src/protocol/siyi.c src/protocol/private_uart.c
HOST_TARGET := tests/camera-app-host
A8_HOST_TARGET := tests/camera-app-a8-host
A8_HOST_SOURCES := $(filter-out src/backends/mt11/control.c \
	src/backends/a8/control.c,$(COMMON_SOURCES)) \
	src/backends/a8/control.c src/media/stub.c
SITL_TARGET ?= ../build/sitl/camera-app
SITL_OBJDIR ?= build/sitl
SITL_EXTRA_CPPFLAGS ?=
SITL_SOURCES := $(COMMON_SOURCES) src/media/stub.c src/media/sitl_terrain.c \
	src/streaming/live_video_server.c src/recording/mp4_minimp4.c
SITL_C_OBJECTS := $(patsubst %.c,$(SITL_OBJDIR)/c/%.o,$(SITL_SOURCES))
XOP_HOST_SOURCES := $(filter-out $(RTSP_ROOT)/xop/RtspConnection.cpp \
	$(RTSP_ROOT)/xop/RtpConnection.cpp,$(wildcard $(RTSP_ROOT)/net/*.cpp) \
	$(wildcard $(RTSP_ROOT)/xop/*.cpp))
XOP_HOST_OBJECTS := $(patsubst $(RTSP_ROOT)/%.cpp,$(SITL_OBJDIR)/xop/%.o,\
	$(XOP_HOST_SOURCES)) $(SITL_OBJDIR)/xop/xop/RtspConnection.o \
	$(SITL_OBJDIR)/xop/xop/RtpConnection.o
SITL_CPP_OBJECTS := $(SITL_OBJDIR)/rtsp_xop.o $(XOP_HOST_OBJECTS)

SSH_HOST ?= MT11
SSH ?= ssh
RSYNC_RSH ?= ssh
REMOTE_BIN ?= /app/bin/camera-app
REMOTE_CONFIG ?= /app/camera.ini
REMOTE_WEB ?= /app/bin/mt11-web
REMOTE_APP_INIT ?= /app/app_init.sh
REMOTE_APP_SELECTION ?= /app/app_selection.sh
REMOTE_TIMESYNC ?= /app/bin/mt11-timesync.sh
APP_INIT ?= ../packaging/mt11/app_init.sh
APP_SELECTION ?= ../packaging/mt11/app_selection.sh
TIMESYNC ?= ../packaging/mt11/mt11-timesync.sh
WEB_PORT ?= 80

.PHONY: all clean host sitl test install install-app install-web install-init FORCE

all: $(TARGET)

build/binlog_version.h: FORCE
	$(PYTHON) ../tools/write_build_version.py $@

src/recording/binlog.c: build/binlog_version.h


ifneq ($(filter $(CAMERA_BACKEND),a8 zr10 z1mini),)
A8_XOP_OBJECTS := $(patsubst $(RTSP_ROOT)/%.cpp,build/$(CAMERA_BACKEND)/xop/%.o,\
	$(XOP_HOST_SOURCES)) build/$(CAMERA_BACKEND)/xop/xop/RtspConnection.o \
	build/$(CAMERA_BACKEND)/xop/xop/RtpConnection.o
A8_C_OBJECTS := $(patsubst %.c,build/$(CAMERA_BACKEND)/c/%.o,$(COMMON_SOURCES) \
	src/streaming/live_video_server.c src/recording/mp4_minimp4.c \
	$(BACKEND_DIR)/media.c $(BACKEND_DIR)/pipeline.c $(if $(filter a8 zr10,$(CAMERA_BACKEND)),src/backends/siyi/overlay.c) $(if $(filter a8,$(CAMERA_BACKEND)),$(BACKEND_DIR)/isp.c) $(if $(filter zr10,$(CAMERA_BACKEND)),src/compat/zr10_uclibc.c) $(if $(filter z1mini,$(CAMERA_BACKEND)),$(BACKEND_DIR)/glibc_compat.c $(BACKEND_DIR)/native.c))
OBJECTS := $(A8_C_OBJECTS) build/$(CAMERA_BACKEND)/rtsp_xop.o $(A8_XOP_OBJECTS)

$(TARGET): $(OBJECTS)
	mkdir -p $(dir $@)
	$(CPP) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) -Wl,--gc-sections \
		-Wl,--no-as-needed -lpthread -lm -ldl $(TARGET_LINK_FLAGS) $(if $(filter zr10 z1mini,$(CAMERA_BACKEND)),-static-libstdc++ -static-libgcc)

build/$(CAMERA_BACKEND)/c/%.o: %.c $(MINIMP4_HEADER)
	mkdir -p $(dir $@)
	$(CC) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-unused-function \
		-Wno-error=sign-compare -Wno-error=unused-variable \
		-Wno-error=implicit-fallthrough -ffunction-sections -fdata-sections \
		-c $< -o $@

build/$(CAMERA_BACKEND)/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p $(dir $@)
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/%.o: $(RTSP_ROOT)/%.cpp
	mkdir -p $(dir $@)
	$(CPP) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-I$(RTSP_ROOT)/3rdpart -include cstdint -O2 -pipe -Wall -Wextra \
		-Wno-error -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/xop/RtspConnection.o: build/xop_rtsp_connection.cpp
	mkdir -p $(dir $@)
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/$(CAMERA_BACKEND)/xop/xop/RtpConnection.o: build/xop_rtp_connection.cpp
	mkdir -p $(dir $@)
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@
else
$(TARGET): $(OBJECTS) $(XOP_COMPAT_LIB)
	$(CPP) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS) \
		-Wl,--start-group $(MEDIA_LIBS) -Wl,--end-group \
		-Wl,--gc-sections -lpthread -lm -ldl
endif

%.o: %.c
	$(CC) $(DEPFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(MAVLINK_XML) $(MAVGEN):
	@echo "error: $@ is missing; run 'git submodule update --init --recursive'" >&2
	@exit 1

$(MAVLINK_HEADER): $(MAVLINK_XML) $(MAVGEN) $(MAVGEN_SOURCES)
	mkdir -p $(MAVLINK_GEN_DIR)
	$(PYTHON) $(MAVGEN) --lang C --wire-protocol 2.0 \
		--output $(MAVLINK_GEN_DIR) $(MAVLINK_XML)

$(MAVLINK_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(CAMERA_BACKEND) $(MAVLINK_DIALECT)' | cmp -s - $@ || echo '$(CAMERA_BACKEND) $(MAVLINK_DIALECT)' > $@

src/protocol/mavlink.o src/protocol/mavlink_server.o src/protocol/support_mavlink.o: $(MAVLINK_DEPS)
$(SITL_OBJDIR)/c/src/protocol/mavlink.o \
	$(SITL_OBJDIR)/c/src/protocol/mavlink_server.o \
	$(SITL_OBJDIR)/c/src/protocol/support_mavlink.o: $(MAVLINK_DEPS)
build/$(CAMERA_BACKEND)/c/src/protocol/mavlink.o \
	build/$(CAMERA_BACKEND)/c/src/protocol/mavlink_server.o \
	build/$(CAMERA_BACKEND)/c/src/protocol/support_mavlink.o: $(MAVLINK_DEPS)

$(TEST_TARGET): $(TEST_SOURCES)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SOURCES)

$(MAVLINK_TEST_TARGET): tests/test_mavlink.c src/protocol/mavlink.c \
                        $(MAVLINK_DEPS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^) -lm

tests/test_mavlink_time: tests/test_mavlink_time.c src/protocol/mavlink_server.c \
                        src/protocol/mavlink.c $(MAVLINK_DEPS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -o $@ $< src/protocol/mavlink.c -lm

$(CONFIG_TEST_TARGET): tests/test_config.c src/config.c include/camera_app/config.h $(APCAM_HEADERS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^)

tests/test_gimbal_rate: tests/test_gimbal_rate.c include/camera_app/gimbal_rate.h $(APCAM_HEADERS)
	$(HOST_CC) -I../include -Iinclude -DAPCAM_TARGET=APCAM_TARGET_A8 $(CFLAGS) -o $@ $< -lm

$(TARGETING_TEST_TARGET): tests/test_targeting.c src/protocol/targeting.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(STILL_TEST_TARGET): tests/test_still.c src/recording/still.c \
		src/recording/metadata.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread -lm

$(AUTOFOCUS_TEST_TARGET): tests/test_autofocus.c src/media/autofocus.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

$(HOST_TARGET): $(COMMON_SOURCES) src/media/stub.c $(MAVLINK_DEPS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^) -lm

$(A8_HOST_TARGET): CAMERA_BACKEND=a8
$(A8_HOST_TARGET): $(A8_HOST_SOURCES) $(MAVLINK_DEPS) src/backends/siyi/control.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) \
		-o $@ $(filter-out src/backends/siyi/control.c,$(filter %.c,$^)) -lm

build/mt11_overlay.o: src/backends/mt11/overlay.c
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -c $< -o $@

build/mt11_media.o: $(BACKEND_DIR)/media.c $(BACKEND_DIR)/pipeline.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -Wno-unused-function -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections -c $< -o $@

build/mt11_pipeline.o: $(BACKEND_DIR)/pipeline.c $(BACKEND_DIR)/pipeline.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections -c $< -o $@

build/mt11_isp.o: $(BACKEND_DIR)/isp.c $(BACKEND_DIR)/isp.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -Wno-error=unused-parameter \
		-ffunction-sections -fdata-sections \
		-c $< -o $@

build/mt11_e5739.o: $(BACKEND_DIR)/e5739.c $(BACKEND_DIR)/e5739.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/mt11_thermal.o: $(BACKEND_DIR)/thermal.c $(BACKEND_DIR)/thermal.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/raw_thermal_server.o: src/streaming/raw_thermal_server.c \
                              include/camera_app/raw_thermal_server.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

build/live_video_server.o: src/streaming/live_video_server.c \
                              include/camera_app/live_video_server.h \
                              include/camera_app/mp4.h
	mkdir -p build
	$(CC) $(DEPFLAGS) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(MINIMP4_HEADER): $(MINIMP4_ROOT)/minimp4.h src/recording/minimp4_metadata.patch Makefile
	mkdir -p build
	cp $< $@
	sed -i 's/\r$$//' $@
	patch $@ < src/recording/minimp4_metadata.patch
	sed -i 's/^#define MP4D_TFDT_SUPPORT[[:space:]]*0/#define MP4D_TFDT_SUPPORT         1/' $@
	grep -q '^#define MP4D_TFDT_SUPPORT[[:space:]]*1' $@

build/mp4_minimp4.o: src/recording/mp4_minimp4.c $(MINIMP4_HEADER)
	mkdir -p build
	$(CC) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-unused-function \
		-Wno-error=sign-compare -Wno-error=unused-variable \
		-Wno-error=implicit-fallthrough -c $< -o $@

build/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p build
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/xop_rtsp_connection.cpp: src/streaming/xop_aggregate_setup.patch Makefile
	mkdir -p build
	cp $(RTSP_ROOT)/xop/RtspConnection.cpp $@
	sed -i 's/\r$$//' $@
	patch $@ < $<

build/xop_rtsp_connection.o: build/xop_rtsp_connection.cpp
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

build/xop_rtp_connection.cpp: src/streaming/xop_satip_payload.patch Makefile
	mkdir -p build
	cp $(RTSP_ROOT)/xop/RtpConnection.cpp $@
	sed -i 's/\r$$//' $@
	patch $@ < $<

build/xop_rtp_connection.o: build/xop_rtp_connection.cpp
	$(CPP) $(CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

$(XOP_COMPAT_LIB): $(XOP_VENDOR_LIB) Makefile
	mkdir -p build
	cp $< $@
	$(CROSS_COMPILE)ar d $@ RtspConnection.o
	$(CROSS_COMPILE)ar d $@ RtpConnection.o

build/mt11_imx586.o: $(BACKEND_DIR)/imx586.c $(BACKEND_DIR)/imx586.h \
		$(BACKEND_DIR)/imx586_mode0_registers.inc
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections \
		-DMT11_IMX586_EXPORT_IMX678_ALIAS -c $< -o $@

build/sample_comm_%.o: $(SAMPLE_COMMON)/sample_comm_%.c
	mkdir -p build
	$(CC) $(DEPFLAGS) $(MEDIA_CPPFLAGS) $(CFLAGS) -Wno-error -fno-aggressive-loop-optimizations \
		-ffunction-sections -fdata-sections -D_GNU_SOURCE -c $< -o $@

build/scene_ot_confaccess.o: $(SCENE_ROOT)/tools/configaccess/src/ot_confaccess.c
	mkdir -p build
	$(CC) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_loadparam.o: $(SCENE_ROOT)/src/sample/scene_loadparam.c
	mkdir -p build
	$(CC) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_ot_scene.o: $(SCENE_ROOT)/src/core/ot_scene.c
	mkdir -p build
	$(CC) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_ot_scene_setparam.o: $(SCENE_ROOT)/src/core/ot_scene_setparam.c
	mkdir -p build
	$(CC) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

build/scene_setparam_inner.o: $(SCENE_ROOT)/src/core/scene_setparam_inner.c
	mkdir -p build
	$(CC) $(MEDIA_CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=gnu11 \
		-Wno-error -ffunction-sections -fdata-sections -c $< -o $@

host: $(HOST_TARGET)

sitl: $(SITL_TARGET)

$(SITL_TARGET): $(SITL_C_OBJECTS) $(SITL_CPP_OBJECTS) Makefile
	mkdir -p $(dir $@)
	$(HOST_CPP) -o $@ $(SITL_C_OBJECTS) $(SITL_CPP_OBJECTS) -lpthread -lm

$(SITL_OBJDIR)/c/%.o: %.c $(MINIMP4_HEADER)
	mkdir -p $(dir $@)
	$(HOST_CC) $(DEPFLAGS) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -Ibuild $(CFLAGS) -Wno-error \
		-DCAMERA_APP_SITL -DCA_SITL_VIDEO_SCRIPT='"$(abspath ../sitl/terrain_video.py)"' -c $< -o $@

$(SITL_OBJDIR)/rtsp_xop.o: src/streaming/rtsp_xop.cpp
	mkdir -p $(dir $@)
	$(HOST_CPP) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/%.o: $(RTSP_ROOT)/%.cpp
	mkdir -p $(dir $@)
	$(HOST_CPP) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop \
		-I$(RTSP_ROOT)/3rdpart -include cstdint \
		-O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter \
		-Wno-error=missing-field-initializers -std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/xop/RtspConnection.o: build/xop_rtsp_connection.cpp
	mkdir -p $(dir $@)
	$(HOST_CPP) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

$(SITL_OBJDIR)/xop/xop/RtpConnection.o: build/xop_rtp_connection.cpp
	mkdir -p $(dir $@)
	$(HOST_CPP) $(CPPFLAGS) $(SITL_EXTRA_CPPFLAGS) -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net \
		-I$(RTSP_ROOT)/xop -O2 -pipe -Wall -Wextra -Werror -Wno-error \
		-Wno-error=unused-parameter -Wno-error=missing-field-initializers \
		-std=c++11 -c $< -o $@

tests/test_support_mavlink: tests/test_support_mavlink.c src/protocol/support_mavlink.c \
                           src/protocol/mavlink.c src/config.c src/log.c $(MAVLINK_DEPS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter %.c,$^) -lpthread -lm

$(VIDEO_METADATA_TEST_TARGET): tests/test_video_metadata.c src/recording/video_metadata.c src/recording/metadata.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread -lm

$(MP4_TEST_TARGET): tests/test_mp4.c src/recording/mp4_minimp4.c \
                    src/recording/video_metadata.c src/recording/metadata.c \
                    $(MINIMP4_HEADER)
	$(HOST_CC) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-error \
		-o $@ $(filter %.c,$^) -lpthread -lm

$(E5739_TEST_TARGET): tests/test_e5739.c $(BACKEND_DIR)/e5739.c src/log.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lm

$(MP4_SYNC_TEST_TARGET): tests/test_mp4_sync.c src/recording/mp4_minimp4.c \
                    src/recording/video_metadata.c src/recording/metadata.c \
                    $(MINIMP4_HEADER)
	$(HOST_CC) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-error \
		-o $@ $(filter %.c,$^) -Wl,--wrap=fdatasync -Wl,--wrap=fstatfs64 -lpthread -lm

$(MP4_ROLLOVER_TEST_TARGET): tests/test_mp4_rollover.c src/recording/mp4_minimp4.c \
                    src/recording/video_metadata.c src/recording/metadata.c \
                    $(MINIMP4_HEADER)
	$(HOST_CC) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-error \
		-o $@ $(filter %.c,$^) -Wl,--wrap=fstatfs64 -Wl,--wrap=pwrite64 -lpthread -lm

.PHONY: recording-test
recording-test: $(MP4_TEST_TARGET) $(MP4_SYNC_TEST_TARGET) $(MP4_ROLLOVER_TEST_TARGET)
	python3 tests/test_mp4_rollover.py --large

$(THERMAL_TEST_TARGET): tests/test_thermal.c $(BACKEND_DIR)/thermal.c src/log.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread

$(MT11_IMX586_TEST_TARGET): tests/test_mt11_imx586.c $(BACKEND_DIR)/imx586.c \
		$(BACKEND_DIR)/imx586.h $(BACKEND_DIR)/imx586_mode0_registers.inc
	$(HOST_CC) $(MEDIA_CPPFLAGS) $(filter-out -static,$(CFLAGS)) \
		-DMT11_IMX586_TEST_API -o $@ \
		$(BACKEND_DIR)/imx586.c tests/test_mt11_imx586.c

$(RAW_THERMAL_TEST_TARGET): tests/test_raw_thermal.c src/recording/raw_thermal.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

$(RAW_THERMAL_SERVER_TEST_TARGET): tests/test_raw_thermal_server.c \
                                   src/streaming/raw_thermal_server.c src/log.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ -lpthread

$(LIVE_VIDEO_SERVER_TEST_TARGET): tests/test_live_video_server.c \
                                  src/streaming/live_video_server.c \
                                  src/recording/mp4_minimp4.c src/log.c \
                                  src/recording/video_metadata.c src/recording/metadata.c \
                                  $(MINIMP4_HEADER)
	$(HOST_CC) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-error \
		-o $@ tests/test_live_video_server.c \
		src/streaming/live_video_server.c src/recording/mp4_minimp4.c \
		src/log.c src/recording/video_metadata.c src/recording/metadata.c -lpthread -lm

test: manual-control-test tests/test_gimbal_rate tests/test_mavlink_time tests/test_support_mavlink $(VIDEO_METADATA_TEST_TARGET) $(TEST_TARGET) $(MAVLINK_TEST_TARGET) $(MP4_TEST_TARGET) $(E5739_TEST_TARGET) \
	$(MP4_SYNC_TEST_TARGET) \
	$(MP4_ROLLOVER_TEST_TARGET) \
	$(THERMAL_TEST_TARGET) $(RAW_THERMAL_TEST_TARGET) \
	$(RAW_THERMAL_SERVER_TEST_TARGET) $(STILL_TEST_TARGET) \
	$(LIVE_VIDEO_SERVER_TEST_TARGET) \
	$(AUTOFOCUS_TEST_TARGET) $(CONFIG_TEST_TARGET) $(TARGETING_TEST_TARGET) \
	$(MT11_IMX586_TEST_TARGET) \
	$(HOST_TARGET) $(A8_HOST_TARGET)
	./$(TEST_TARGET)
	./$(MAVLINK_TEST_TARGET)
	./tests/test_mavlink_time
	./tests/test_gimbal_rate
	./tests/test_support_mavlink
	python3 ../sitl/test_support_video_queue.py
	python3 ../tests/test_targets.py
	python3 ../tests/test_a8_service.py
	./$(VIDEO_METADATA_TEST_TARGET)
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
	ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=10 \
		-t 2.3 -c:v libx264 -x264-params aud=1:bframes=0:slices=4 \
		-pix_fmt yuv420p -f h264 -y tests/test.h264
	./$(LIVE_VIDEO_SERVER_TEST_TARGET) tests/test.h264
	./$(MP4_TEST_TARGET) tests/test.h264 tests/test.mp4
	./$(MP4_SYNC_TEST_TARGET) tests/test.h264
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
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' ../web/mt11-web $(SSH_HOST):$(REMOTE_WEB).new
	$(SSH) $(SSH_HOST) 'set -eu; chmod 0755 $(REMOTE_WEB).new; mv $(REMOTE_WEB).new $(REMOTE_WEB); sed -i "s/mt11-web -p 8080/mt11-web -p $(WEB_PORT)/g" /app/app_init.sh; pids=$$(ps | awk '\''/mt11-web -p (80|8080)/ && !/awk/ {print $$1}'\''); for pid in $$pids; do kill -TERM "$$pid"; done; sleep 1; $(REMOTE_WEB) -p $(WEB_PORT) >/run/mt11-web.log 2>&1 &'

install-init: $(APP_INIT) $(APP_SELECTION) $(TIMESYNC)
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(TIMESYNC) $(SSH_HOST):$(REMOTE_TIMESYNC).new
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(APP_SELECTION) $(SSH_HOST):$(REMOTE_APP_SELECTION).new
	rsync -av --chmod=F755 -e '$(RSYNC_RSH)' $(APP_INIT) $(SSH_HOST):$(REMOTE_APP_INIT).new
	$(SSH) $(SSH_HOST) 'set -eu; chmod 0755 $(REMOTE_TIMESYNC).new $(REMOTE_APP_SELECTION).new $(REMOTE_APP_INIT).new; /bin/fsync $(REMOTE_TIMESYNC).new; /bin/fsync $(REMOTE_APP_SELECTION).new; /bin/fsync $(REMOTE_APP_INIT).new; mv $(REMOTE_TIMESYNC).new $(REMOTE_TIMESYNC); /bin/fsync /app/bin; test -x $(REMOTE_TIMESYNC); mv $(REMOTE_APP_SELECTION).new $(REMOTE_APP_SELECTION); /bin/fsync /app; test -x $(REMOTE_APP_SELECTION); mv $(REMOTE_APP_INIT).new $(REMOTE_APP_INIT); /bin/fsync /app; test -x $(REMOTE_APP_INIT); sha256sum $(REMOTE_TIMESYNC) $(REMOTE_APP_SELECTION) $(REMOTE_APP_INIT)'

clean:
	rm -f tests/test_gimbal_rate
	rm -f tests/test_mavlink_time
	rm -f tests/test_support_mavlink tests/test_thermal_monitor $(MP4_SYNC_TEST_TARGET) $(MP4_ROLLOVER_TEST_TARGET)
	rm -f tests/test_manual_control $(TARGET) $(COMMON_SOURCES:.c=.o) $(COMMON_SOURCES:.c=.d) $(TEST_TARGET) $(MAVLINK_TEST_TARGET) $(MP4_TEST_TARGET) $(E5739_TEST_TARGET) $(THERMAL_TEST_TARGET) $(RAW_THERMAL_TEST_TARGET) $(RAW_THERMAL_SERVER_TEST_TARGET) $(LIVE_VIDEO_SERVER_TEST_TARGET) $(VIDEO_METADATA_TEST_TARGET) $(STILL_TEST_TARGET) $(AUTOFOCUS_TEST_TARGET) $(CONFIG_TEST_TARGET) $(TARGETING_TEST_TARGET) $(MT11_IMX586_TEST_TARGET) $(HOST_TARGET) $(A8_HOST_TARGET)
	rm -f tests/test.h264 tests/test.mp4 tests/test.mp4.fragmented \
		tests/test.ffprobe.err tests/test.fragmented.ffprobe.err
	rm -rf build

-include $(OBJECTS:.o=.d) $(SITL_C_OBJECTS:.o=.d)

# Backend wrapper includes the common SIYI control implementation.
src/backends/a8/control.o src/backends/zr10/control.o: src/backends/siyi/control.c

ZR10_HOST_TARGET := tests/camera-app-zr10-host
$(ZR10_HOST_TARGET): CAMERA_BACKEND=zr10
$(ZR10_HOST_TARGET): $(filter-out src/backends/mt11/control.c src/backends/a8/control.c src/backends/zr10/control.c,$(COMMON_SOURCES)) src/backends/zr10/control.c src/backends/siyi/control.c src/media/stub.c $(MAVLINK_DEPS)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(filter-out src/backends/siyi/control.c,$(filter %.c,$^)) -lm

.PHONY: zr10-test
zr10-test: $(ZR10_HOST_TARGET)
	python3 tests/test_zr10.py ./$(ZR10_HOST_TARGET)
	python3 tests/test_zr10_pipeline.py

# Optional target-runtime verification using a separately extracted stock rootfs.
ZR10_QEMU ?= qemu-arm
ZR10_ROOTFS ?=
.PHONY: zr10-runtime-test
zr10-runtime-test: $(TARGET)
	test '$(CAMERA_BACKEND)' = zr10
	test -n '$(ZR10_ROOTFS)'
	$(CC) $(CFLAGS) tests/test_zr10_ctype.c src/compat/zr10_uclibc.c -o build/zr10/test_ctype
	$(ZR10_QEMU) -L '$(ZR10_ROOTFS)' -E LD_BIND_NOW=1 build/zr10/test_ctype
	$(ZR10_QEMU) -L '$(ZR10_ROOTFS)' -E LD_BIND_NOW=1 $(TARGET) --help

ifeq ($(CAMERA_BACKEND),z1mini)
# Exercise the real Z1 media receiver with a local RTSP fixture and PTY MCU.
Z1_HOST_TARGET := build/z1mini-host/camera-app
Z1_HOST_SOURCES := $(COMMON_SOURCES) src/backends/z1mini/media.c \
	src/backends/z1mini/pipeline.c src/backends/z1mini/native.c src/streaming/live_video_server.c src/recording/mp4_minimp4.c
Z1_HOST_OBJECTS := $(patsubst %.c,build/z1mini-host/c/%.o,$(Z1_HOST_SOURCES))
.PHONY: z1mini-host
z1mini-host: $(Z1_HOST_TARGET)
$(Z1_HOST_TARGET): $(Z1_HOST_OBJECTS) $(SITL_CPP_OBJECTS)
	$(HOST_CPP) -o $@ $^ -lpthread -lm
build/z1mini-host/c/%.o: %.c $(MINIMP4_HEADER) $(MAVLINK_DEPS)
	mkdir -p $(dir $@)
	$(HOST_CC) $(DEPFLAGS) $(CPPFLAGS) -Ibuild $(CFLAGS) -Wno-unused-function -Wno-error=sign-compare -Wno-error=unused-variable -Wno-error=implicit-fallthrough -DCA_Z1_TEST -c $< -o $@
-include $(Z1_HOST_OBJECTS:.o=.d)

endif

# Native Z1 capture uses the public AX620A BSP headers at build time, and the
# camera's installed SDK shared libraries at runtime. Headers are not packaged.
Z1MINI_AX_SDK_INCLUDE ?=
.PHONY: z1mini-capture
z1mini-capture: build/z1mini/ax-capture
.PHONY: z1mini-overlay-test
z1mini-overlay-test:
	@test -n "$(Z1MINI_AX_SDK_INCLUDE)" || { echo 'Set Z1MINI_AX_SDK_INCLUDE to AX620A msp/out/include' >&2; exit 1; }
	mkdir -p build/tests
	$(HOST_CC) -O2 -Wall -Wextra -Werror -std=c11 -ffunction-sections -fdata-sections -Wl,--gc-sections -I../include -DAPCAM_TARGET=APCAM_TARGET_Z1_MINI -Iinclude -I'$(Z1MINI_AX_SDK_INCLUDE)' -Isrc/backends/z1mini tests/test_z1mini_overlay.c src/media/overlay.c -o build/tests/test_z1mini_overlay -ldl -pthread -lm
	./build/tests/test_z1mini_overlay

build/z1mini/ax-capture: src/backends/z1mini/ax_capture.c src/media/overlay.c include/camera_app/overlay.h src/backends/z1mini/ax_config.h src/backends/z1mini/native.h include/camera_app/exposure.h
	@test -n "$(Z1MINI_AX_SDK_INCLUDE)" || { echo 'Set Z1MINI_AX_SDK_INCLUDE to AX620A msp/out/include' >&2; exit 1; }
	mkdir -p $(dir $@)
	$(CC) -O2 -g -Wall -Wextra -Werror -std=c11 $(APCAM_CPPFLAGS) -Iinclude -I'$(Z1MINI_AX_SDK_INCLUDE)' -Isrc/backends/z1mini $< src/media/overlay.c -o $@ -ldl -pthread -lm

.PHONY: thermal-monitor-test
thermal-monitor-test: tests/test_thermal_monitor
	$(PYTHON) -c 'import subprocess; subprocess.run(["./tests/test_thermal_monitor"], check=True, timeout=10)'

tests/test_thermal_monitor: tests/test_thermal_monitor.c src/media/media.c include/camera_app/media.h include/camera_app/binlog.h include/camera_app/exposure.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tests/test_thermal_monitor.c src/media/media.c -lpthread -lm

.PHONY: exposure-test
exposure-test: thermal-monitor-test tests/test_exposure tests/test_exposure_native
	./tests/test_exposure
	$(PYTHON) -c 'import subprocess; subprocess.run(["./tests/test_exposure_native"], check=True, timeout=10)'

tests/test_exposure: tests/test_exposure.c include/camera_app/sigmastar_exposure.h include/camera_app/exposure.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< -lm

tests/test_exposure_native: tests/test_z1mini_native.c src/backends/z1mini/native.c src/backends/z1mini/native.h include/camera_app/exposure.h src/log.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_z1mini_native.c src/backends/z1mini/native.c src/log.c -lpthread -lm

.PHONY: manual-control-test
manual-control-test: tests/test_manual_control
	./tests/test_manual_control

.PHONY: mavlink-components-test
mavlink-components-test: host
	$(PYTHON) tests/test_mavlink_components.py ./$(HOST_TARGET) ../sitl/gimbal_sim.py

tests/test_manual_control: tests/test_manual_control.c src/protocol/manual_control.c include/camera_app/manual_control.h ../include/apcam/manual_control.h include/camera_app/backend.h
	$(HOST_CC) -Wall -Wextra -Werror -std=c11 -DAPCAM_TARGET=APCAM_TARGET_MT11 -I../include -Iinclude -o $@ $< -lm
