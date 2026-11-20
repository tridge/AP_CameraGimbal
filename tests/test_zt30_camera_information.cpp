// Exercise the native camera's first reply before thermal frames arrive.
// Flight controllers cache this reply, so capability flags must be stable.
#include "../camera_app/src/protocol/mavlink_server.cpp"
#include <cassert>

static bool photo_ready=false;
static mavlink_message_t reply{};
bool ca_media_photo_available(const ca_media*) { return photo_ready; }
bool ca_media_survey_available(ca_media*,unsigned) { return false; }
bool ca_media_tracking_available(ca_media*) { return false; }
bool ca_backend_has_gimbal(const ca_backend*) { return false; }
void ca_support_mavlink_send(ca_support_mavlink*,const mavlink_message_t *message) { reply=*message; }
void ca_log(const char*,...) {}

int main() {
    ca_mavlink_server server{};
    server.system_id=42;server.camera_component_id=100;server.gimbal_component_id=154;
    server.settings.main_resolution=CA_VIDEO_720P;
    const route destination{.kind=ROUTE_PROXY};
    uint32_t initial_flags=0;
    for(bool ready : {false,true,false}) {
        photo_ready=ready;
        for(unsigned scope : {unsigned(CA_PHOTO_SCOPE_ALL),unsigned(CA_PHOTO_SCOPE_THERMAL)}) {
            server.photo_scope=ca_photo_scope(scope);
            send_camera_information(&server,&destination);
            assert(reply.msgid==MAVLINK_MSG_ID_CAMERA_INFORMATION);
            mavlink_camera_information_t info{};
            mavlink_msg_camera_information_decode(&reply,&info);
            assert(info.flags & CAMERA_CAP_FLAGS_CAPTURE_IMAGE);
            assert(info.flags & CAMERA_CAP_FLAGS_CAN_CAPTURE_IMAGE_IN_VIDEO_MODE);
            if(!initial_flags)initial_flags=info.flags;
            assert(info.flags==initial_flags);
            assert(info.resolution_h==(scope==CA_PHOTO_SCOPE_THERMAL?640:3840));
            assert(info.resolution_v==(scope==CA_PHOTO_SCOPE_THERMAL?512:2160));
        }
    }
}
