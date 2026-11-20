/* Deliberately inert native control for the bounded RAM media integration.
 * This file never opens the MCU UART or delegates to the SIYI transport. */
#include "camera_app/backend.h"
#include "camera_app/siyi.h"
#include <cerrno>
#include <cstring>
#include <new>
struct ca_backend { ca_siyi_emit_fn emit;void *opaque; };
static int unsupported() { errno=ENOTSUP;return -1; }
int ca_backend_open(ca_backend **out,const ca_backend_config *config) {
    if(!out || !config || !config->name || std::strcmp(config->name,"zt30")) { errno=EINVAL;return -1; }
    *out=new(std::nothrow) ca_backend{config->emit,config->emit_opaque};
    if(!*out) { errno=ENOMEM;return -1; }return 0;
}
int ca_backend_fd(const ca_backend*) { return -1; }
int ca_backend_handle_fd(ca_backend*) { return 0; }
int ca_backend_handle_siyi(ca_backend *b,const uint8_t *raw,size_t len) {
    ca_siyi_packet packet{};size_t consumed=0;
    if(!b || ca_siyi_parse_one(raw,len,&packet,&consumed)!=1 || consumed!=len) { errno=EPROTO;return -1; }
    // Software identity only. No fabricated gimbal/zoom/thermal measurements.
    if(packet.opcode!=1 || packet.payload_length)return unsupported();
    uint8_t version[12]{};version[3]=0x7b;
    uint8_t reply[CA_SIYI_MAX_PACKET];size_t bytes=ca_siyi_build(reply,sizeof(reply),2,packet.sequence,packet.opcode,version,sizeof(version));
    if(!bytes || !b->emit)return unsupported();
    b->emit(b->opaque,reply,bytes);return 0;
}
int ca_backend_handle_private(ca_backend*,const ca_private_frame*) { return unsupported(); }
int ca_backend_write_external_uart(ca_backend*,const uint8_t*,size_t) { return unsupported(); }
void ca_backend_periodic(ca_backend*) {}
int ca_backend_request_gimbal_attitude(ca_backend*) { return unsupported(); }
bool ca_backend_gimbal_attitude(const ca_backend*,ca_gimbal_attitude*) { return false; }
int ca_backend_set_gimbal_angles(ca_backend*,float,float) { return unsupported(); }
int ca_backend_set_gimbal_rates(ca_backend*,float,float) { return unsupported(); }
int ca_backend_set_zoom(ca_backend*,float) { return unsupported(); }
int ca_backend_set_zoom_rate(ca_backend*,float) { return unsupported(); }
int ca_backend_set_gimbal_neutral(ca_backend*) { return unsupported(); }
bool ca_backend_recording(const ca_backend*) { return false; }
const char *ca_backend_name(const ca_backend*) {
#ifdef CAMERA_APP_ZT30_DEV
    return "zt30";
#else
    return "zt30-ram";
#endif
}
void ca_backend_close(ca_backend *b) { delete b; }
bool ca_backend_has_gimbal(const ca_backend*) { return false; }
