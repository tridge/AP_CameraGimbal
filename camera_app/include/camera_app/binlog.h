#ifndef CAMERA_APP_BINLOG_H
#define CAMERA_APP_BINLOG_H
#include <stdbool.h>
#include "camera_app/exposure.h"
#include <stddef.h>
#include <stdint.h>
struct ca_config;
struct ca_gimbal_attitude;
/* DataFlash records are packed little-endian, as on all supported targets. */
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error BIN logging requires little endian
#endif
enum ca_binlog_id { CA_LOG_PARM=129, CA_LOG_MSG, CA_LOG_POS, CA_LOG_ATT,
    CA_LOG_GIMB, CA_LOG_PIDP, CA_LOG_PIDY, CA_LOG_MODE, CA_LOG_CMD,
    CA_LOG_CAM, CA_LOG_VID, CA_LOG_GCMD, CA_LOG_STAT, CA_LOG_TIME, CA_LOG_ROI,
    CA_LOG_PRMA, CA_LOG_VEND, CA_LOG_AE, CA_LOG_SYS,
    CA_LOG_MAVC, CA_LOG_GMBC, CA_LOG_MAVP, CA_LOG_MAVH,
    CA_LOG_SIIN, CA_LOG_SIOU, CA_LOG_XFIN, CA_LOG_XFOU,
    CA_LOG_TRKF, CA_LOG_TFRA };
enum ca_packet_protocol { CA_PACKET_SIYI=1, CA_PACKET_MT11, CA_PACKET_SIYI_MCU,
                          CA_PACKET_XFROBOT, CA_PACKET_XFROBOT_MCU, CA_PACKET_SIYI_LONG,
                          CA_PACKET_CADDX };
enum ca_packet_link { CA_PACKET_UDP=1, CA_PACKET_TCP, CA_PACKET_UART,
                      CA_PACKET_MCU_UART, CA_PACKET_MCU_UDP, CA_PACKET_MCU_TCP };
struct __attribute__((packed)) ca_log_packet {
    uint64_t time_us;
    uint32_t packet_id;
    uint8_t link, protocol;
    uint32_t ip;
    uint16_t port;
    uint8_t source, destination;
    uint16_t command, sequence, length, offset;
    int32_t result;
    uint8_t data[192];
};
struct __attribute__((packed)) ca_log_vendor { uint64_t time_us; uint8_t opcode; uint16_t length; char payload[64]; };
struct __attribute__((packed)) ca_log_parm { uint64_t time_us; char name[16]; float value; };
struct __attribute__((packed)) ca_log_msg { uint64_t time_us; char text[64]; };
struct __attribute__((packed)) ca_log_pos { uint64_t time_us; uint32_t boot_ms; int32_t lat, lon; float alt, relalt, vn, ve, vd; uint8_t source_system, source_component; };
struct __attribute__((packed)) ca_log_att { uint64_t time_us; uint32_t boot_ms; uint8_t source; float roll,pitch,yaw,rollrate,pitchrate,yawrate; uint8_t source_system, source_component; };
struct __attribute__((packed)) ca_log_gimb { uint64_t time_us, sample_us; float roll,pitch,yaw,rollrate,pitchrate,yawrate; };
struct __attribute__((packed)) ca_log_trkf {
    uint64_t time_us, frame_ms;
    uint8_t state;
    float quality, pitch_error, yaw_error, pitch_rate, yaw_rate, roll, pitch, yaw, hfov;
};
struct __attribute__((packed)) ca_log_tfra { uint64_t time_us, pts_us, sdk_us; };
struct __attribute__((packed)) ca_log_pid { uint64_t time_us; float target,actual,rate,ff,error,p,i,d,output,dt,age; };
struct __attribute__((packed)) ca_log_mode { uint64_t time_us; uint32_t flight_mode; uint8_t armed,mode,method,yawlock,recording,system; };
struct __attribute__((packed)) ca_log_cmd { uint64_t time_us; uint16_t command; uint8_t system,component,result; float p1,p2,p3,p4,p5,p6,p7; };
// Double X/Y preserve both COMMAND_INT integers and COMMAND_LONG float values.
// Result 255 means ignored/no ACK; frame 255 means COMMAND_LONG has no frame.
struct __attribute__((packed)) ca_log_mavc {
    uint64_t time_us;
    uint8_t target_system, target_component, source_system, source_component, frame;
    uint16_t command;
    float p1, p2, p3, p4;
    double x, y;
    float z;
    uint8_t result, was_long;
};
struct __attribute__((packed)) ca_log_gmbc {
    uint64_t time_us;
    uint8_t target_system, target_component, source_system, source_component;
    uint16_t flags;
    float q[4], rates[3];
    uint8_t result;
};
struct __attribute__((packed)) ca_log_mavp {
    uint64_t time_us;
    uint8_t target_system, target_component, source_system, source_component, type, extended;
    char name[16];
    float value;
    uint8_t result;
    // Two DataFlash 'a' fields preserve all 128 PARAM_EXT_SET value bytes.
    uint8_t raw[128];
};
struct __attribute__((packed)) ca_log_mavh {
    uint64_t time_us;
    uint8_t source_system, source_component;
    uint32_t custom_mode;
    uint8_t type, autopilot, base_mode, system_status, version;
};
struct __attribute__((packed)) ca_log_cam { uint64_t time_us; uint8_t scope; int32_t result,lat,lon; float alt,roll,pitch,yaw; };
struct __attribute__((packed)) ca_log_vid { uint64_t time_us; uint8_t active; int32_t result; char path[64]; };
struct __attribute__((packed)) ca_log_gcmd { uint64_t time_us; uint8_t mode; float pitch,yaw,wirep,wirey; int32_t result; };
struct __attribute__((packed)) ca_log_stat { uint64_t time_us; uint32_t dropped,queued,errors; };
/* SYS validity bits: temperature, CPU busy, free RAM, available RAM, SD space.
 * Memory/storage are bytes, temperature Celsius and CPU busy 0..100 percent. */
