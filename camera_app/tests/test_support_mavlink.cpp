#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/support_proxy.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static bool receive(struct ca_support_mavlink *proxy, mavlink_message_t *message)
{
    for (unsigned i = 0; i < 30; i++) {
        if (ca_support_mavlink_receive(proxy, message)) return true;
        usleep(10000);
    }
    return false;
}

int main(void)
{
    struct ca_config config;
    ca_config_defaults(&config);
    struct ca_support_mavlink *proxy = NULL;
    assert(ca_support_mavlink_open(&proxy, &config.support) == 0 && proxy == NULL);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t address_length = sizeof(address);
    assert(getsockname(fd, (struct sockaddr *)&address, &address_length) == 0);
    struct timeval timeout = {.tv_sec = 3};
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    config.support.enabled = true;
    config.support.signing = true;
    config.support.mavlink_port = ntohs(address.sin_port);
    strcpy(config.support.host, "127.0.0.1");
    strcpy(config.support.signing_passphrase, "test-signing-passphrase");
    assert(ca_support_mavlink_open(&proxy, &config.support) == 0);
    mavlink_status_t local = {.current_tx_seq = 17};
    mavlink_message_t message;
    mavlink_msg_heartbeat_pack_status(42, 1, &local, &message,
        MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_ACTIVE);
    ca_support_mavlink_send(proxy, &message);
    uint8_t packet[MAVLINK_MAX_PACKET_LEN];
    ssize_t count = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&address, &address_length);
    assert(count > 0);
    mavlink_signing_t signing = {.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING, .link_id = 7};
    ca_mavlink_signing_key(config.support.signing_passphrase, signing.secret_key);
    signing.timestamp = (uint64_t)(time(NULL) - 1420070400) * 100000U;
    mavlink_signing_streams_t streams = {};
    struct ca_mavlink_parser parser;
    ca_mavlink_parser_init(&parser);
    parser.status.signing = &signing;
    parser.status.signing_streams = &streams;
    int parsed = 0;
    for (ssize_t i = 0; i < count; i++) parsed = ca_mavlink_parse_byte(&parser, packet[i], &message);
    assert(parsed == 1 && message.sysid == 42 && message.compid == 1 && message.seq == 17);
    assert(message.incompat_flags & MAVLINK_IFLAG_SIGNED);

    mavlink_status_t remote = {.current_tx_seq = 99, .signing = &signing};
    mavlink_msg_heartbeat_pack_status(255, 190, &remote, &message,
        MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    size_t size = ca_mavlink_to_wire(packet, sizeof(packet), &message);
    packet[size - 1] ^= 1; // right CRC, forged signature
    assert(sendto(fd, packet, size, 0, (struct sockaddr *)&address, address_length) == (ssize_t)size);
    assert(!receive(proxy, &message));
    packet[size - 1] ^= 1;
    assert(sendto(fd, packet, size, 0, (struct sockaddr *)&address, address_length) == (ssize_t)size);
    assert(receive(proxy, &message));
    assert(message.sysid == 255 && message.compid == 190 && message.seq == 99);
    assert(!(message.incompat_flags & MAVLINK_IFLAG_SIGNED));
    // The exact same valid signed packet must not be accepted twice.
    assert(sendto(fd, packet, size, 0, (struct sockaddr *)&address, address_length) == (ssize_t)size);
    assert(!receive(proxy, &message));
    remote.signing = NULL;
    mavlink_msg_heartbeat_pack_status(255, 190, &remote, &message,
        MAV_TYPE_GCS, MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    size = ca_mavlink_to_wire(packet, sizeof(packet), &message);
    assert(sendto(fd, packet, size, 0, (struct sockaddr *)&address, address_length) == (ssize_t)size);
    assert(!receive(proxy, &message));
    ca_support_mavlink_close(proxy);
    close(fd);
    puts("PASS SupportProxy signing, identity/sequence preservation, unsigned/forged/replay rejection");
    return 0;
}
