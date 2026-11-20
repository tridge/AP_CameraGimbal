# Reuse the common app with the qualified native media resource owners.
# Development build is continuous; RAM qualification remains independently bounded.
# Default native/package release remains guarded. MCU integration follows.
ifeq ($(ZT30_DEV_BUILD),1)
ZT30_APP_DIR := build/zt30-dev
ZT30_APP_FACTORY := src/backends/zt30/factory.cpp
ZT30_APP_CONTROL := $(CONTROL_DIR)/control.cpp
ZT30_APP_MODE := -DCAMERA_APP_ZT30_DEV
else
ZT30_APP_DIR := build/zt30-ram
ZT30_APP_FACTORY := src/backends/zt30/ram_factory.cpp
ZT30_APP_CONTROL := src/backends/zt30/ram_control.cpp
ZT30_APP_MODE := -DCAMERA_APP_ZT30_RAM
endif
ZT30_APP_SRC := $(filter-out $(CONTROL_DIR)/control.cpp,$(COMMON_SOURCES)) \
 $(ZT30_APP_CONTROL) $(ZT30_APP_FACTORY) \
 src/backends/zt30/media_backend.cpp src/backends/zt30/rgb_owner.cpp src/backends/zt30/encoder_owner.cpp \
 src/streaming/live_video_server.cpp src/recording/mp4_minimp4.cpp src/streaming/rtsp_xop.cpp
ifeq ($(ZT30_DEV_BUILD),1)
ZT30_APP_SRC += src/streaming/raw_thermal_server.cpp src/backends/zt30/photo_owner.cpp src/backends/zt30/isp.cpp src/backends/zt30/output.cpp src/backends/zt30/recording.cpp src/backends/zt30/overlay.cpp src/backends/zt30/input_buffer.cpp src/backends/zt30/input_mapping.cpp src/backends/zt30/thermal_encoder.cpp src/backends/zt30/thermal_live.cpp src/backends/zt30/thermal_controls.cpp src/backends/zt30/thermal_pixels.cpp src/backends/zt30/usb_iso.cpp src/backends/zt30/uvc_frame.cpp
endif
ZT30_APP_CPPFLAGS := $(CPPFLAGS) $(ZT30_APP_MODE) -D_FILE_OFFSET_BITS=64 \
 -I$(RTSP_ROOT) -I$(RTSP_ROOT)/net -I$(RTSP_ROOT)/xop -I$(RTSP_ROOT)/3rdpart
# Enumerate the pinned source tree, which exists before the private copy is
# prepared. Looking in build/xop here silently drops every object on first make.
ZT30_APP_XOP := $(patsubst $(XOP_ENUM_ROOT)/%,$(RTSP_ROOT)/%, \
 $(filter-out $(XOP_ENUM_ROOT)/net/TaskScheduler.cpp $(XOP_ENUM_ROOT)/xop/RtspConnection.cpp $(XOP_ENUM_ROOT)/xop/RtpConnection.cpp, \
 $(wildcard $(XOP_ENUM_ROOT)/net/*.cpp $(XOP_ENUM_ROOT)/xop/*.cpp)))
ZT30_APP_OBJECTS := $(patsubst %.cpp,$(ZT30_APP_DIR)/c/%.o,$(ZT30_APP_SRC)) \
 $(patsubst $(RTSP_ROOT)/%.cpp,$(ZT30_APP_DIR)/xop/%.o,$(ZT30_APP_XOP)) \
 $(ZT30_APP_DIR)/patched-rtsp.o $(ZT30_APP_DIR)/patched-rtp.o $(ZT30_APP_DIR)/patched-scheduler.o $(ZT30_APP_DIR)/tracking_dlib.o
$(ZT30_APP_DIR)/c/src/protocol/unigcs.o: build/binlog_version.h
ifeq ($(ZT30_DEV_BUILD),1)
ZT30_THERMAL_LIBS := $(THERMAL_CODECS_ROOT)/armv7/lib/libavcodec.a $(THERMAL_CODECS_ROOT)/armv7/lib/libavutil.a
$(ZT30_THERMAL_LIBS) &: ../tools/build_thermal_codecs.sh
	+MAKE='$(MAKE)' THERMAL_CROSS_COMPILE='$(CROSS_COMPILE)' ../tools/build_thermal_codecs.sh armv7
$(ZT30_APP_DIR)/c/src/streaming/thermal_stream.o: ZT30_APP_CPPFLAGS += -DCA_THERMAL_STREAM_FFV1 -I$(THERMAL_CODECS_ROOT)/armv7/include
$(ZT30_APP_DIR)/c/src/streaming/thermal_stream.o: $(ZT30_THERMAL_LIBS)
endif
OBJECTS := $(ZT30_APP_OBJECTS)
$(TARGET): $(OBJECTS) $(ZT30_THERMAL_LIBS) Makefile ../tools/zt30_app.mk
	mkdir -p $(@D)
	$(CXX) $(APP_CXXFLAGS) -o $@ $(OBJECTS) $(ZT30_THERMAL_LIBS) $(call thermal_extra_libs,armv7) -lpthread -ldl -lm
	python3 ../tools/check_zt30_elf.py $@ --readelf $(CROSS_COMPILE)readelf
$(ZT30_APP_DIR)/c/%.o: %.cpp Makefile ../tools/zt30_app.mk | generated
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) $(APP_CXXFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-error=sign-compare -Wno-error=unused-variable -Wno-error=implicit-fallthrough -c $< -o $@
$(ZT30_APP_DIR)/xop/%.o: $(RTSP_ROOT)/%.cpp | $(XOP_PREPARE_STAMP)
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -include cstdint -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/patched-rtsp.o: build/xop_rtsp_connection.cpp
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/patched-rtp.o: build/xop_rtp_connection.cpp
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/TaskScheduler.cpp: $(RTSP_ROOT)/net/TaskScheduler.cpp ../tools/zt30_xop_signal.patch ../tools/patch_source.py
	python3 ../tools/patch_source.py $< ../tools/zt30_xop_signal.patch $@
$(ZT30_APP_DIR)/patched-scheduler.o: $(ZT30_APP_DIR)/TaskScheduler.cpp
	$(CXX) $(DEPFLAGS) $(ZT30_APP_CPPFLAGS) -O2 -std=gnu++17 -c $< -o $@
$(ZT30_APP_DIR)/tracking_dlib.o: src/media/tracking_dlib.cpp include/camera_app/image_tracker.h Makefile ../tools/zt30_app.mk | generated
	mkdir -p $(@D)
	$(CXX) $(DEPFLAGS) $(DLIB_FLAGS) -c $< -o $@
-include $(ZT30_APP_OBJECTS:.o=.d)
