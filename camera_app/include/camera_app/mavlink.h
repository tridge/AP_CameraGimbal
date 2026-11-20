#ifndef CAMERA_APP_MAVLINK_H
#define CAMERA_APP_MAVLINK_H

#include <stddef.h>
#include <stdint.h>

/* pymavlink-generated bindings for the dialect selected by the Makefile
 * (MAVLINK_DIALECT). The helper bodies are compiled once in mavlink.cpp, and
 * every link carries its own parser state, so the per-channel globals are
 * never used and are kept to a single entry. */
#define MAVLINK_SEPARATE_HELPERS
#define MAVLINK_COMM_NUM_BUFFERS 1
#ifndef CA_MAVLINK_DIALECT
#define CA_MAVLINK_DIALECT all
#endif
#define CA_MAVLINK_STRINGIFY_(x) #x
#define CA_MAVLINK_DIALECT_HEADER(dialect) CA_MAVLINK_STRINGIFY_(dialect/mavlink.h)

/* The generated encode helpers take the address of packed struct members.
 * -Wpragmas covers compilers that predate that warning. */
#define CA_MAVLINK_WARNINGS_PUSH \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wpragmas\"") \
    _Pragma("GCC diagnostic ignored \"-Waddress-of-packed-member\"")
#define CA_MAVLINK_WARNINGS_POP _Pragma("GCC diagnostic pop")

CA_MAVLINK_WARNINGS_PUSH
#include CA_MAVLINK_DIALECT_HEADER(CA_MAVLINK_DIALECT)
CA_MAVLINK_WARNINGS_POP

/* Per-link receive state: the frame being assembled and its parser status. */
struct ca_mavlink_parser {
    mavlink_message_t message;
    mavlink_status_t status;
};

void ca_mavlink_signing_key(const char *passphrase, uint8_t key[32]);

void ca_mavlink_parser_init(struct ca_mavlink_parser *parser);

/* Feed one received byte. Returns 1 with *message filled when a frame with a
 * valid CRC completes, 0 while a frame is incomplete and -1 when a frame was
 * dropped (bad CRC, unknown message id or bad signature). MAVLink 1 and 2
 * frames are both accepted; MAVLink 2 payloads are zero-filled to the full
 * message length. */
int ca_mavlink_parse_byte(struct ca_mavlink_parser *parser, uint8_t byte,
                          mavlink_message_t *message);

/* Write the wire form of a finalised message. Returns the byte count, or 0
 * when the packet does not fit. */
size_t ca_mavlink_to_wire(uint8_t *packet, size_t capacity,
                          const mavlink_message_t *message);

/* Give an already packed message the next sequence number of its component
 * and refresh the CRC, so the same message can be sent on several links with
 * its own sequence number on each. */
void ca_mavlink_restamp(mavlink_message_t *message, mavlink_status_t *status);

#endif
