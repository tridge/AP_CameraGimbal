#pragma once
#include <cstdint>

// Unstretched luma from the selected live sensor, before any OSD is drawn.
// Backends copy/release SDK buffers before returning. No vendor buffer escapes.
constexpr unsigned CA_TRACK_WIDTH = 320, CA_TRACK_HEIGHT = 256;
struct ca_tracking_frame {
    unsigned width = 0, height = 0;
    uint64_t timestamp_ms = 0;
    uint8_t pixels[CA_TRACK_WIDTH * CA_TRACK_HEIGHT] {};
};
struct ca_tracking_rect { float left, top, right, bottom; }; // normalized 0..1
enum ca_tracking_state { CA_TRACK_IDLE, CA_TRACK_ACQUIRING, CA_TRACK_ACTIVE,
                         CA_TRACK_COASTING, CA_TRACK_LOST };
enum ca_tracking_owner { CA_TRACK_OWNER_NONE, CA_TRACK_OWNER_SIYI, CA_TRACK_OWNER_MAVLINK };
struct ca_tracking_pose {
    // Camera roll/pitch are stabilized, yaw is earth-frame when aircraft
    // attitude is available. Do not add aircraft pitch to stabilized pitch.
    float roll = 0, pitch = 0, yaw = 0, hfov = 0;
    bool valid = false;
};
struct ca_tracking_status {
    ca_tracking_state state = CA_TRACK_IDLE;
    ca_tracking_owner owner = CA_TRACK_OWNER_NONE;
    ca_tracking_rect rect {};
    uint64_t timestamp_ms = 0, confirmed_ms = 0;
    float quality = 0;
    float pitch_error = 0, yaw_error = 0;
    float pitch_rate = 0, yaw_rate = 0; // inertial target LOS, radians/s
    ca_tracking_pose pose; // attitude at image exposure, for latency compensation
    float aspect = 1;
    uint64_t capture_delay_ms = 0;
};

// All engine calls run on one worker. Exception handling is isolated in the
// implementation; a failed frame returns LOST rather than escaping to the app.
struct ca_image_tracker;
ca_image_tracker *ca_image_tracker_create();
void ca_image_tracker_destroy(ca_image_tracker *tracker);
bool ca_image_tracker_start(ca_image_tracker *tracker, const ca_tracking_frame &frame,
                            const ca_tracking_rect &rect, const ca_tracking_pose &pose);
ca_tracking_status ca_image_tracker_update(ca_image_tracker *tracker,
                                           const ca_tracking_frame &frame,
                                           const ca_tracking_pose &pose);
bool ca_tracking_rect_valid(const ca_tracking_rect &rect);
// MAV_CMD_CAMERA_TRACK_POINT: a square of +-radius (a fraction of image width)
// centred on the point, shrunk to fit the image but no smaller than start() accepts
ca_tracking_rect ca_tracking_point_rect(float x, float y, float radius, float aspect);
// Project a measured rectangle to another exposure/control time. This does
// not refresh confirmation or mutate the correlation tracker.
bool ca_tracking_project(ca_tracking_status &status, const ca_tracking_pose &pose, uint64_t ms);
