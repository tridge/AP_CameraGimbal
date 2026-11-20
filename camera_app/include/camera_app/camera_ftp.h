#ifndef CAMERA_APP_CAMERA_FTP_H
#define CAMERA_APP_CAMERA_FTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only MAVFTP service. The root exports the immutable camera definition
 * plus the record, capture and log roots of the card as /record, /capture and
 * /logs. Nothing outside those roots is opened and clients have independent
 * sessions. */
#define CA_CAMERA_FTP_ROOTS 3
#define CA_CAMERA_FTP_ROOT_MAX 256
#ifdef __cplusplus
extern "C" {
#endif

struct ca_camera_ftp_session {
    uint8_t system, component, id;
    bool active;
    uint64_t last_ms;
    int fd; /* open card file, -1 for the camera definition */
    uint64_t size;
    /* burst read continuing after the reply packet */
    uint32_t burst_offset;
    uint32_t burst_remaining;
    uint16_t burst_seq;
    uint8_t burst_size;
};
struct ca_camera_ftp {
    struct ca_camera_ftp_session sessions[4];
    char roots[CA_CAMERA_FTP_ROOTS][CA_CAMERA_FTP_ROOT_MAX];
    /* packets per burst read including the reply; 0 behaves as 1 */
    unsigned burst_packets;
    const char *xml;
    size_t xml_length;
};
/* Empty or oversized roots are not exported. */
void ca_camera_ftp_init(struct ca_camera_ftp *ftp, const char *record_root,
                        const char *capture_root, const char *log_root);
void ca_camera_ftp_reply(struct ca_camera_ftp *ftp, const char *xml, size_t length,
                         uint8_t system, uint8_t component, uint64_t now_ms,
                         const uint8_t request[251], uint8_t response[251]);
/* Next packet of the burst started by the last reply to this client, or
 * false once it is complete. final ends the burst with this packet, for a
 * link that has no room for more. */
bool ca_camera_ftp_burst_next(struct ca_camera_ftp *ftp, uint8_t system,
                              uint8_t component, uint8_t response[251], bool final);
void ca_camera_ftp_close(struct ca_camera_ftp *ftp);
#ifdef __cplusplus
}
#endif

#endif
