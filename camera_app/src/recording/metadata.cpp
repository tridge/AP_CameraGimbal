#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/metadata.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAD_TO_DEG (180.0 / M_PI)

static struct {
    pthread_mutex_t lock;
    char model[CA_METADATA_MODEL_MAX];
    uint64_t position_ms;
    int32_t lat_e7;
    int32_t lon_e7;
    float alt_amsl_m;
    float alt_relative_m;
    float heading_rad;
    uint64_t velocity_ms;
    float vn_m_s, ve_m_s, vd_m_s;
    uint64_t vehicle_attitude_ms;
    float vehicle_roll_rad;
    float vehicle_pitch_rad;
    float vehicle_yaw_rad;
    float vehicle_yaw_rate_rad_s;
    uint64_t gimbal_attitude_ms;
    float gimbal_roll_rad;
    float gimbal_pitch_rad;
    float gimbal_yaw_rad;
    float gimbal_yaw_rate_rad_s;
    float gimbal_pitch_rate_rad_s;
    float zoom;
} state = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 1;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U + 1U;
}

void ca_metadata_set_model(const char *model)
{
    pthread_mutex_lock(&state.lock);
    snprintf(state.model, sizeof(state.model), "%s", model != NULL ? model : "");
    pthread_mutex_unlock(&state.lock);
}

void ca_metadata_set_position(int32_t lat_e7, int32_t lon_e7, float alt_amsl_m,
                              float alt_relative_m, float heading_rad, uint64_t timestamp_ms)
{
    pthread_mutex_lock(&state.lock);
    state.lat_e7 = lat_e7;
    state.lon_e7 = lon_e7;
    state.alt_amsl_m = alt_amsl_m;
    state.alt_relative_m = alt_relative_m;
    state.heading_rad = heading_rad;
    state.position_ms = timestamp_ms ? timestamp_ms : monotonic_ms();
    pthread_mutex_unlock(&state.lock);
}

void ca_metadata_set_vehicle_attitude(float roll_rad, float pitch_rad,
                                      float yaw_rad)
{
    ca_metadata_set_vehicle_attitude_motion(roll_rad, pitch_rad, yaw_rad, NAN);
}

void ca_metadata_set_velocity(float vn_m_s, float ve_m_s, float vd_m_s, uint64_t timestamp_ms)
{
    if (!isfinite(vn_m_s) || !isfinite(ve_m_s) || !isfinite(vd_m_s)) return;
    pthread_mutex_lock(&state.lock);
    state.vn_m_s = vn_m_s;
    state.ve_m_s = ve_m_s;
    state.vd_m_s = vd_m_s;
    state.velocity_ms = timestamp_ms ? timestamp_ms : monotonic_ms();
    pthread_mutex_unlock(&state.lock);
}

void ca_metadata_set_vehicle_attitude_motion(float roll_rad, float pitch_rad,
                                             float yaw_rad, float yaw_rate_rad_s, uint64_t timestamp_ms)
{
    pthread_mutex_lock(&state.lock);
    state.vehicle_yaw_rate_rad_s = yaw_rate_rad_s;
    state.vehicle_roll_rad = roll_rad;
    state.vehicle_pitch_rad = pitch_rad;
    state.vehicle_yaw_rad = yaw_rad;
    state.vehicle_attitude_ms = timestamp_ms ? timestamp_ms : monotonic_ms();
    pthread_mutex_unlock(&state.lock);
}

void ca_metadata_set_gimbal_attitude(float roll_rad, float pitch_rad,
                                     float yaw_rad)
{
    ca_metadata_set_gimbal_attitude_sample(roll_rad, pitch_rad, yaw_rad, monotonic_ms() - 1U);
}

void ca_metadata_set_gimbal_attitude_sample(float roll_rad, float pitch_rad,
                                            float yaw_rad, uint64_t timestamp_ms)
{
    ca_metadata_set_gimbal_attitude_motion(roll_rad, pitch_rad, yaw_rad, NAN, timestamp_ms);
}

