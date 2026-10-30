#include "camera_app/misb.h"

#include <cmath>
#include <cstring>

namespace {
constexpr double degrees = 180.0 / 3.14159265358979323846;
constexpr uint8_t uas_key[] = {
    0x06,0x0e,0x2b,0x34,0x02,0x0b,0x01,0x01,
    0x0e,0x01,0x03,0x01,0x01,0x00,0x00,0x00,
};

struct LocalSet {
    uint8_t data[CA_MISB_KLV_MAX]{};
    size_t used = 0;
    bool valid = true;

    void item(uint8_t tag, uint64_t value, unsigned bytes)
    {
        if (used + 2 + bytes > sizeof(data)) { valid = false; return; }
        data[used++] = tag; data[used++] = bytes;
        for (unsigned i = bytes; i; --i) data[used++] = value >> ((i-1)*8);
    }
    void text(uint8_t tag, const char *s, size_t limit)
    {
        size_t n = strnlen(s, limit);
        if (!n) return;
        if (n >= 128 || used + 2 + n > sizeof(data)) { valid = false; return; }
        data[used++] = tag; data[used++] = n;
        memcpy(data + used, s, n); used += n;
    }
    void unsigned_angle(uint8_t tag, double value, double range, unsigned bytes)
    {
        if (!std::isfinite(value) || value < 0 || value > range) return;
        const uint64_t max = (UINT64_C(1) << (bytes*8)) - 1;
        item(tag, uint64_t(std::llround(value / range * double(max))), bytes);
    }
    void signed_angle(uint8_t tag, double value, double range, unsigned bytes)
    {
        if (!std::isfinite(value)) return;
        const int64_t max = (INT64_C(1) << (bytes*8-1)) - 1;
        // The most negative integer is the ST 0601 out-of-range indicator.
        item(tag, std::fabs(value) <= range
            ? uint64_t(std::llround(value / range * double(max)))
            : uint64_t(-max-1), bytes);
    }
};

double positive_angle(double a)
{
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

double angle_degrees(float radians,double limit)
{
    const double angle=double(radians)*degrees;
    // Float sensor values at a boundary (notably pi/2) can convert a few
    // microdegrees outside it. Only clamp values inside the float boundary.
    if(std::fabs(radians)<=float(limit/degrees))
        return std::fmax(-limit,std::fmin(limit,angle));
    return angle;
}

void rotation(double roll, double pitch, double yaw, double r[3][3])
{
    const double cr=cos(roll), sr=sin(roll), cp=cos(pitch), sp=sin(pitch),
                 cy=cos(yaw), sy=sin(yaw);
    r[0][0]=cy*cp; r[0][1]=cy*sp*sr-sy*cr; r[0][2]=cy*sp*cr+sy*sr;
    r[1][0]=sy*cp; r[1][1]=sy*sp*sr+cy*cr; r[1][2]=sy*sp*cr-cy*sr;
    r[2][0]=-sp;   r[2][1]=cp*sr;          r[2][2]=cp*cr;
}

bool attitude(double roll, double pitch, double yaw)
{
    return std::isfinite(roll) && std::isfinite(pitch) && std::isfinite(yaw);
}
}

size_t ca_misb_klv(const ca_metadata *m, const timespec *utc,
                   unsigned width, unsigned height, uint8_t *out, size_t capacity)
{
    if (!m || !utc || !out || utc->tv_sec < 0 || utc->tv_nsec < 0 ||
        utc->tv_nsec >= 1000000000 ||
        uint64_t(utc->tv_sec) > (UINT64_MAX - 999999) / 1000000) return 0;
    LocalSet set;
    // ST 0601 requires timestamp first, version present, checksum last.
    set.item(2, uint64_t(utc->tv_sec)*1000000 + utc->tv_nsec/1000, 8);
    set.text(11, m->model, sizeof(m->model));
    set.text(12, "Geodetic WGS84", 14);
    if (m->have_position) {
        if (m->lat_e7 >= -900000000 && m->lat_e7 <= 900000000)
            set.signed_angle(13, m->lat_e7*1e-7, 90, 4);
        if (m->lon_e7 >= -1800000000 && m->lon_e7 <= 1800000000)
            set.signed_angle(14, m->lon_e7*1e-7, 180, 4);
        // Tag 15 uses MSL; relative altitude must never be substituted here.
        if (std::isfinite(m->alt_amsl_m) && m->alt_amsl_m >= -900 && m->alt_amsl_m <= 19000)
            set.unsigned_angle(15, m->alt_amsl_m + 900, 19900, 2);
    }
    const bool vehicle = m->have_vehicle_attitude &&
        attitude(m->vehicle_roll_rad,m->vehicle_pitch_rad,m->vehicle_yaw_rad);
    if (vehicle) {
        set.unsigned_angle(5, positive_angle(m->vehicle_yaw_rad*degrees), 360, 2);
        // Keep legacy tags for older viewers; full-range tags take precedence
        // under ST 0601.8-16. Both full-range fields map +/-90 to signed int32.
        set.signed_angle(6, angle_degrees(m->vehicle_pitch_rad,20), 20, 2);
        set.signed_angle(7, angle_degrees(m->vehicle_roll_rad,50), 50, 2);
        set.signed_angle(90, angle_degrees(m->vehicle_pitch_rad,90), 90, 4);
        set.signed_angle(91, angle_degrees(m->vehicle_roll_rad,90), 90, 4);
    }
    if (std::isfinite(m->hfov_deg) && m->hfov_deg > 0 && m->hfov_deg < 180) {
        set.unsigned_angle(16, m->hfov_deg, 180, 2);
        if (width && height)
            set.unsigned_angle(17, 2*atan(tan(m->hfov_deg/degrees/2)*height/width)*degrees, 180, 2);
    }
    if (vehicle && m->have_gimbal_attitude &&
        attitude(m->gimbal_roll_rad,m->gimbal_pitch_rad,m->gimbal_yaw_rad)) {
        double platform[3][3], camera[3][3], relative[3][3]{};
        rotation(m->vehicle_roll_rad,m->vehicle_pitch_rad,m->vehicle_yaw_rad,platform);
        // Feedback roll/pitch are earth stabilized; yaw is vehicle relative.
        rotation(m->gimbal_roll_rad,m->gimbal_pitch_rad,
                 double(m->vehicle_yaw_rad)+m->gimbal_yaw_rad,camera);
        for (unsigned i=0;i<3;i++) for (unsigned j=0;j<3;j++)
            for (unsigned k=0;k<3;k++) relative[i][j]+=platform[k][i]*camera[k][j];
        const double pitch=atan2(-relative[2][0],hypot(relative[0][0],relative[1][0]));
        double roll, yaw;
        if (hypot(relative[0][0],relative[1][0]) > 1e-8) {
            yaw=atan2(relative[1][0],relative[0][0]);
            roll=atan2(relative[2][1],relative[2][2]);
        } else {
            yaw=atan2(-relative[0][1],relative[1][1]); roll=0;
        }
        set.unsigned_angle(18, positive_angle(yaw*degrees), 360, 4);
        set.signed_angle(19, pitch*degrees, 180, 4);
        set.unsigned_angle(20, positive_angle(roll*degrees), 360, 4);
    }
    set.item(65, 8, 1); // Deliberately implement the interoperable ST 0601.8 subset.
    const size_t length = set.used + 4; // checksum tag, length, uint16
    const size_t ber = length < 128 ? 1 : length < 256 ? 2 : 3;
    if (!set.valid || sizeof(uas_key)+ber+length > capacity) return 0;
    memcpy(out, uas_key, sizeof(uas_key));
    size_t used=sizeof(uas_key);
    if (ber > 1) out[used++]=0x80 | (ber-1);
    if (ber == 3) out[used++]=length >> 8;
    out[used++]=length;
    memcpy(out+used,set.data,set.used); used+=set.used;
    out[used++]=1; out[used++]=2;
    unsigned checksum=0;
    for (size_t i=0;i<used;i++) checksum+=unsigned(out[i]) << (i%2 ? 0 : 8);
    out[used++]=checksum >> 8; out[used++]=checksum;
    return used;
}
