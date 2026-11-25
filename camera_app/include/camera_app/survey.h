#pragma once
#include "camera_app/config.h"
#include "camera_app/metadata.h"
#include <stdint.h>
#include <time.h>

struct ca_survey_pose {
    int32_t lat, lon;
    float alt, ground, vn, ve;
};

// Reconstruct and smooth a straight waypoint leg from L1 telemetry. The caller
// must verify this is a waypoint leg: loiter cross-track error is radial.
struct ca_survey_path {
    ca_survey_pose anchor{};
    double course=0;
    unsigned last_distance=0;
    uint64_t updated_ms=0, stable_ms=0;
    bool update(const ca_survey_pose &, float bearing_deg, unsigned distance_m,
                float xtrack_m, uint64_t now);
    bool project(const ca_survey_pose &, uint64_t now, ca_survey_pose &) const;
};

inline unsigned ca_survey_positions(unsigned pattern)
{
    switch (pattern) {
    case CA_SURVEY_BOTH: return 9;
    case CA_SURVEY_LEFT_RIGHT:
    case CA_SURVEY_FORE_AFT: return 2;
    case CA_SURVEY_FORE_ONLY:
    case CA_SURVEY_FORE_WIDE:
    case CA_SURVEY_PORT:
    case CA_SURVEY_STARBOARD: return 3;
    default: return 0;
    }
}

// A fixed ground grid for one straight flight leg. No target moves during a burst.
struct ca_survey_grid {
    ca_survey_pose origin;
    double course, spacing, cross_spacing, speed, height, cycle_s;
    double side; // sideways offset of left/right columns in metres
    double budget[9];
    unsigned slots[9], slot_count; // view IDs: original grid 0..8, forward pitch views 9..11
    int fore_rows, aft_rows;
    uint64_t start_ms;
    float nominal_overlap;
    bool init(const ca_survey_pose &, const ca_survey_config &, float hfov,
              float aspect, float rate_deg_s, uint64_t now);
    void target(int64_t cycle, unsigned slot, int32_t &lat, int32_t &lon,
                int64_t &row, int &column) const;
    void target_ahead(const ca_survey_pose &, double lead_m, double fore_m, double aft_m,
                      unsigned slot, int32_t &lat, int32_t &lon, int64_t &row, int &column) const;
    double along(const ca_survey_pose &) const;
    double cross_track(const ca_survey_pose &) const;
    double alignment_error(const ca_survey_pose &) const;
    double alignment_limit(const ca_survey_pose &) const;
    uint64_t deadline(uint64_t cycle_start, unsigned slot) const;
};

struct ca_survey_request {
    uint64_t session, generation, eligible_ms, deadline_ms;
    uint32_t leg;
    int32_t index;
    int64_t cycle, row;
    int column;
    unsigned slot, burst, lens;
    unsigned pattern, position, positions; // slot retains its original geometric meaning
    int32_t target_lat, target_lon;
    float target_alt, ground, hfov, aspect;
    float pointing_error_deg, residual_rate_deg_s;
    bool settled;
    bool target_terrain_uploaded;
};

struct ca_survey_result {
    ca_survey_request request;
    ca_metadata pose;
    timespec captured_at;
    uint64_t frame_ms;
    int error;
    bool stream_queued;
    char path[512];
};

// Clockwise footprint corners; attitude uses the firmware's level roll/pitch,
// vehicle-relative yaw convention. Returns false for sky/unbounded rays.
bool ca_survey_footprint(const ca_metadata &, float ground, float hfov, float aspect,
                         double points[4][2]);
// Sidecar and thermal-stream metadata share this exact record.
bool ca_survey_json(const ca_survey_result &, char *, unsigned);
bool ca_survey_write_metadata(const ca_survey_result &);