void ca_metadata_set_gimbal_attitude_motion(float roll_rad, float pitch_rad,
                                            float yaw_rad, float yaw_rate_rad_s,
                                            uint64_t timestamp_ms)
{
    pthread_mutex_lock(&state.lock);
    uint64_t sample=timestamp_ms+1U;
    if(state.gimbal_attitude_ms && sample>state.gimbal_attitude_ms && sample-state.gimbal_attitude_ms<500)
        state.gimbal_pitch_rate_rad_s=(pitch_rad-state.gimbal_pitch_rad)*1000/(sample-state.gimbal_attitude_ms);
    state.gimbal_roll_rad = roll_rad;
    state.gimbal_pitch_rad = pitch_rad;
    state.gimbal_yaw_rad = yaw_rad;
    state.gimbal_yaw_rate_rad_s = yaw_rate_rad_s;
    state.gimbal_attitude_ms = timestamp_ms + 1U;
    pthread_mutex_unlock(&state.lock);
}

void ca_metadata_set_zoom(float zoom)
{
    pthread_mutex_lock(&state.lock);
    state.zoom = zoom;
    pthread_mutex_unlock(&state.lock);
}

static bool fresh(uint64_t now, uint64_t updated)
{
    return updated != 0U && now - updated <= CA_METADATA_MAX_AGE_MS;
}

void ca_metadata_snapshot(struct ca_metadata *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&state.lock);
    uint64_t now = monotonic_ms();
    memcpy(snapshot->model, state.model, sizeof(snapshot->model));
    snapshot->have_position = fresh(now, state.position_ms);
    if (snapshot->have_position) {
        snapshot->position_age_ms = (unsigned)(now - state.position_ms);
        snapshot->lat_e7 = state.lat_e7;
        snapshot->lon_e7 = state.lon_e7;
        snapshot->alt_amsl_m = state.alt_amsl_m;
        snapshot->alt_relative_m = state.alt_relative_m;
        snapshot->heading_rad = state.heading_rad;
    }
    snapshot->have_velocity = fresh(now, state.velocity_ms);
    if (snapshot->have_velocity) {
        snapshot->velocity_age_ms = (unsigned)(now - state.velocity_ms);
        snapshot->vn_m_s = state.vn_m_s;
        snapshot->ve_m_s = state.ve_m_s;
        snapshot->vd_m_s = state.vd_m_s;
    }
    snapshot->have_vehicle_attitude = fresh(now, state.vehicle_attitude_ms);
    if (snapshot->have_vehicle_attitude) {
        snapshot->vehicle_attitude_age_ms = (unsigned)(now - state.vehicle_attitude_ms);
        snapshot->vehicle_roll_rad = state.vehicle_roll_rad;
        snapshot->vehicle_pitch_rad = state.vehicle_pitch_rad;
        snapshot->vehicle_yaw_rad = state.vehicle_yaw_rad;
        snapshot->vehicle_yaw_rate_rad_s = state.vehicle_yaw_rate_rad_s;
    }
    snapshot->have_gimbal_attitude = fresh(now, state.gimbal_attitude_ms);
    if (snapshot->have_gimbal_attitude) {
        snapshot->gimbal_attitude_age_ms = (unsigned)(now - state.gimbal_attitude_ms);
        snapshot->gimbal_roll_rad = state.gimbal_roll_rad;
        snapshot->gimbal_pitch_rad = state.gimbal_pitch_rad;
        snapshot->gimbal_yaw_rad = state.gimbal_yaw_rad;
        snapshot->gimbal_yaw_rate_rad_s = state.gimbal_yaw_rate_rad_s;
        snapshot->gimbal_pitch_rate_rad_s = state.gimbal_pitch_rate_rad_s;
    }
    snapshot->zoom = state.zoom;
    pthread_mutex_unlock(&state.lock);
}

