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

static float feedback=1,commanded=1;
static unsigned stops=0;
static enum ca_media_lens selected=CA_MEDIA_LENS_ZOOM;
float ca_media_lens_zoom(const ca_media*,enum ca_media_lens) { return feedback; }
float ca_media_lens_zoom_command(const ca_media*,enum ca_media_lens) { return commanded; }
enum ca_media_lens ca_media_lens(const ca_media*) { return selected; }
bool ca_media_thermal_main(const ca_media*) { return false; }
int ca_media_set_lens_zoom(ca_media*,enum ca_media_lens,float value) { commanded=value;return 0; }
void ca_media_cancel_zoom(ca_media*) { ++stops;commanded=feedback; }
int ca_backend_set_zoom_rate(ca_backend*,float) { assert(false);return -1; }

int main() {
    ca_mavlink_server server{};
    server.system_id=42;server.camera_component_id=100;server.gimbal_component_id=154;
    server.settings.main_resolution=CA_VIDEO_720P;
    const route destination{.kind=ROUTE_PROXY};
    // Measured feedback may lag each accepted command. Repeated MAVLink rate
    // messages must preserve fractional progress until stop or another owner.
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED);
    auto now=server.zoom_updated_ms;
    update_zoom(&server,now+200);assert(server.zoom_rate==1 && fabsf(commanded-1.4f)<.001f);
    feedback=1.1f;
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED && server.zoom_updated_ms==now+200);
    update_zoom(&server,now+400);assert(server.zoom_rate==1 && fabsf(commanded-1.8f)<.001f);
    commanded=3;update_zoom(&server,now+500);assert(server.zoom_rate==0 && !stops);
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED);
    assert(set_zoom(&server,1,0)==MAV_RESULT_ACCEPTED && server.zoom_rate==0 && stops==1);
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED);
    feedback=NAN;update_zoom(&server,server.zoom_updated_ms+100);
    assert(server.zoom_rate==0 && stops==2);
    // Reverse directly while the integrated target is ahead of measured lens
    // feedback. The next target must move away from that measured position,
    // rather than waiting for the old target to catch up with the lens.
    feedback=commanded=5;
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED);
    update_zoom(&server,server.zoom_updated_ms+2000);
    assert(commanded==9);
    feedback=5.2f;
    assert(set_zoom(&server,1,-1)==MAV_RESULT_ACCEPTED);
    assert(server.zoom_target==feedback && stops==3);
    now=server.zoom_updated_ms;
    update_zoom(&server,now+100);
    assert(fabsf(commanded-5.0f)<.001f);
    // Repeated messages in the same direction preserve accumulated progress.
    feedback=5.1f;
    assert(set_zoom(&server,1,-.5f)==MAV_RESULT_ACCEPTED);
    assert(server.zoom_updated_ms==now+100 && stops==3);
    update_zoom(&server,now+200);
    assert(fabsf(commanded-4.9f)<.001f);
    // The negative-to-positive reversal must rebase in the same way.
    assert(set_zoom(&server,1,1)==MAV_RESULT_ACCEPTED);
    assert(server.zoom_target==feedback && stops==4);
    update_zoom(&server,server.zoom_updated_ms+100);
    assert(fabsf(commanded-5.3f)<.001f);
    assert(set_zoom(&server,1,0)==MAV_RESULT_ACCEPTED && stops==5);
    feedback=1;
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
            assert(info.flags & CAMERA_CAP_FLAGS_HAS_BASIC_ZOOM);
            if(!initial_flags)initial_flags=info.flags;
            assert(info.flags==initial_flags);
            assert(info.resolution_h==(scope==CA_PHOTO_SCOPE_THERMAL?640:3840));
            assert(info.resolution_v==(scope==CA_PHOTO_SCOPE_THERMAL?512:2160));
        }
    }
}
