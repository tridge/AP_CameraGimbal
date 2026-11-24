# Tests, hardware probes and other developer targets, kept out of the
# top-level Makefile so its tab completion stays short. Run from the
# repository root: make -f tools/dev.mk <target>
include Makefile

.PHONY: release-test camera-definition-test sitl-mavlink-test a8_sitl-mavlink-test \
	sitl-video-telemetry-test a8_sitl-video-telemetry-test \
	sitl-armed-recording-test a8_sitl-armed-recording-test \
	recording-recovery-test sitl-supportproxy-test a8_sitl-supportproxy-test \
	ardupilot-siyi-test ardupilot-mavlink-test mavproxy-camera-test sitl-clean

.PHONY: safe-tar-test
safe-tar-test:
	python3 tests/test_safe_tar.py

.PHONY: mt11-storage-test
mt11-storage-test:
	python3 tests/test_mt11_storage.py
camera-definition-test: _camera-definitions
	python3 tests/test_camera_definition.py

.PHONY: survey-runtime-test
survey-runtime-test:
	python3 tests/test_survey_runtime.py

release-test:
	python3 tests/test_cpp_build.py
	python3 tests/test_build_version.py
	python3 tests/test_release.py
	python3 tests/test_build_dependencies.py
	python3 tests/test_parallel_build.py
	python3 tests/test_prebuilt_tools.py

.PHONY: platform-test
platform-test: mt11-storage-test
	python3 tests/test_siyi_uuid.py
	python3 tests/test_zr10_install.py
	python3 tests/test_platform_packages.py '$(RELEASE_ROOT)/$(MT11_VERSION)'

sitl-mavlink-test: _sitl-mt11
	python3 sitl/test_mavlink_parameters.py --backend mt11 --build $(SITL_BUILD)
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD)
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD) --jitter
	python3 sitl/test_roi_motion.py --build $(SITL_BUILD) --jitter --rate
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --build $(SITL_BUILD)

a8_sitl-mavlink-test: _sitl-a8
	python3 sitl/test_mavlink_parameters.py --backend a8 --build $(A8_SITL_BUILD)

.PHONY: a8_sitl-rate-test
a8_sitl-rate-test: _sitl-a8
	$(MAKE) -C camera_app tests/test_gimbal_rate
	./camera_app/tests/test_gimbal_rate
	$(SITL_VIDEO_PYTHON) sitl/test_rate_tracking.py --output $(A8_SITL_BUILD)/rate-test

.PHONY: sitl-angle-hold-test
.PHONY: sitl-rate-default-test
sitl-rate-default-test: _sitl-mt11
	$(SITL_VIDEO_PYTHON) sitl/test_rate_default.py --build $(SITL_BUILD)

.PHONY: sitl-live-tracking-test
sitl-live-tracking-test: _sitl-mt11
	$(MAKE) -C web portable-sitl SITL_BIN_ROOT=$(abspath $(SITL_BUILD))
	$(MAKE) -C camera_app thermal-monitor-test
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_rates.py
	$(SITL_VIDEO_PYTHON) sitl/test_live_tracking.py --build $(SITL_BUILD)

sitl-angle-hold-test: _sitl-mt11 _sitl-a8 _sitl-zr10
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend mt11 --build $(SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend mt11 --build $(SITL_BUILD) --orientation inverted
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend a8 --build $(A8_SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend a8 --build $(A8_SITL_BUILD) --orientation inverted
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend zr10 --build $(ZR10_SITL_BUILD)
	$(SITL_VIDEO_PYTHON) sitl/test_gimbal_angle_hold.py --backend zr10 --build $(ZR10_SITL_BUILD) --orientation inverted

.PHONY: sitl-misb-telemetry-test
sitl-misb-telemetry-test: _sitl-mt11 _sitl-a8 _sitl-zr10 _sitl-z1mini _sitl-zt30
	$(SITL_VIDEO_PYTHON) sitl/test_misb_telemetry.py
	$(SITL_VIDEO_PYTHON) sitl/test_misb_telemetry.py --codec h265

sitl-video-telemetry-test: _sitl-mt11
	python3 sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --check-stale
	python3 sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --codec hevc

.PHONY: sitl-buffering-test
sitl-buffering-test: _sitl-mt11
	python3 sitl/test_buffered_terrain.py --build $(SITL_BUILD)

.PHONY: sitl-terrain-test a8_sitl-terrain-test
SITL_TERRAIN_PYTHON ?= $(SITL_VIDEO_PYTHON)

.PHONY: sitl-image-controls-test
sitl-image-controls-test: _sitl-mt11 _sitl-a8 _sitl-zr10 _sitl-z1mini
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend mt11
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend a8
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend zr10
	$(SITL_VIDEO_PYTHON) sitl/test_image_controls.py --backend z1mini
sitl-terrain-test: _sitl-mt11
	$(SITL_TERRAIN_PYTHON) sitl/test_terrain_video.py
	$(SITL_TERRAIN_PYTHON) sitl/test_video_telemetry.py --backend mt11 --build $(SITL_BUILD) --terrain

a8_sitl-terrain-test: _sitl-a8
	$(SITL_TERRAIN_PYTHON) sitl/test_terrain_video.py
	$(SITL_TERRAIN_PYTHON) sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD) --terrain

a8_sitl-video-telemetry-test: _sitl-a8
	python3 sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD)
	python3 sitl/test_video_telemetry.py --backend a8 --build $(A8_SITL_BUILD) --codec hevc