// The short history aligns frame metadata with the control loop's monotonic
// clock. Extrapolation is bounded; stale sensor data never becomes fresh merely
// because a worker asks for another snapshot.
static struct {
    pthread_mutex_t lock;
    ca_metadata pose[64];
    uint64_t ms[64];
    unsigned next, count;
} history = { .lock=PTHREAD_MUTEX_INITIALIZER };

static void advance_pose(ca_metadata &m, double seconds)
{
    if(m.have_position && m.have_velocity) {
        const double earth=6378137.0, radians=M_PI/180;
        double latitude=m.lat_e7*1e-7*radians;
        m.lat_e7+=llround(m.vn_m_s*seconds/earth/radians*1e7);
        if(fabs(cos(latitude))>.001)
            m.lon_e7=int32_t(llround(remainder(m.lon_e7*1e-7+m.ve_m_s*seconds/(earth*cos(latitude))/radians,360)*1e7));
        m.alt_amsl_m-=m.vd_m_s*seconds;
    }
    if(isfinite(m.vehicle_yaw_rate_rad_s)) m.vehicle_yaw_rad+=m.vehicle_yaw_rate_rad_s*seconds;
    if(isfinite(m.gimbal_yaw_rate_rad_s)) m.gimbal_yaw_rad+=m.gimbal_yaw_rate_rad_s*seconds;
    if(isfinite(m.gimbal_pitch_rate_rad_s)) m.gimbal_pitch_rad+=m.gimbal_pitch_rate_rad_s*seconds;
}

void ca_metadata_record_sample(uint64_t now)
{
    ca_metadata m; ca_metadata_snapshot(&m);
    m.have_position=m.have_position && m.position_age_ms<=250;
    m.have_vehicle_attitude=m.have_vehicle_attitude && m.vehicle_attitude_age_ms<=250;
    m.have_gimbal_attitude=m.have_gimbal_attitude && m.gimbal_attitude_age_ms<=250;
    // Correct each source to the sample instant using that source's own age.
    ca_metadata position=m; advance_pose(position,m.position_age_ms*.001);
    m.lat_e7=position.lat_e7; m.lon_e7=position.lon_e7; m.alt_amsl_m=position.alt_amsl_m;
    if(isfinite(m.vehicle_yaw_rate_rad_s)) m.vehicle_yaw_rad+=m.vehicle_yaw_rate_rad_s*m.vehicle_attitude_age_ms*.001;
    if(isfinite(m.gimbal_yaw_rate_rad_s)) m.gimbal_yaw_rad+=m.gimbal_yaw_rate_rad_s*m.gimbal_attitude_age_ms*.001;
    if(isfinite(m.gimbal_pitch_rate_rad_s)) m.gimbal_pitch_rad+=m.gimbal_pitch_rate_rad_s*m.gimbal_attitude_age_ms*.001;
    pthread_mutex_lock(&history.lock);
    unsigned last=(history.next+63)%64;
    if(!history.count || now>=history.ms[last]+20) {
        history.pose[history.next]=m; history.ms[history.next]=now;
        history.next=(history.next+1)%64;
        if(history.count<64) ++history.count;
    }
    pthread_mutex_unlock(&history.lock);
}

