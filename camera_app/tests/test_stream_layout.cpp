// Exercise the actual discovery helpers while the raw source stalls/recovers.
#include "../src/protocol/mavlink_server.cpp"
#include <cassert>
static bool thermal_available;
bool ca_thermal_stream_available() { return thermal_available; }
int main()
{
    ca_mavlink_server server{};
    route proxy{}; proxy.kind=ROUTE_PROXY;
    server.settings.video_telemetry_format=CA_VIDEO_TELEMETRY_MISB;
    constexpr unsigned streams=APCAM_NUM_STREAMS;
    constexpr unsigned base=streams+(APCAM_HAVE_RAW_THERMAL?1U:0U);
    for(bool available: {true,false,true}) {
        thermal_available=available;
        assert(stream_count(&server)==base+streams);
        assert(stream_count(&server,&proxy)==base);
        for(unsigned id=1;id<=streams;id++) {
            assert(stream_source(&server,id)==id);
            assert(stream_source(&server,base+id)==id);
            assert(stream_uses_misb(&server,id) && !stream_uses_misb(&server,base+id));
        }
        if(APCAM_HAVE_RAW_THERMAL) assert(stream_source(&server,CA_RAW_THERMAL_STREAM_ID)==0 && raw_thermal_stream(CA_RAW_THERMAL_STREAM_ID));
        assert(!stream_uses_misb(&server,1,&proxy));
    }
    server.settings.video_telemetry_format=CA_VIDEO_TELEMETRY_APCG;
    assert(stream_count(&server)==base && stream_source(&server,base+1)==0);
    puts("PASS stream IDs remain stable across thermal source loss/recovery");
}