sitl-armed-recording-test: _sitl-mt11
	python3 sitl/test_armed_recording.py --backend mt11 --build $(SITL_BUILD)

a8_sitl-armed-recording-test: _sitl-a8
	python3 sitl/test_armed_recording.py --backend a8 --build $(A8_SITL_BUILD)

sitl-supportproxy-test: _sitl-mt11
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case signed --reconnect
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case session
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case hevc
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case disabled
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case video-only
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --case single-video
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --raw-thermal --reconnect \
		$(if $(MAVPROXY_REPO),--mavproxy $(MAVPROXY_REPO))

a8_sitl-supportproxy-test: _sitl-a8
	python3 sitl/test_support_proxy.py --proxy $(SUPPORTPROXY_ROOT) --backend a8 --build $(A8_SITL_BUILD)

recording-recovery-test:
	+python3 web/tests/test_recording_download.py

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

.PHONY: z1mini-test z1mini-test-headers
z1mini-test-headers:
	$(MAKE) -C web build/version.h build/icons.h
z1mini-test: z1mini-test-headers
	+python3 tests/test_z1mini.py
	+python3 web/tests/test_z1mini_upgrade.py
	python3 tests/test_z1mini_4k.py
	python3 tests/test_z1mini_4k.py --orientation inverted
	python3 tests/test_z1mini_service.py
	python3 tests/test_z1mini_service.py --native

.PHONY: z1mini-overlay-test
z1mini-overlay-test:
	$(MAKE) -C camera_app z1mini-overlay-test Z1MINI_AX_SDK_INCLUDE='$(Z1MINI_AX_SDK_INCLUDE)'

.PHONY: targets-test
targets-test: _targets
	python3 tests/test_targets.py
	python3 tests/test_launcher_config.py
	python3 tests/test_windows_package_ports.py

.PHONY: z1mini_retained_isp_package
z1mini_retained_isp_package: _z1mini-binaries
	python3 tools/build_z1mini_package.py '$(Z1MINI_RETAINED_ISP_PACKAGE_OUT)'

# ABI observation probe; no explicit pipeline Init/Exit/bind/buffer calls.
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

.PHONY: zt30_ram_camera zt30_ram_camera_build zt30_dev_camera zt30_dev_camera_build
zt30_ram_camera: build/zt30/encoder-sdk.so zt30_ram_camera_build
zt30_ram_camera_build: _generated
	$(MAKE) -C camera_app CAMERA_BACKEND=zt30 ZT30_RAM_BUILD=1 CROSS_COMPILE=$(ZT30_CROSS_COMPILE) all
zt30_dev_camera: _zt30-dev-camera
zt30_dev_camera_build: _zt30-dev-camera-build

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

.PHONY: zt30_native_control-test zt30-zoom-test
zt30-zoom-test:
	@mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -Icamera_app/include tests/test_zt30_zoom.cpp -pthread -o build/tests/zt30-zoom-test
	build/tests/zt30-zoom-test

zt30_native_control-test: _generated
	mkdir -p build/tests
	$(ZT30_HOST_CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers -DCAMERA_APP_ZT30_DEV -DAPCAM_TARGET=APCAM_TARGET_ZT30 -Iinclude -Icamera_app/include -Icamera_app/build tests/test_zt30_native_control.cpp camera_app/src/protocol/siyi.cpp camera_app/src/protocol/private_uart.cpp -lutil -Wl,--wrap=write -o build/tests/zt30-native-control-test
	build/tests/zt30-native-control-test

# Compile the native protocol policy without the hardware media factory.
.PHONY: zt30_camera_information-test
zt30_camera_information-test: _generated
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

.PHONY: zt30_slots-test zt30-firmware-test
zt30-firmware-test:
	python3 tests/test_zt30_firmware.py

zt30_slots-test:
	python3 tests/test_zt30_slots.py

.PHONY: zt30_bundle-test
zt30_bundle-test:
	python3 tests/test_zt30_bundle.py

.PHONY: zt30_update-test
zt30_update-test: _generated
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

.PHONY: vpn-test
vpn-test:
	$(MAKE) -C web generated
	mkdir -p build/tests
	$(CXX) -std=gnu++17 -O2 -Wall -Wextra -Werror -DAPCAM_TARGET=APCAM_TARGET_MT11 tests/test_vpn_service.cpp -o build/tests/vpn-service-test
	python3 -c 'import subprocess,tempfile; t=tempfile.TemporaryDirectory(prefix="apcg-vpn-service-"); subprocess.run(["build/tests/vpn-service-test",t.name],check=True,timeout=90)'
	python3 tests/test_vpn_resources.py
	python3 web/tests/test_vpn.py
	$(MAKE) -C web CAMERA_BACKEND=mt11 portable-sitl SITL_BIN_ROOT=../build/vpn-portable-mt11
	$(MAKE) -C web CAMERA_BACKEND=zt30 portable-sitl SITL_BIN_ROOT=../build/vpn-portable-zt30

# Standalone names for steps the camera and release targets run themselves.
.PHONY: camera-definitions mt11_tools
camera-definitions: _camera-definitions
mt11_tools: _mt11_tools