bool ca_metadata_at(uint64_t when, ca_metadata *out)
{
    memset(out,0,sizeof(*out));
    pthread_mutex_lock(&history.lock);
    int before=-1,after=-1;
    for(unsigned i=0;i<history.count;i++) {
        unsigned k=(history.next+64-history.count+i)%64;
        if(history.ms[k]<=when) before=k;
        else { after=k; break; }
    }
    if(before<0 || when-history.ms[before]>200) { pthread_mutex_unlock(&history.lock); return false; }
    *out=history.pose[before];
    if(after>=0 && history.ms[after]-history.ms[before]<=250) {
        const auto &b=history.pose[after];
        double t=double(when-history.ms[before])/(history.ms[after]-history.ms[before]);
        out->lat_e7+=llround((double(b.lat_e7)-out->lat_e7)*t);
        out->lon_e7=int32_t(llround(remainder(out->lon_e7*1e-7+remainder((double(b.lon_e7)-out->lon_e7)*1e-7,360)*t,360)*1e7));
        out->alt_amsl_m+=(b.alt_amsl_m-out->alt_amsl_m)*t;
#define ANGLE(field) out->field+=remainder(b.field-out->field,2*M_PI)*t
        ANGLE(vehicle_roll_rad); ANGLE(vehicle_pitch_rad); ANGLE(vehicle_yaw_rad);
        ANGLE(gimbal_roll_rad); ANGLE(gimbal_pitch_rad); ANGLE(gimbal_yaw_rad);
#undef ANGLE
        out->have_position=out->have_position && b.have_position;
        out->have_vehicle_attitude=out->have_vehicle_attitude && b.have_vehicle_attitude;
        out->have_gimbal_attitude=out->have_gimbal_attitude && b.have_gimbal_attitude;
    } else advance_pose(*out,(when-history.ms[before])*.001);
    pthread_mutex_unlock(&history.lock);
    return out->have_position && out->have_vehicle_attitude && out->have_gimbal_attitude;
}

/* ---- EXIF (TIFF little-endian) ---- */

enum tiff_type {
    TIFF_BYTE = 1,
    TIFF_ASCII = 2,
    TIFF_SHORT = 3,
    TIFF_LONG = 4,
    TIFF_RATIONAL = 5,
    TIFF_UNDEFINED = 7,
    TIFF_SRATIONAL = 10,
};

struct tiff_entry {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint8_t data[24];
};

struct tiff {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
};

#define IFD_MAX_ENTRIES 16

struct ifd {
    struct tiff_entry entries[IFD_MAX_ENTRIES];
    unsigned count;
};

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    put_u16(out, (uint16_t)value);
    put_u16(out + 2, (uint16_t)(value >> 16));
}

static size_t type_size(uint16_t type)
{
    switch (type) {
    case TIFF_SHORT: return 2;
    case TIFF_LONG: return 4;
    case TIFF_RATIONAL:
    case TIFF_SRATIONAL: return 8;
    default: return 1;
    }
}

static struct tiff_entry *ifd_add(struct ifd *ifd, uint16_t tag, uint16_t type,
                                  uint32_t count)
{
    struct tiff_entry *entry;
    if (ifd->count >= IFD_MAX_ENTRIES) return NULL;
    entry = &ifd->entries[ifd->count++];
    memset(entry, 0, sizeof(*entry));
    entry->tag = tag;
    entry->type = type;
    entry->count = count;
    return entry;
}

static void ifd_ascii(struct ifd *ifd, uint16_t tag, const char *text)
{
    size_t length = strlen(text) + 1U;
    struct tiff_entry *entry;
    if (length > sizeof(entry->data)) return;
    entry = ifd_add(ifd, tag, TIFF_ASCII, (uint32_t)length);
    if (entry != NULL) memcpy(entry->data, text, length);
}

static void ifd_long(struct ifd *ifd, uint16_t tag, uint32_t value)
{
    struct tiff_entry *entry = ifd_add(ifd, tag, TIFF_LONG, 1);
    if (entry != NULL) put_u32(entry->data, value);
}

static void ifd_bytes(struct ifd *ifd, uint16_t tag, uint16_t type,
                      const uint8_t *bytes, uint32_t count)
{
    struct tiff_entry *entry;
    if (count > sizeof(entry->data)) return;
    entry = ifd_add(ifd, tag, type, count);
    if (entry != NULL) memcpy(entry->data, bytes, count);
}

