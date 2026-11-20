#include "../camera_app/tests/test_media_backend.h"
#include "camera_app/binlog.h"
#include <cassert>
#include <cerrno>
#include <cstring>
static unsigned creates=0,destroys=0;
class Backend : public TestMediaBackend {
public:
    ~Backend() override { destroys++; }
};
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config&) {
    creates++;return std::unique_ptr<APC_Media_Backend>(new Backend);
}
void ca_log(const char*,...) {}
bool ca_binlog_active() { return false; }
uint64_t ca_binlog_time_us() { return 0; }
void ca_binlog_emit(uint8_t,const void*,size_t) {}
int ca_backend_set_gimbal_rates(ca_backend*,float,float) { return -1; }
int main() {
    ca_media_config config{};config.settings.main_resolution=CA_VIDEO_720P;
    ca_media *media=nullptr;assert(ca_media_open(&media,&config)==0);
    auto next=config.settings;next.main_resolution=CA_VIDEO_1080P;
    assert(ca_media_configure(media,&next)==-1 && errno==ENOTSUP);
    assert(creates==1 && destroys==0 && ca_media_ready(media));
    assert(ca_media_settings(media)->main_resolution==CA_VIDEO_720P);
    next=config.settings;next.network.primary_address[0]='x';next.uart_protocol=CA_UART_MAVLINK;
    assert(ca_media_configure(media,&next)==-1 && errno==ENOTSUP);
    assert(creates==1 && destroys==0 && ca_media_ready(media));
    assert(ca_media_settings(media)->uart_protocol==CA_UART_NONE);
    ca_media_close(media);assert(destroys==1);
}
