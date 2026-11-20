# No camera-app/package guard bypass. This builds only the RAM diagnostic.
ZT30_XOP_ROOT := $(abspath camera_app/build/xop)
ZT30_XOP_SRC := $(filter-out $(ZT30_XOP_ROOT)/xop/RtspConnection.cpp $(ZT30_XOP_ROOT)/xop/RtpConnection.cpp $(ZT30_XOP_ROOT)/net/TaskScheduler.cpp,$(wildcard $(ZT30_XOP_ROOT)/net/*.cpp $(ZT30_XOP_ROOT)/xop/*.cpp))
ZT30_STREAM_SRC := tools/zt30_rgb_probe.cpp camera_app/src/backends/zt30/rgb_owner.cpp camera_app/src/backends/zt30/encoder_owner.cpp camera_app/src/streaming/rtsp_xop.cpp camera_app/src/streaming/support_video.cpp camera_app/src/recording/video_metadata.cpp camera_app/src/recording/misb.cpp camera_app/src/recording/misb_ts.cpp camera_app/src/recording/metadata.cpp camera_app/src/log.cpp
ZT30_STREAM_XOP := $(ZT30_XOP_SRC) camera_app/build/xop_rtsp_connection.cpp camera_app/build/xop_rtp_connection.cpp
ZT30_STREAM_OBJECTS := $(patsubst %.cpp,build/zt30/stream/%.o,$(ZT30_STREAM_SRC)) $(patsubst $(ZT30_XOP_ROOT)/%.cpp,build/zt30/stream/xop/%.o,$(ZT30_XOP_SRC)) build/zt30/stream/patched-rtsp.o build/zt30/stream/patched-rtp.o build/zt30/stream/patched-scheduler.o
ZT30_STREAM_FLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Icamera_app/include -Iinclude -I$(ZT30_XOP_ROOT) -I$(ZT30_XOP_ROOT)/net -I$(ZT30_XOP_ROOT)/xop -I$(ZT30_XOP_ROOT)/3rdpart -include cstdint
.PHONY: zt30_stream_probe
zt30_stream_probe: build/zt30/encoder-sdk.so
	$(MAKE) -C camera_app build/xop_rtsp_connection.cpp build/xop_rtp_connection.cpp
	$(MAKE) build/zt30/stream-probe
camera_app/build/xop/.prepared camera_app/build/xop_rtsp_connection.cpp camera_app/build/xop_rtp_connection.cpp:
	$(MAKE) -C camera_app build/xop_rtsp_connection.cpp build/xop_rtp_connection.cpp
build/zt30/stream-probe: $(ZT30_STREAM_OBJECTS)
	$(ZT30_CROSS_COMPILE)g++ $^ -lpthread -ldl -lm -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
build/zt30/stream/%.o: %.cpp | camera_app/build/xop/.prepared
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -DZT30_ENCODER_PROBE -DZT30_STREAM_PROBE -MMD -MP -c $< -o $@
build/zt30/stream/xop/%.o: $(ZT30_XOP_ROOT)/%.cpp | camera_app/build/xop/.prepared
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -MMD -MP -c $< -o $@
build/zt30/stream/patched-rtsp.o: camera_app/build/xop_rtsp_connection.cpp
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -MMD -MP -c $< -o $@
build/zt30/stream/patched-rtp.o: camera_app/build/xop_rtp_connection.cpp
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -MMD -MP -c $< -o $@
-include $(ZT30_STREAM_OBJECTS:.o=.d)

build/zt30/TaskScheduler.cpp: $(ZT30_XOP_ROOT)/net/TaskScheduler.cpp tools/zt30_xop_signal.patch tools/patch_source.py
	python3 tools/patch_source.py $< tools/zt30_xop_signal.patch $@
build/zt30/stream/patched-scheduler.o: build/zt30/TaskScheduler.cpp
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -MMD -MP -c $< -o $@

ZT30_H264_EXTRA := camera_app/src/streaming/live_video_server.cpp camera_app/src/recording/mp4_minimp4.cpp
ZT30_H264_OBJECTS := $(patsubst %.cpp,build/zt30/h264-web/%.o,$(ZT30_STREAM_SRC) $(ZT30_H264_EXTRA)) $(filter build/zt30/stream/xop/% build/zt30/stream/patched-%,$(ZT30_STREAM_OBJECTS))
.PHONY: zt30_h264_web_probe
zt30_h264_web_probe: build/zt30/encoder-sdk.so
	$(MAKE) -C camera_app build/minimp4.h build/xop_rtsp_connection.cpp build/xop_rtp_connection.cpp
	$(MAKE) build/zt30/h264-web-probe
build/zt30/h264-web-probe: $(ZT30_H264_OBJECTS)
	$(ZT30_CROSS_COMPILE)g++ $^ -lpthread -ldl -lm -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
build/zt30/h264-web/%.o: %.cpp | camera_app/build/xop/.prepared camera_app/build/minimp4.h
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -Icamera_app/build -D_FILE_OFFSET_BITS=64 -DZT30_ENCODER_PROBE -DZT30_STREAM_PROBE -DZT30_H264_WEB_PROBE -MMD -MP -c $< -o $@
-include $(ZT30_H264_OBJECTS:.o=.d)

# Native backend harness remains separate from camera-app/release activation.
ZT30_NATIVE_OBJECTS := $(patsubst %.cpp,build/zt30/native-media/%.o,$(ZT30_STREAM_SRC) $(ZT30_H264_EXTRA) camera_app/src/backends/zt30/media_backend.cpp) $(filter build/zt30/stream/xop/% build/zt30/stream/patched-%,$(ZT30_STREAM_OBJECTS))
.PHONY: zt30_native_media_probe
zt30_native_media_probe: build/zt30/encoder-sdk.so
	$(MAKE) -C camera_app build/minimp4.h build/xop_rtsp_connection.cpp build/xop_rtp_connection.cpp
	$(MAKE) build/zt30/native-media-probe
build/zt30/native-media-probe: $(ZT30_NATIVE_OBJECTS)
	$(ZT30_CROSS_COMPILE)g++ $^ -lpthread -ldl -lm -o $@
	python3 tools/check_zt30_elf.py $@ --readelf $(ZT30_CROSS_COMPILE)readelf
build/zt30/native-media/%.o: %.cpp | camera_app/build/xop/.prepared camera_app/build/minimp4.h
	@mkdir -p $(@D)
	$(ZT30_CROSS_COMPILE)g++ $(ZT30_STREAM_FLAGS) -Icamera_app/build -D_FILE_OFFSET_BITS=64 -DZT30_ENCODER_PROBE -DZT30_STREAM_PROBE -DZT30_H264_WEB_PROBE -DZT30_NATIVE_MEDIA_PROBE -MMD -MP -c $< -o $@
-include $(ZT30_NATIVE_OBJECTS:.o=.d)