static void ifd_rationals(struct ifd *ifd, uint16_t tag, uint16_t type,
                          const uint32_t *pairs, uint32_t count)
{
    struct tiff_entry *entry;
    if (count * 8U > sizeof(entry->data)) return;
    entry = ifd_add(ifd, tag, type, count);
    if (entry == NULL) return;
    for (uint32_t i = 0; i < count * 2U; i++) {
        put_u32(entry->data + i * 4U, pairs[i]);
    }
}

static void ifd_rational(struct ifd *ifd, uint16_t tag, uint32_t numerator,
                         uint32_t denominator)
{
    uint32_t pair[2] = {numerator, denominator};
    ifd_rationals(ifd, tag, TIFF_RATIONAL, pair, 1);
}

static int entry_compare(const void *a, const void *b)
{
    const struct tiff_entry *x = (const tiff_entry*)(a), *y = (const tiff_entry*)(b);
    return (int)x->tag - (int)y->tag;
}

static void tiff_append(struct tiff *tiff, const void *data, size_t length)
{
    if (tiff->overflow || tiff->capacity - tiff->length < length) {
        tiff->overflow = true;
        return;
    }
    memcpy(tiff->buffer + tiff->length, data, length);
    tiff->length += length;
}

/* appends the IFD and its out-of-line data; returns the IFD offset and
 * records where pointer tags (0x8769/0x8825) keep their values */
static uint32_t tiff_write_ifd(struct tiff *tiff, struct ifd *ifd,
                               size_t *exif_pointer, size_t *gps_pointer)
{
    uint32_t ifd_offset = (uint32_t)tiff->length;
    size_t data_offset;
    uint8_t scratch[4];

    qsort(ifd->entries, ifd->count, sizeof(ifd->entries[0]), entry_compare);
    data_offset = tiff->length + 2U + (size_t)ifd->count * 12U + 4U;
    put_u16(scratch, (uint16_t)ifd->count);
    tiff_append(tiff, scratch, 2);
    for (unsigned i = 0; i < ifd->count; i++) {
        const struct tiff_entry *entry = &ifd->entries[i];
        size_t size = type_size(entry->type) * entry->count;
        uint8_t raw[12] = {};
        put_u16(raw, entry->tag);
        put_u16(raw + 2, entry->type);
        put_u32(raw + 4, entry->count);
        if (size <= 4U) {
            memcpy(raw + 8, entry->data, size);
        } else {
            put_u32(raw + 8, (uint32_t)data_offset);
            data_offset += (size + 1U) & ~(size_t)1U;
        }
        if (entry->tag == 0x8769U && exif_pointer != NULL) {
            *exif_pointer = tiff->length + 8U;
        }
        if (entry->tag == 0x8825U && gps_pointer != NULL) {
            *gps_pointer = tiff->length + 8U;
        }
        tiff_append(tiff, raw, sizeof(raw));
    }
    memset(scratch, 0, sizeof(scratch));
    tiff_append(tiff, scratch, 4);
    for (unsigned i = 0; i < ifd->count; i++) {
        const struct tiff_entry *entry = &ifd->entries[i];
        size_t size = type_size(entry->type) * entry->count;
        if (size <= 4U) continue;
        tiff_append(tiff, entry->data, size);
        if (size & 1U) tiff_append(tiff, scratch, 1);
    }
    return ifd_offset;
}

static void gps_coordinate(struct ifd *ifd, uint16_t tag, int32_t value_e7)
{
    /* degrees, minutes and 1/10000 seconds, all exact from the e7 value */
    int64_t total = value_e7 < 0 ? -(int64_t)value_e7 : value_e7;
    uint32_t degrees = (uint32_t)(total / 10000000);
    int64_t minutes_e7 = (total % 10000000) * 60;
    uint32_t minutes = (uint32_t)(minutes_e7 / 10000000);
    uint32_t seconds_e4 = (uint32_t)((minutes_e7 % 10000000) * 60 / 1000);
    uint32_t pairs[6] = {degrees, 1, minutes, 1, seconds_e4, 10000};
    ifd_rationals(ifd, tag, TIFF_RATIONAL, pairs, 3);
}

