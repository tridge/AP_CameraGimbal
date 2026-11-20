#include "../camera_app/src/protocol/unigcs.cpp"
#include <cassert>
#include <cstdio>
static ca_config settings{};
static bool thermal_primary;
const ca_config *ca_media_settings(const ca_media *) { return &settings; }
bool ca_media_thermal_main(const ca_media *) { return thermal_primary; }
unsigned ca_media_frame_rate(const ca_media *,bool thermal) { return thermal?15:30; }
void ca_video_resolution_size(ca_video_resolution,unsigned *w,unsigned *h) { *w=1280;*h=720; }
int main() {
    ca_unigcs server{};uint8_t out[9]{};
    for(bool thermal : {false,true}) {
        thermal_primary=thermal;
        for(unsigned id=0;id<3;id++) {
            encoder(&server,id,out);
            const bool thermal_encoder=id && ((id==1)==thermal);
            const unsigned bitrate=unsigned(out[6])|(unsigned(out[7])<<8);
            assert(bitrate==(id==0?12000:thermal_encoder?1000:1570));
            assert(out[8]==(thermal_encoder?15:30));
        }
    }
    puts("PASS native UniGCS encoder rates for recording, RGB and thermal routes");
}