enum ca_sys_valid { CA_SYS_TEMP=1, CA_SYS_CPU=2, CA_SYS_MEM_FREE=4,
    CA_SYS_MEM_AVAILABLE=8, CA_SYS_SD_FREE=16 };
struct __attribute__((packed)) ca_log_sys {
    uint64_t time_us;
    float cpu_temp, cpu_load;
    uint64_t mem_free, mem_available, sd_free;
    uint8_t valid;
};
struct __attribute__((packed)) ca_log_time { uint64_t time_us, utc_us; };
struct __attribute__((packed)) ca_log_roi { uint64_t time_us; int32_t lat,lon; float alt; uint8_t active; };
#define CA_BINLOG(id, type, ...) do { struct type r_ = { .time_us=ca_binlog_time_us(), __VA_ARGS__ }; ca_binlog_emit(id, &r_, sizeof(r_)); } while (0)
int ca_binlog_init(const char *root);
void ca_binlog_close(void);
bool ca_binlog_active(void);
/* Returns true on a new logging session. Startup parameters precede samples. */
bool ca_binlog_start(const struct ca_config *config);
void ca_binlog_stop(void);
void ca_binlog_emit(uint8_t id, const void *data, size_t size);
uint64_t ca_binlog_time_us(void);
void ca_binlog_message(const char *text);
void ca_binlog_parameter(const char *name, float value, bool applied);
void ca_binlog_feedback(const struct ca_gimbal_attitude *attitude);
void ca_binlog_stats(void);
void ca_binlog_vendor(uint8_t opcode, const uint8_t *payload, uint16_t length);
// Whole validated RX frames or TX attempts/queue submissions; result is 0 or
// negative errno. IP/port use host byte order and identify the remote endpoint.
// Chunk groups are enqueued atomically, preserving errno and complete packets.
void ca_binlog_packet(bool outgoing, uint8_t protocol, uint8_t link,
                     uint32_t ip, uint16_t port, const uint8_t *data, size_t length, int32_t result=0);
#endif