static float wrap_360(double degrees)
{
    degrees = fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return (float)degrees;
}

static size_t build_exif(const struct ca_metadata *metadata,
                         const struct timespec *captured_at,
                         const struct tm *local, const struct tm *utc,
                         uint8_t *output, size_t capacity)
{
    static const uint8_t prefix[] = {'E', 'x', 'i', 'f', 0, 0};
    static const uint8_t header[] = {'I', 'I', 0x2a, 0, 8, 0, 0, 0};
    /* TIFF offsets count from the byte order mark, after the Exif prefix */
    struct tiff tiff = {.buffer = output + sizeof(prefix),
                        .capacity = capacity - sizeof(prefix)};
    struct ifd ifd0 = {}, exif = {}, gps = {};
    size_t exif_pointer = 0, gps_pointer = 0;
    uint32_t offset;
    char text[32];
    uint8_t version[4] = {'0', '2', '3', '2'};

    if (capacity < sizeof(prefix)) return 0;
    memcpy(output, prefix, sizeof(prefix));
    tiff_append(&tiff, header, sizeof(header));

    ifd_ascii(&ifd0, 0x010f, "ArduPilot");
    ifd_ascii(&ifd0, 0x0110, metadata->model[0] != '\0' ? metadata->model
                                                         : "AP_CameraGimbal");
    strftime(text, sizeof(text), "%Y:%m:%d %H:%M:%S", local);
    ifd_ascii(&ifd0, 0x0132, text);
    ifd_ascii(&ifd0, 0x0131, "AP_CameraGimbal");
    ifd_long(&ifd0, 0x8769, 0);
    if (metadata->have_position) ifd_long(&ifd0, 0x8825, 0);

    ifd_bytes(&exif, 0x9000, TIFF_UNDEFINED, version, sizeof(version));
    ifd_ascii(&exif, 0x9003, text);
    ifd_ascii(&exif, 0x9004, text);
    snprintf(text, sizeof(text), "%03ld", captured_at->tv_nsec / 1000000L);
    ifd_ascii(&exif, 0x9291, text);
    ifd_ascii(&exif, 0x9292, text);
    snprintf(text, sizeof(text), "%c%02ld:%02ld",
             local->tm_gmtoff < 0 ? '-' : '+', labs(local->tm_gmtoff) / 3600,
             (labs(local->tm_gmtoff) % 3600) / 60);
    ifd_ascii(&exif, 0x9011, text);
    ifd_ascii(&exif, 0x9012, text);
    if (metadata->zoom > 0.0f) {
        ifd_rational(&exif, 0xa404, (uint32_t)(metadata->zoom * 100.0f + 0.5f),
                     100);
    }

    if (metadata->have_position) {
        static const uint8_t gps_version[4] = {2, 3, 0, 0};
        uint8_t altitude_ref = metadata->alt_amsl_m < 0.0f ? 1U : 0U;
        float altitude = fabsf(metadata->alt_amsl_m);
        uint32_t time[6] = {(uint32_t)utc->tm_hour, 1, (uint32_t)utc->tm_min, 1,
                            (uint32_t)utc->tm_sec, 1};
        double direction = NAN;

        ifd_bytes(&gps, 0x0000, TIFF_BYTE, gps_version, sizeof(gps_version));
        ifd_ascii(&gps, 0x0001, metadata->lat_e7 < 0 ? "S" : "N");
        gps_coordinate(&gps, 0x0002, metadata->lat_e7);
        ifd_ascii(&gps, 0x0003, metadata->lon_e7 < 0 ? "W" : "E");
        gps_coordinate(&gps, 0x0004, metadata->lon_e7);
        ifd_bytes(&gps, 0x0005, TIFF_BYTE, &altitude_ref, 1);
        ifd_rational(&gps, 0x0006, (uint32_t)(altitude * 100.0f + 0.5f), 100);
        ifd_rationals(&gps, 0x0007, TIFF_RATIONAL, time, 3);
        ifd_ascii(&gps, 0x0012, "WGS-84");
        strftime(text, sizeof(text), "%Y:%m:%d", utc);
        ifd_ascii(&gps, 0x001d, text);
        /* image direction: the gimbal's absolute heading when known */
        if (metadata->have_gimbal_attitude && metadata->have_vehicle_attitude) {
            direction = (metadata->vehicle_yaw_rad + metadata->gimbal_yaw_rad) *
                        RAD_TO_DEG;
        } else if (!metadata->have_gimbal_attitude) {
            direction = metadata->heading_rad * RAD_TO_DEG;
        }
        if (!isnan(direction)) {
            ifd_ascii(&gps, 0x0010, "T");
            ifd_rational(&gps, 0x0011,
                         (uint32_t)(wrap_360(direction) * 100.0f + 0.5f), 100);
        }
    }

    tiff_write_ifd(&tiff, &ifd0, &exif_pointer, &gps_pointer);
    offset = tiff_write_ifd(&tiff, &exif, NULL, NULL);
    if (!tiff.overflow) put_u32(tiff.buffer + exif_pointer, offset);
    if (metadata->have_position) {
        offset = tiff_write_ifd(&tiff, &gps, NULL, NULL);
        if (!tiff.overflow) put_u32(tiff.buffer + gps_pointer, offset);
    }
    return tiff.overflow ? 0U : tiff.length + sizeof(prefix);
}

