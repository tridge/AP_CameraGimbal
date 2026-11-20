#include "camera_app/siyi.h"

#include <string.h>

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void put_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

uint16_t ca_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    size_t i;
    unsigned bit;

    for (i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) != 0U
                      ? (uint16_t)((crc << 1) ^ 0x1021U)
                      : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

int ca_siyi_parse_one(const uint8_t *data, size_t length,
                      struct ca_siyi_packet *packet, size_t *consumed)
{
    uint16_t payload_length;
    uint16_t received_crc;
    size_t total;

    if (consumed != NULL) *consumed = 0;
    if (data == NULL || packet == NULL || length < CA_SIYI_MIN_PACKET) return 0;
    if (data[0] != 0x55U || data[1] != 0x66U) return -1;
    payload_length = get_u16_le(data + 3);
    total = (size_t)payload_length + CA_SIYI_MIN_PACKET;
    if (total > CA_SIYI_MAX_PACKET) return -1;
    if (length < total) return 0;
    received_crc = get_u16_le(data + total - 2U);
    if (received_crc != ca_crc16(data, total - 2U)) return -1;

    packet->control = data[2];
    packet->payload_length = payload_length;
    packet->sequence = get_u16_le(data + 5);
    packet->opcode = data[7];
    packet->payload = data + 8;
    packet->raw = data;
    packet->raw_length = total;
    if (consumed != NULL) *consumed = total;
    return 1;
}

size_t ca_siyi_build(uint8_t *output, size_t capacity, uint8_t control,
                     uint16_t sequence, uint8_t opcode,
                     const uint8_t *payload, uint16_t payload_length)
{
    size_t total = (size_t)payload_length + CA_SIYI_MIN_PACKET;

    if (output == NULL || capacity < total || total > CA_SIYI_MAX_PACKET) return 0;
    output[0] = 0x55;
    output[1] = 0x66;
    output[2] = control;
    put_u16_le(output + 3, payload_length);
    put_u16_le(output + 5, sequence);
    output[7] = opcode;
    if (payload_length != 0U && payload != NULL) {
        memcpy(output + 8, payload, payload_length);
    }
    put_u16_le(output + total - 2U, ca_crc16(output, total - 2U));
    return total;
}

bool ca_siyi_rewrite_payload_byte(uint8_t *packet, size_t length,
                                  size_t payload_offset, uint8_t value)
{
    struct ca_siyi_packet parsed;
    size_t consumed;

    if (ca_siyi_parse_one(packet, length, &parsed, &consumed) != 1 ||
        consumed != length || payload_offset >= parsed.payload_length) {
        return false;
    }
    packet[8U + payload_offset] = value;
    put_u16_le(packet + length - 2U, ca_crc16(packet, length - 2U));
    return true;
}

bool ca_siyi_opcode_known(uint8_t opcode)
{
    switch (opcode) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
    case 0x0e:
    case 0x0f:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x30:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x37:
    case 0x38:
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x3c:
    case 0x3e:
    case 0x40:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4d:
    case 0x4e:
    case 0x4f:
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x5f:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x71:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0xfa:
    case 0xfb:
    case 0xfc:
    case 0xfd:
    case 0xfe:
        return true;
    default:
        return false;
    }
}
