#include "camera_app/camera_ftp.h"
#include "camera_app/camera_definition.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

void ca_camera_ftp_reply(struct ca_camera_ftp *ftp, const char *xml, size_t length,
                         uint8_t system, uint8_t component, uint64_t now_ms,
                         const uint8_t request[251], uint8_t response[251])
{
    uint16_t seq = (uint16_t)((unsigned)request[0] | ((unsigned)request[1] << 8));
    seq++;
    memset(response, 0, 251);
    response[0] = (uint8_t)seq;
    response[1] = (uint8_t)(seq >> 8);
    response[2] = request[2];
    response[3] = 128; /* ACK */
    response[5] = request[3];
    memcpy(response + 8, request + 8, 4);
    uint8_t error = 0;
    uint8_t opcode = request[3], size = request[4];
    for (unsigned i = 0; i < 4; i++) {
        if (now_ms - ftp->sessions[i].last_ms > 60000) ftp->sessions[i].active = false;
    }
    struct ca_camera_ftp_session *session = NULL;
    for (unsigned i = 0; i < 4; i++) {
        struct ca_camera_ftp_session *candidate = &ftp->sessions[i];
        if (candidate->active && candidate->system == system && candidate->component == component &&
            candidate->id == request[2]) session = candidate;
    }
    if (size > 239) {
        error = 3; /* InvalidDataSize */
    } else if (opcode == 2) { /* ResetSessions, only for the requesting client */
        for (unsigned i = 0; i < 4; i++) {
            if (ftp->sessions[i].system == system && ftp->sessions[i].component == component)
                ftp->sessions[i].active = false;
        }
    } else if (opcode == 3 || opcode == 16) { /* ListDirectory / with mtime */
        char path[240] = {0};
        memcpy(path, request + 12, size);
        /* This is a virtual export directory, independent of the camera's
         * filesystem. Offsets count directory entries, not bytes. */
        if (path[0] && strcmp(path, "/") && strcmp(path, ".") && strcmp(path, "./")) {
            error = 10; /* FileNotFound */
        } else if (get32(request + 8) != 0) {
            error = 6; /* EOF: the root currently exports one file */
        } else {
            int count = snprintf((char *)response + 12, 239, "F%s\t%zu%s",
                CA_CAMERA_DEFINITION_PATH + 1, length, opcode == 16 ? "\t0" : "");
            /* The generated definition has no filesystem modification time. */
            if (count < 0 || count >= 239) error = 3;
            else response[4] = (uint8_t)(count + 1); /* include entry terminator */
        }
    } else if (opcode == 4) { /* OpenFileRO */
        char path[240] = {0};
        memcpy(path, request + 12, size);
        /* QGC can retain an extra slash when parsing the MAVFTP URI. */
        const char *name = path;
        while (*name == '/') name++;
        if (strcmp(name, CA_CAMERA_DEFINITION_PATH + 1)) {
            error = 10; /* FileNotFound */
        } else {
            /* Reopening this immutable file reuses a client's session. That
             * makes a retry of a lost Open ACK harmless without leaking slots. */
            session = NULL;
            for (unsigned i = 0; i < 4; i++) {
                struct ca_camera_ftp_session *candidate = &ftp->sessions[i];
                if (candidate->active && candidate->system == system && candidate->component == component &&
                    candidate->id == request[2]) {
                    session = candidate;
                    break;
                }
                if (!candidate->active && !session) session = candidate;
            }
            if (!session) error = 5; /* NoSessionsAvailable */
            else {
                *session = (struct ca_camera_ftp_session){system, component, request[2], true, now_ms};
                response[4] = 4;
                put32(response + 12, (uint32_t)length);
            }
        }
    } else if (opcode == 1) { /* TerminateSession */
        if (session) session->active = false;
        else error = 4;
    } else if (opcode == 5 || opcode == 15) { /* ReadFile / BurstReadFile */
        uint32_t offset = get32(request + 8);
        if (!session) error = 4;
        else if (!size) error = 3;
        else if (offset >= length) error = 6; /* EOF */
        else {
            size_t count = length - offset;
            if (count > size) count = size;
            response[4] = (uint8_t)count;
            /* A bounded one-packet burst avoids starving control traffic on
             * slow UARTs; the GCS requests the next burst at the next offset. */
            response[6] = opcode == 15 ? 1 : 0;
            memcpy(response + 12, xml + offset, count);
            session->last_ms = now_ms;
        }
    } else {
        error = 7; /* UnknownCommand: no writes or host filesystem access */
    }
    if (error) {
        response[3] = 129;
        response[4] = 1;
        response[12] = error;
    }
}
