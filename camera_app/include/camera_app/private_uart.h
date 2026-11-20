#ifndef CAMERA_APP_PRIVATE_UART_H
#define CAMERA_APP_PRIVATE_UART_H

#include <stddef.h>
#include <stdint.h>

#define CA_PRIVATE_MAX_PAYLOAD 4096U
#define CA_PRIVATE_MAX_FRAME (CA_PRIVATE_MAX_PAYLOAD + 14U)
#define CA_LONG_MAX_FRAME (CA_PRIVATE_MAX_PAYLOAD + 20U)

struct ca_private_frame {
    uint8_t control;
    uint16_t sequence;
    uint8_t source;
    uint8_t destination;
    uint8_t link;
    uint8_t command;
    const uint8_t *payload;
    uint16_t payload_length;
    const uint8_t *raw;
    size_t raw_length;
};

typedef void (*ca_private_frame_fn)(void *opaque,
                                    const struct ca_private_frame *frame);

struct ca_private_parser {
    uint8_t data[CA_LONG_MAX_FRAME * 2U];
    size_t length;
    unsigned discarded;
    unsigned bad_header_crc;
    unsigned bad_frame_crc;
};

uint8_t ca_crc8_maxim(const uint8_t *data, size_t length);
size_t ca_private_build(uint8_t *output, size_t capacity, uint8_t control,
                        uint16_t sequence, uint8_t source, uint8_t destination,
                        uint8_t link, uint8_t command, const uint8_t *payload,
                        uint16_t payload_length);
void ca_private_parser_init(struct ca_private_parser *parser);
void ca_private_parser_feed(struct ca_private_parser *parser,
                            const uint8_t *data, size_t length,
                            ca_private_frame_fn callback, void *opaque);

/* UniGCS also uses the older 55 66 AA BB network framing for A8.
 * This is never accepted on the private MCU UART parser above. */
size_t ca_long_build(uint8_t *output, size_t capacity, uint8_t control,
                    uint16_t sequence, uint8_t command,
                    const uint8_t *payload, uint16_t payload_length);
void ca_private_network_parser_feed(struct ca_private_parser *parser,
                                    const uint8_t *data, size_t length,
                                    ca_private_frame_fn callback, void *opaque);

#endif
