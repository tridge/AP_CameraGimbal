// Exercise native raw-stream metadata without a USB module or video clients.
#include "../camera_app/src/streaming/thermal_stream.cpp"
#include <cassert>
void ca_log(const char *,...) {}
void ca_metadata_snapshot(ca_metadata *pose) { *pose={}; }
bool ca_metadata_at(uint64_t,ca_metadata *) { return false; }
size_t ca_video_metadata_json(const ca_metadata *,const timespec *,uint64_t,char *out,size_t size) {
    assert(size>=3);memcpy(out,"{}",3);return 2;
}
int main() {
    ca_thermal_stream stream;
    stream.pixels.resize(640U*512U);stream.streaming_clients=1;
    std::vector<uint16_t> pixels(640U*512U,1234);
    timespec at{};clock_gettime(CLOCK_REALTIME,&at);
    ca_config config{};config.raw_stream_fps=5;
    for(unsigned fov : {2420U,3000U,0U}) {
        config.thermal_hfov_cd=fov;
        assert(ca_thermal_stream_configure(&stream,&config)==0);
        ca_thermal_stream_publish_counts(&stream,pixels.data(),&at,255,false,mono_us());
        char expected[64];
        if(fov)snprintf(expected,sizeof(expected),"\"hfov_deg\":%.6f",double(fov*.01f));
        else snprintf(expected,sizeof(expected),"\"hfov_deg\":null");
        assert(stream.json.find(expected)!=std::string::npos);
        assert(stream.json.find("\"gain\":-1")!=std::string::npos);
        assert(stream.json.find("\"temperature_calibration\":\"unverified_sensor_counts\"")!=std::string::npos);
    }
    puts("PASS native thermal metadata: configurable/unknown FOV and unknown gain");
}
