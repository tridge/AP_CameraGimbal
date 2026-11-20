#include "camera_app/binlog.h"
#include "camera_app/config.h"
#include "camera_app/siyi.h"
#include "camera_app/private_uart.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
void ca_log(const char *, ...) {}
int main(int argc, char **argv)
{
    assert(argc == 2);
    ca_config config;
    ca_config_defaults(&config);
    assert(ca_binlog_init(argv[1]) == 0);
    uint8_t payload[4096], frame[CA_PRIVATE_MAX_FRAME];
    for (unsigned i=0; i<sizeof(payload); i++) payload[i]=i;
    size_t n=ca_siyi_build(frame,sizeof(frame),1,0x1234,0x7f,payload,2038);
    assert(n == CA_SIYI_MAX_PACKET);
    ca_binlog_packet(false, CA_PACKET_SIYI, CA_PACKET_TCP, 0x7f000001, 4000, frame, n);
    assert(ca_binlog_start(&config));
    errno=EBUSY;
    ca_binlog_packet(false, CA_PACKET_SIYI, CA_PACKET_TCP, 0x7f000001, 4000, frame, n);
    assert(errno == EBUSY);
    ca_binlog_packet(true, CA_PACKET_SIYI, CA_PACKET_TCP, 0x7f000001, 4000, frame, n, -EPIPE);
    assert(errno == EBUSY);
    n=ca_private_build(frame,sizeof(frame),8,65535,0x2e,0x34,0x6b,0xee,payload,sizeof(payload));
    assert(n == CA_PRIVATE_MAX_FRAME);
    ca_binlog_packet(false, CA_PACKET_MT11, CA_PACKET_MCU_UART, 0, 0, frame, n);
    const uint8_t old_query[]={0x55,0x66,0xaa,0xbb,1,0,0,0,0,0,0,0x80,
                              0x2d,0x97,0x7a,0x34,0xb7,0xad,0x40,0xeb};
    ca_binlog_packet(false, CA_PACKET_SIYI_LONG, CA_PACKET_TCP, 0x7f000001, 4000,
                     old_query, sizeof(old_query));
    ca_binlog_stop();
    ca_binlog_packet(false, CA_PACKET_MT11, CA_PACKET_MCU_UART, 0, 0, frame, n);
    assert(ca_binlog_start(&config));
    const uint8_t xf[] = {0xb5,0x9a,1,2};
    ca_binlog_packet(false, CA_PACKET_XFROBOT_MCU, CA_PACKET_MCU_UART, 0, 0, xf, sizeof(xf));
    ca_binlog_close();
}