/* ---- XMP ---- */

static size_t build_xmp(const struct ca_metadata *metadata,
                        const struct timespec *captured_at,
                        const struct tm *local, char *output, size_t capacity)
{
    size_t length = 0;
    char stamp[48];
    int written;

#define XMP(...)                                                           \
    do {                                                                   \
        written = snprintf(output + length, capacity - length, __VA_ARGS__); \
        if (written < 0 || (size_t)written >= capacity - length) return 0; \
        length += (size_t)written;                                         \
    } while (0)

    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", local);
    XMP("http://ns.adobe.com/xap/1.0/%c", '\0');
    XMP("<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\""
        " xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\""
        " xmlns:drone-dji=\"http://www.dji.com/drone-dji/1.0/\""
        " xmlns:apcg=\"http://ardupilot.org/camera-gimbal/1.0/\"");
    XMP(" xmp:CreateDate=\"%s.%03ld%c%02ld:%02ld\"", stamp,
        captured_at->tv_nsec / 1000000L, local->tm_gmtoff < 0 ? '-' : '+',
        labs(local->tm_gmtoff) / 3600, (labs(local->tm_gmtoff) % 3600) / 60);
    XMP(" apcg:CaptureUnixTime=\"%lld.%03ld\"", (long long)captured_at->tv_sec,
        captured_at->tv_nsec / 1000000L);
    if (metadata->have_position) {
        XMP(" drone-dji:GpsLatitude=\"%.7f\" drone-dji:GpsLongitude=\"%.7f\""
            " drone-dji:AbsoluteAltitude=\"%+.2f\""
            " drone-dji:RelativeAltitude=\"%+.2f\""
            " apcg:Latitude=\"%.7f\" apcg:Longitude=\"%.7f\""
            " apcg:AltitudeAMSL=\"%.2f\" apcg:AltitudeRelative=\"%.2f\""
            " apcg:VehicleHeading=\"%.2f\" apcg:PositionAgeMs=\"%u\"",
            metadata->lat_e7 * 1e-7, metadata->lon_e7 * 1e-7,
            (double)metadata->alt_amsl_m, (double)metadata->alt_relative_m,
            metadata->lat_e7 * 1e-7, metadata->lon_e7 * 1e-7,
            (double)metadata->alt_amsl_m, (double)metadata->alt_relative_m,
            (double)wrap_360(metadata->heading_rad * RAD_TO_DEG),
            metadata->position_age_ms);
    }
    if (metadata->have_vehicle_attitude) {
        XMP(" drone-dji:FlightRollDegree=\"%+.2f\""
            " drone-dji:FlightPitchDegree=\"%+.2f\""
            " drone-dji:FlightYawDegree=\"%+.2f\""
            " apcg:VehicleRoll=\"%.2f\" apcg:VehiclePitch=\"%.2f\""
            " apcg:VehicleYaw=\"%.2f\"",
            metadata->vehicle_roll_rad * RAD_TO_DEG,
            metadata->vehicle_pitch_rad * RAD_TO_DEG,
            metadata->vehicle_yaw_rad * RAD_TO_DEG,
            metadata->vehicle_roll_rad * RAD_TO_DEG,
            metadata->vehicle_pitch_rad * RAD_TO_DEG,
            metadata->vehicle_yaw_rad * RAD_TO_DEG);
    }
    if (metadata->have_gimbal_attitude) {
        XMP(" drone-dji:GimbalRollDegree=\"%+.2f\""
            " drone-dji:GimbalPitchDegree=\"%+.2f\""
            " apcg:GimbalRoll=\"%.2f\" apcg:GimbalPitch=\"%.2f\""
            " apcg:GimbalYawVehicle=\"%.2f\"",
            metadata->gimbal_roll_rad * RAD_TO_DEG,
            metadata->gimbal_pitch_rad * RAD_TO_DEG,
            metadata->gimbal_roll_rad * RAD_TO_DEG,
            metadata->gimbal_pitch_rad * RAD_TO_DEG,
            metadata->gimbal_yaw_rad * RAD_TO_DEG);
        if (metadata->have_vehicle_attitude) {
            double yaw = (metadata->vehicle_yaw_rad + metadata->gimbal_yaw_rad) *
                         RAD_TO_DEG;
            XMP(" drone-dji:GimbalYawDegree=\"%+.2f\" apcg:GimbalYaw=\"%.2f\"",
                (double)wrap_360(yaw), (double)wrap_360(yaw));
        }
    }
    if (metadata->zoom > 0.0f) {
        XMP(" apcg:Zoom=\"%.1f\"", (double)metadata->zoom);
    }
    XMP("/></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>");
#undef XMP
    return length;
}

