#include "camera_app/mavlink.h"

#include <stdbool.h>
#include <string.h>

/* Single definition of the generated helper functions (parser, CRC table
 * lookup, finalise and serialise); the other objects only see prototypes. */
CA_MAVLINK_WARNINGS_PUSH
#include "mavlink_helpers.h"
CA_MAVLINK_WARNINGS_POP

void ca_mavlink_parser_init(struct ca_mavlink_parser *parser)
{
    if (parser != NULL) memset(parser, 0, sizeof(*parser));
}

int ca_mavlink_parse_byte(struct ca_mavlink_parser *parser, uint8_t byte,
                          mavlink_message_t *message)
{
    mavlink_status_t status;
    uint8_t result;
    bool in_signature;
    if (parser == NULL || message == NULL) return -1;
    in_signature =
        parser->status.parse_state == MAVLINK_PARSE_STATE_SIGNATURE_WAIT ||
        parser->status.parse_state ==
            MAVLINK_PARSE_STATE_SIGNATURE_WAIT_BAD_CRC;
    result = mavlink_frame_char_buffer(&parser->message, &parser->status,
                                       byte, message, &status);
    if (result == MAVLINK_FRAMING_OK) return 1;
    if (result == MAVLINK_FRAMING_INCOMPLETE) return 0;
    /* Bad CRC, unknown id or bad signature: the frame is dropped. A signed
     * frame reports its bad CRC before the signature block, which the helper
     * still has to consume, so only resync (as mavlink_parse_char() does)
     * once the helper is idle again, and never reuse a signature byte as a
     * frame start. */
    if (parser->status.parse_state ==
        MAVLINK_PARSE_STATE_SIGNATURE_WAIT_BAD_CRC) {
        return -1;
    }
    parser->status.msg_received = MAVLINK_FRAMING_INCOMPLETE;
    parser->status.parse_state = MAVLINK_PARSE_STATE_IDLE;
    if (byte == MAVLINK_STX && !in_signature) {
        parser->status.parse_state = MAVLINK_PARSE_STATE_GOT_STX;
        parser->message.len = 0U;
        mavlink_start_checksum(&parser->message);
    }
    return -1;
}

size_t ca_mavlink_to_wire(uint8_t *packet, size_t capacity,
                          const mavlink_message_t *message)
{
    size_t length;
    if (packet == NULL || message == NULL) return 0U;
    if (message->magic == MAVLINK_STX_MAVLINK1) {
        length = (size_t)MAVLINK_CORE_HEADER_MAVLINK1_LEN + 1U +
                 MAVLINK_NUM_CHECKSUM_BYTES + message->len;
    } else {
        length = (size_t)MAVLINK_NUM_NON_PAYLOAD_BYTES + message->len;
        if ((message->incompat_flags & MAVLINK_IFLAG_SIGNED) != 0U) {
            length += MAVLINK_SIGNATURE_BLOCK_LEN;
        }
    }
    if (length > capacity) return 0U;
    return mavlink_msg_to_send_buffer(packet, message);
}

void ca_mavlink_restamp(mavlink_message_t *message, mavlink_status_t *status)
{
    const mavlink_msg_entry_t *entry = mavlink_get_msg_entry(message->msgid);
    (void)mavlink_finalize_message_buffer(message, message->sysid,
                                          message->compid, status,
                                          message->len, message->len,
                                          entry != NULL ? entry->crc_extra
                                                        : 0U);
}

void ca_mavlink_signing_key(const char *passphrase, uint8_t key[32])
{
    mavlink_sha256_ctx ctx;
    uint8_t short_hash[6];
    mavlink_sha256_init(&ctx);
    mavlink_sha256_update(&ctx, passphrase, strlen(passphrase));
    mavlink_sha256_final_48(&ctx, short_hash);
    /* The helper finalizes the full SHA-256 state but normally exports only
     * the six signature bytes. SupportProxy passphrases use all 32 bytes. */
    for (unsigned i = 0; i < 32U; i++)
        key[i] = ctx.counter[i / 4U] >> (24U - 8U * (i % 4U));
}