static size_t app1(uint8_t *output, size_t capacity, const void *body,
                   size_t body_length)
{
    if (body_length + 2U > 0xffffU || capacity < body_length + 4U) return 0;
    output[0] = 0xff;
    output[1] = 0xe1;
    output[2] = (uint8_t)((body_length + 2U) >> 8);
    output[3] = (uint8_t)(body_length + 2U);
    memcpy(output + 4, body, body_length);
    return body_length + 4U;
}

size_t ca_metadata_jpeg_segments(const struct ca_metadata *metadata,
                                 const struct timespec *captured_at,
                                 uint8_t *output, size_t capacity)
{
    uint8_t exif[1024];
    char xmp[2048];
    struct tm local, utc;
    size_t exif_length, xmp_length, length, added;

    if (metadata == NULL || captured_at == NULL || output == NULL ||
        localtime_r(&captured_at->tv_sec, &local) == NULL ||
        gmtime_r(&captured_at->tv_sec, &utc) == NULL) {
        return 0;
    }
    exif_length = build_exif(metadata, captured_at, &local, &utc, exif,
                             sizeof(exif));
    xmp_length = build_xmp(metadata, captured_at, &local, xmp, sizeof(xmp));
    if (exif_length == 0U || xmp_length == 0U) return 0;
    length = app1(output, capacity, exif, exif_length);
    if (length == 0U) return 0;
    added = app1(output + length, capacity - length, xmp, xmp_length);
    return added == 0U ? 0U : length + added;
}
