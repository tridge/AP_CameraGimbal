#include "camera_app/thermal_stream.h"
#include <stddef.h>
#include <errno.h>

void ca_thermal_test_pattern(uint16_t *pixels, uint64_t sequence)
{
    // All 65536 codes occur five times, including low bits and extrema. This
    // is an explicit sensor test pattern, not simulated terrain temperature.
    for (unsigned i = 0; i < 640U*512U; i++) pixels[i] = uint16_t(i + sequence*257U);
}

#ifdef CA_THERMAL_STREAM_FFV1
#include "thermal_codec.h"
#include "camera_app/support_video.h"
#include "thermal_matroska.h"
#include "camera_app/log.h"
#include "camera_app/metadata.h"
#include "camera_app/video_metadata.h"
#include "camera_app/raw_thermal.h"
#include "apcam/target.h"
#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <memory>
#include <mutex>
#include <deque>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/file.h>
#include <unistd.h>

using thermal_mkv::Bytes;
static std::atomic<unsigned> public_port{0}, public_fps{5};
static std::atomic<uint64_t> public_source_timeout{0}, public_last_frame{0};
static std::atomic<bool> enabled{true}, available{false};
static uint64_t mono_us() {
    timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000 + t.tv_nsec/1000;
}
struct ca_thermal_stream {
    std::atomic<uint64_t> source_timeout{0};
    std::atomic<unsigned> thermal_hfov_cd{unsigned(APCAM_LENS3_FOV_H*100)};
    int listener = -1;
    ca_support_video *proxy = nullptr;
    pthread_t thread{};
    bool started = false, simulated = false;
    std::atomic<bool> stop{false};
    std::mutex lock;
    std::vector<uint16_t> pixels;
    std::string json;
    uint64_t sequence = 0, capture_us = 0;
    ca_metadata pose{};
    timespec captured_at{};
    struct SurveyFrame {
        std::vector<uint16_t> pixels;
        std::string json;
        uint64_t capture, sequence;
    };
    std::deque<SurveyFrame> survey_frames;
    std::atomic<unsigned> survey_pending{0};
    ThermalCodec codec;
    Bytes header;
    std::mutex record_lock;
    int record_fd = -1;
    unsigned record_fps = 5;
    bool recording = false;
    std::string video_path, record_path;
    uint64_t record_generation = 0, record_start = 0, record_epoch = 0;
    uint64_t record_previous = 0, next_record = 0;
    // publish skips all per-frame work unless something consumes frames
    std::atomic<unsigned> streaming_clients{0};
    std::atomic<bool> record_open{false};
    int wake[2] = {-1, -1};
    ~ca_thermal_stream() { for (int fd:wake) if (fd >= 0) close(fd); }
};

static void wake_worker(ca_thermal_stream *s)
{
    const char b = 0;
    if (s->wake[1] >= 0) (void)!write(s->wake[1], &b, 1);
}

static bool frames_wanted(const ca_thermal_stream *s)
{
    return s->survey_pending.load() != 0 || s->streaming_clients.load() != 0 || s->record_open.load() ||
           (s->proxy && enabled.load());
}
struct ThermalClient {
    int fd;
    std::string request;
    bool streaming = false;
    uint64_t progress_us;
    std::shared_ptr<const Bytes> pending;
    size_t offset = 0;
    std::deque<std::shared_ptr<const Bytes>> survey_queue;
};

static void publish_frame(ca_thermal_stream *s, const uint16_t *pixels, uint64_t now,
                          const char *telemetry, uint8_t gain, bool rotated,
                          const char *simulation_source, float hfov,
                          const ca_metadata &pose, const timespec &captured_at,
                          bool calibrated=true)
{
    unsigned minimum = UINT16_MAX, maximum = 0;
    for (unsigned i=0;i<640U*512U;i++) {
        if (pixels[i]<minimum) minimum=pixels[i];
        if (pixels[i]>maximum) maximum=pixels[i];
    }
    std::lock_guard<std::mutex> guard(s->lock);
    ++s->sequence;
    char hfov_json[32] = "null";
    const bool known_fov = isfinite(hfov) && hfov > 0 && hfov < 180;
    if (known_fov) snprintf(hfov_json,sizeof(hfov_json),"%.6f",double(hfov));
    char minimum_c[32]="null",maximum_c[32]="null";
    if (calibrated) {
        snprintf(minimum_c,sizeof(minimum_c),"%.6f",minimum/64.0-273.15);
        snprintf(maximum_c,sizeof(maximum_c),"%.6f",maximum/64.0-273.15);
    }
    char json[CA_VIDEO_METADATA_JSON_MAX+2048];
    int n = snprintf(json,sizeof(json),
        "{\"schema\":\"apcg.thermal.v1\",\"frame_id\":%llu,\"capture_monotonic_us\":%llu,"
        "\"timestamp_source\":\"%s\",\"simulated\":%s,\"simulation_source\":\"%s\",\"width\":640,\"height\":512,"
        "\"pixel_format\":\"gray16le\",\"bits_per_sample\":16,"
        "\"temperature_scale_k\":%s,\"temperature_offset_k\":%s,\"temperature_calibration\":\"%s\",\"gain\":%d,"
        "\"minimum_raw\":%u,\"maximum_raw\":%u,\"minimum_c\":%s,\"maximum_c\":%s,"
        "\"rotation_deg\":%u,\"hfov_deg\":%s,\"calibration\":\"%s\","
        "\"altitude_datum\":\"AMSL\",\"gimbal_frame\":\"roll_pitch_level_yaw_vehicle\","
        "\"telemetry\":%s}",
        (unsigned long long)s->sequence,(unsigned long long)now,
        s->simulated?"simulation":"usb_receive",s->simulated?"true":"false",simulation_source,calibrated?"0.015625":"null",calibrated?"0":"null",
        calibrated?"sensor_kelvin_64":"unverified_sensor_counts",gain==255?-1:int(gain),
        minimum,maximum,minimum_c,maximum_c,
        rotated?180:0,hfov_json,known_fov?"nominal_pinhole":"unknown",telemetry);
    if (n<0 || size_t(n)>=sizeof(json)) return;
    memcpy(s->pixels.data(),pixels,640U*512U*sizeof(uint16_t));
    s->json.assign(json,size_t(n)); s->capture_us=now;
    s->pose=pose; s->captured_at=captured_at;
    if(strcmp(simulation_source,"terrain")!=0) (void)ca_metadata_at(now/1000,&s->pose);
    wake_worker(s);
}

static void publish_sensor(ca_thermal_stream *s, const uint16_t *pixels,
                           const timespec *captured_at, uint8_t gain, bool rotated, bool calibrated,uint64_t received_us=0)
{
    if (!s || !pixels || !captured_at) return;
    const uint64_t current=mono_us();
    const uint64_t now=received_us?received_us:current;
    if(now>current || current-now>1000000)return;
    public_last_frame=now;
    if (!frames_wanted(s)) {
        // frame_id still counts every sensor frame; drop the cached frame so
        // a new consumer never starts from a stale one
        std::lock_guard<std::mutex> guard(s->lock);
        ++s->sequence;
        s->json.clear();
        return;
    }
    ca_metadata metadata;
    ca_metadata_snapshot(&metadata); // freeze telemetry with pixels, never at client-read time
    char telemetry[CA_VIDEO_METADATA_JSON_MAX];
    if (!ca_video_metadata_json(&metadata,captured_at,now*9/100,telemetry,sizeof(telemetry))) return;
    publish_frame(s, pixels, now, telemetry, gain, rotated,
                  s->simulated ? "test_pattern" : "", s->thermal_hfov_cd.load()*.01f, metadata, *captured_at, calibrated);
}

void ca_thermal_stream_publish(ca_thermal_stream *s,const uint16_t *pixels,
                               const timespec *at,uint8_t gain,bool rotated) {
    publish_sensor(s,pixels,at,gain,rotated,true);
}
void ca_thermal_stream_publish_counts(ca_thermal_stream *s,const uint16_t *pixels,
                                      const timespec *at,uint8_t gain,bool rotated,uint64_t received_us) {
    publish_sensor(s,pixels,at,gain,rotated,false,received_us);
}
void ca_thermal_stream_source_timeout(ca_thermal_stream *s,unsigned milliseconds) {
    if(!s)return;
    s->source_timeout=uint64_t(milliseconds)*1000;
    public_source_timeout=s->source_timeout.load();public_last_frame=0;
}
void ca_thermal_stream_publish_terrain(ca_thermal_stream *s, const uint16_t *pixels,
                                      uint64_t capture_us, const char *telemetry, uint8_t gain, float hfov,
                                      const ca_metadata *render_pose, const timespec *render_utc)
{
    if (!s || !pixels || !telemetry || !*telemetry) return;
    // Terrain pixels are already upright. Metadata belongs to the rendered
    // pose, not the newer vehicle state at encoding/client delivery time.
    ca_metadata pose=render_pose?*render_pose:ca_metadata{};
    timespec captured=render_utc?*render_utc:timespec{};
    publish_frame(s, pixels, capture_us, telemetry, gain, false, "terrain", hfov, pose, captured);
}

int ca_thermal_stream_capture(ca_thermal_stream *s, const char *root,
                              const ca_survey_request &request, ca_survey_result &result,
                              const std::atomic<uint64_t> &generation)
{
    if(!s) { errno=ENOTSUP; return -1; }
    struct Pending {
        std::atomic<unsigned> &n;
        Pending(std::atomic<unsigned> &v):n(v) { ++n; }
        ~Pending() { --n; }
    } pending(s->survey_pending);
    ca_thermal_stream::SurveyFrame frame;
    while(true) {
        if(generation.load()!=request.generation) { errno=ECANCELED; return -1; }
        if(mono_us()/1000>request.deadline_ms) { errno=ETIMEDOUT; return -1; }
        {
            std::lock_guard<std::mutex> g(s->lock);
            if(s->capture_us/1000>request.eligible_ms && !s->json.empty()) {
                frame={s->pixels,s->json,s->capture_us,s->sequence};
                result.pose=s->pose; result.captured_at=s->captured_at;
                result.frame_ms=s->capture_us/1000;
                break;
            }
        }
        usleep(5000);
    }
    double corners[4][2];
    if(!ca_survey_footprint(result.pose,request.target_alt,request.hfov,request.aspect,corners)) { errno=ETIMEDOUT; return -1; }
    ca_raw_thermal *raw=nullptr;
    if(ca_raw_thermal_open(&raw,root)<0) return -1;
    int ret=ca_raw_thermal_write_at(raw,frame.pixels.data(),640,512,&result.captured_at);
    if(!ret) snprintf(result.path,sizeof(result.path),"%s",ca_raw_thermal_path(raw));
    ca_raw_thermal_close(raw);
    if(ret<0) return -1;
    std::lock_guard<std::mutex> g(s->lock);
    result.stream_queued=s->survey_frames.size()<32;
    if(result.stream_queued) {
        char json[2048];
        if(ca_survey_json(result,json,sizeof(json))) {
            frame.json.pop_back(); frame.json+=",\"survey\":";
            frame.json+=json; frame.json+="}";
            s->survey_frames.push_back(std::move(frame));
            wake_worker(s);
        } else result.stream_queued=false;
    }
    if(!result.stream_queued) ca_log("Survey stream queue full; image %d retained on SD",request.index);
    return 0;
}

static int write_all(int fd, const Bytes &data)
{
    // Files downloads snapshot an immutable prefix between complete clusters.
    if (flock(fd, LOCK_EX) < 0) return -1;
    off_t start = lseek(fd, 0, SEEK_CUR);
    size_t offset = 0;
    int result = start < 0 ? -1 : 0;
    while (result == 0 && offset < data.size()) {
        ssize_t n = write(fd, data.data()+offset, data.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; result = -1; break; }
        offset += size_t(n);
    }
    int saved = errno;
    // Keep earlier frames readable if storage fills part way through a frame.
    if (result < 0 && start >= 0 && ftruncate(fd, start) < 0)
        ca_log("raw thermal partial-frame truncation failed: %s", strerror(errno));
    (void)flock(fd, LOCK_UN);
    errno = saved;
    return result;
}

// Called with record_lock held; stop waits for the current complete frame.
static int close_recording(ca_thermal_stream *s)
{
    if (s->record_fd < 0) return 0;
    int result = fdatasync(s->record_fd);
    int saved = errno;
    if (close(s->record_fd) < 0 && result == 0) { result = -1; saved = errno; }
    s->record_fd = -1;
    s->record_open = false;
    ++s->record_generation;
    ca_log("raw thermal recording stopped: %s%s", s->record_path.c_str(),
           result < 0 ? " (flush failed)" : "");
    errno = saved;
    return result;
}

static int open_recording(ca_thermal_stream *s)
{
    std::string stem = s->video_path;
    if (stem.size() >= 4 && stem.compare(stem.size()-4, 4, ".mp4") == 0)
        stem.resize(stem.size()-4);
    else if (stem.size() >= 3 && stem.compare(stem.size()-3, 3, ".ts") == 0)
        stem.resize(stem.size()-3);
    for (unsigned part=0; part<10000; part++) {
        s->record_path = stem + ".raw" + (part ? "-" + std::to_string(part) : "") + ".mkv";
        s->record_fd = open(s->record_path.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC, 0644);
        if (s->record_fd >= 0) break;
        if (errno != EEXIST) return -1;
    }
    if (s->record_fd < 0) return -1;
    if (write_all(s->record_fd, s->header) < 0) {
        int saved = errno;
        close(s->record_fd); s->record_fd = -1;
        unlink(s->record_path.c_str()); errno = saved;
        return -1;
    }
    ++s->record_generation;
    s->record_open = true;
    s->record_start = mono_us();
    s->record_epoch = s->record_previous = s->next_record = 0;
    ca_log("raw thermal recording started: %s (%u fps)", s->record_path.c_str(), s->record_fps);
    return 0;
}

int ca_thermal_stream_recording(ca_thermal_stream *s, bool active, const char *video_path)
{
    if (!s) return 0;
    std::lock_guard<std::mutex> guard(s->record_lock);
    if (!active) {
        s->recording = false;
        return close_recording(s);
    }
    if (s->recording) return 0;
    if (!video_path || !*video_path) { errno = EINVAL; return -1; }
    s->video_path = video_path;
    if (s->record_fps && open_recording(s) < 0) return -1;
    s->recording = true;
    return 0;
}

int ca_thermal_stream_configure(ca_thermal_stream *s, const ca_config *settings)
{
    if (!s) return 0;
    if (!settings || settings->raw_stream_fps < 1 || settings->raw_stream_fps > 25 ||
        settings->raw_record_fps > 25) { errno = EINVAL; return -1; }
    std::lock_guard<std::mutex> guard(s->record_lock);
    unsigned previous = s->record_fps;
    s->record_fps = settings->raw_record_fps;
    if (!s->record_fps) {
        if (close_recording(s) < 0) { s->record_fps = previous; return -1; }
    } else if (s->recording && s->record_fd < 0 && open_recording(s) < 0) {
        s->record_fps = previous;
        return -1;
    }
    if (previous != s->record_fps) s->next_record = 0;
    public_fps = settings->raw_stream_fps;
#ifdef CAMERA_APP_ZT30_DEV
    s->thermal_hfov_cd = settings->thermal_hfov_cd;
#endif
    return 0;
}

static uint64_t next_deadline(uint64_t previous, uint64_t now, unsigned fps)
{
    uint64_t interval = 1000000 / fps;
    // Preserve phase when selecting from the 25 Hz sensor (e.g. 10 Hz needs
    // alternating two/three-frame gaps), but never queue catch-up frames.
    return previous ? previous + ((now-previous)/interval+1)*interval : now+interval;
}

static void *thermal_worker(void *opaque)
{
    auto *s = static_cast<ca_thermal_stream *>(opaque);
    std::vector<ThermalClient> clients;
    std::vector<uint16_t> pixels(640U*512U);
    uint64_t previous=0, next_encode=0, epoch=0, encoded_sequence=0;
    unsigned stream_fps = public_fps.load();
    Bytes encoded;
    std::vector<pollfd> waits;
    while (!s->stop.load()) {
        // Frames, recording changes and close all signal the wake pipe; the
        // timeout only polices stalled clients.
        waits.assign(1, pollfd{s->wake[0],POLLIN,0});
        if (s->listener >= 0) waits.push_back(pollfd{s->listener,POLLIN,0});
        for (const auto &c:clients) {
            waits.push_back(pollfd{c.fd,short(POLLIN | (c.pending ? POLLOUT : 0)),0});
        }
        poll(waits.data(),waits.size(),clients.empty() ? -1 : 500);
        char drain[64];
        while (read(s->wake[0],drain,sizeof(drain)) > 0) {}
        if (s->stop.load()) break;
        while (s->listener >= 0) {
            int fd=accept4(s->listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if (fd<0) break;
            if (clients.size()>=4) { close(fd); continue; }
            clients.push_back(ThermalClient{fd,{},false,mono_us(),{},0});
        }
        const uint64_t now=mono_us();
        for (auto &c:clients) {
            if (!enabled.load()) { close(c.fd); c.fd=-1; continue; }
            if (!c.streaming) {
                char buffer[1024];
                ssize_t n=recv(c.fd,buffer,sizeof(buffer),0);
                if (n>0) c.request.append(buffer,size_t(n));
                else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) { close(c.fd); c.fd=-1; continue; }
                if (c.request.size()>4096) { close(c.fd); c.fd=-1; continue; }
                if (c.request.find("\r\n\r\n")!=std::string::npos) {
                    if (c.request.rfind("GET /thermal.mkv HTTP/1.1\r\n",0) != 0 &&
                        c.request.rfind("GET /thermal.mkv HTTP/1.0\r\n",0) != 0) {
                        close(c.fd); c.fd=-1; continue;
                    }
                    const char response[]="HTTP/1.1 200 OK\r\nContent-Type: video/x-matroska\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
                    auto out=std::make_shared<Bytes>(response,response+sizeof(response)-1);
                    thermal_mkv::append(*out,s->header); c.pending=out;
                    c.streaming=true; c.progress_us=now;
                }
            } else {
                // discard anything a streaming client sends, or poll() stays
                // readable and the worker spins
                char buffer[256];
                ssize_t n;
                while ((n=recv(c.fd,buffer,sizeof(buffer),0)) > 0) {}
                if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) {
                    close(c.fd); c.fd=-1; continue;
                }
            }
            if (!c.pending && !c.survey_queue.empty()) {
                c.pending=c.survey_queue.front(); c.survey_queue.pop_front(); c.offset=0; c.progress_us=now;
            }
            if (c.pending) {
                const auto &b=*c.pending;
                ssize_t n=send(c.fd,b.data()+c.offset,b.size()-c.offset,MSG_NOSIGNAL);
                if (n>0) {
                    c.offset+=n; c.progress_us=now;
                    if (c.offset==b.size()) { c.pending.reset(); c.offset=0; }
                } else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) {
                    close(c.fd); c.fd=-1; continue;
                }
            }
            if ((!c.streaming || c.pending) && now-c.progress_us>2000000) { close(c.fd); c.fd=-1; }
        }
        for (size_t i=0;i<clients.size();) {
            if (clients[i].fd<0) clients.erase(clients.begin()+i); else i++;
        }
        unsigned streaming=0;
        for (const auto &c:clients) streaming += c.streaming;
        s->streaming_clients = streaming;
        if (stream_fps != public_fps.load()) {
            stream_fps = public_fps.load();
            next_encode = 0;
        }
        bool stream_due = s->proxy && enabled.load();
        for (auto &c:clients) stream_due |= c.streaming && !c.pending;
        bool survey_due=false;
        {
            std::lock_guard<std::mutex> g(s->lock);
            survey_due=!s->survey_frames.empty();
        }
        if(s->survey_pending.load() && !survey_due) continue;
        stream_due = survey_due || (stream_due && now >= next_encode);
        uint64_t generation;
        bool record_due;
        {
            std::lock_guard<std::mutex> guard(s->record_lock);
            record_due = s->record_fd >= 0 && now >= s->next_record;
            generation = s->record_generation;
        }
        if (!stream_due && !record_due) continue;
        std::string json;
        uint64_t sequence,capture;
        {
            std::lock_guard<std::mutex> guard(s->lock);
            if(survey_due) {
                auto &f=s->survey_frames.front();
                sequence=f.sequence; capture=f.capture;
                pixels=std::move(f.pixels); json=std::move(f.json);
                s->survey_frames.pop_front();
            } else {
                sequence=s->sequence; capture=s->capture_us;
                const uint64_t timeout=s->source_timeout.load();
                if (s->json.empty() || (timeout && (now<capture || now-capture>timeout))) continue;
                pixels=s->pixels; json=s->json;
            }
        }
        stream_due = stream_due && (survey_due || sequence != previous);
        {
            std::lock_guard<std::mutex> guard(s->record_lock);
            record_due = record_due && generation == s->record_generation &&
                capture >= s->record_start && sequence != s->record_previous;
        }
        if (!stream_due && !record_due) continue;
        if (!epoch) epoch=capture;
        // Reuse the last intra frame if the other output selects it later.
        if (encoded_sequence != sequence && !s->codec.encode(pixels.data(),(capture-epoch)/1000,encoded)) {
            ca_log("thermal FFV1 encoding failed"); enabled=false;
            std::lock_guard<std::mutex> guard(s->record_lock);
            (void)close_recording(s);
            break;
        }
        encoded_sequence = sequence;
        if (stream_due) {
            auto data=std::make_shared<Bytes>(thermal_mkv::frame(encoded,json.c_str(),(capture-epoch)/1000));
            for (auto &c:clients) if (c.streaming) {
                if(!c.pending && c.survey_queue.empty()) { c.pending=data; c.progress_us=now; }
                else if(survey_due && c.survey_queue.size()<32) c.survey_queue.push_back(data);
                else if(survey_due) ca_log("Survey delivery gap on slow thermal client; recover images from SD");
            }
            if (enabled.load()) ca_support_video_push(s->proxy, data->data(), data->size(), 0, true);
            previous=sequence; next_encode=next_deadline(next_encode,now,stream_fps);
        }
        if (record_due) {
            std::lock_guard<std::mutex> guard(s->record_lock);
            // A stop/restart or rate change can occur while encoding.
            if (s->record_fd >= 0 && generation == s->record_generation && now >= s->next_record) {
                if (!s->record_epoch) s->record_epoch=capture;
                Bytes data=thermal_mkv::frame(encoded,json.c_str(),(capture-s->record_epoch)/1000);
                if (write_all(s->record_fd,data) < 0) {
                    ca_log("raw thermal recording write failed: %s",strerror(errno));
                    (void)close_recording(s);
                } else {
                    s->record_previous=sequence;
                    s->next_record=next_deadline(s->next_record,now,s->record_fps);
                }
            }
        }
    }
    for (auto &c:clients) close(c.fd);
    return nullptr;
}

static bool env_unsigned(const char *name,unsigned fallback,unsigned maximum,unsigned &value)
{
    const char *s=getenv(name);
    if (!s) { value=fallback; return value<=maximum; }
    char *end; errno=0; unsigned long n=strtoul(s,&end,10);
    if (!*s || *end || errno || n>maximum) return false;
    value=unsigned(n); return true;
}
int ca_thermal_stream_open(ca_thermal_stream **result,unsigned rtsp_port,bool simulated,const ca_config *settings)
{
    *result=nullptr;
    unsigned port;
    if (!settings || settings->raw_stream_fps < 1 || settings->raw_stream_fps > 25 ||
        settings->raw_record_fps > 25 ||
        !env_unsigned("CAMERA_APP_RAW_THERMAL_PORT",rtsp_port+2,65535,port)) { errno=EINVAL; return -1; }
    unsigned fps = settings->raw_stream_fps;
    auto *s=new(std::nothrow) ca_thermal_stream;
    if (!s) { errno=ENOMEM; return -1; }
    s->simulated=simulated; s->pixels.resize(640U*512U);
    if (!s->codec.open()) { delete s; errno=ENOTSUP; return -1; }
    if (pipe(s->wake) < 0 || fcntl(s->wake[0],F_SETFL,O_NONBLOCK) < 0 ||
        fcntl(s->wake[1],F_SETFL,O_NONBLOCK) < 0 ||
        fcntl(s->wake[0],F_SETFD,FD_CLOEXEC) < 0 ||
        fcntl(s->wake[1],F_SETFD,FD_CLOEXEC) < 0) {
        int error=errno;
        delete s; errno=error; return -1;
    }
    s->header=thermal_mkv::header(s->codec.context->extradata,s->codec.context->extradata_size,0);
    s->record_fps = settings->raw_record_fps;
    if (port) {
        s->listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        int one=1; setsockopt(s->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_ANY);
        if (s->listener<0 || bind(s->listener,reinterpret_cast<sockaddr *>(&address),sizeof(address))<0 || listen(s->listener,4)<0) {
            int error=errno; if (s->listener>=0) close(s->listener); delete s; errno=error; return -1;
        }
    }
    if (ca_support_video_open_matroska(&s->proxy, &settings->support, s->header.data(), s->header.size()) < 0) {
        if (s->listener >= 0) close(s->listener);
        delete s; return -1;
    }
    enabled=true; public_fps=fps;public_source_timeout=0;public_last_frame=0;
    int error=pthread_create(&s->thread,nullptr,thermal_worker,s);
    if (error) { ca_support_video_close(s->proxy); close(s->listener); delete s; errno=error; return -1; }
    s->started=true; public_port=port; available=port || s->proxy; *result=s;
    ca_log("lossless thermal ready: HTTP port=%u FFV1 gray16 640x512 %u fps%s",port,fps,simulated?" (simulated)":"");
    return 0;
}
void ca_thermal_stream_close(ca_thermal_stream *s)
{
    if (!s) return;
    public_port=0; available=false; s->stop=true;
    wake_worker(s);
    if (s->started) pthread_join(s->thread,nullptr);
    ca_support_video_close(s->proxy);
    (void)ca_thermal_stream_recording(s, false, nullptr);
    if (s->listener >= 0) close(s->listener);
    delete s;
}
unsigned ca_thermal_stream_port() { return public_port.load(); }
bool ca_thermal_stream_available() {
    const uint64_t timeout=public_source_timeout.load(),last=public_last_frame.load(),now=mono_us();
    return available.load() && (!timeout || (last && now>=last && now-last<=timeout));
}
unsigned ca_thermal_stream_fps() { return public_fps.load(); }
bool ca_thermal_stream_enabled() { return enabled.load(); }
void ca_thermal_stream_enable(bool value) { enabled=value; }
#else
int ca_thermal_stream_open(ca_thermal_stream **s,unsigned,bool,const ca_config *) { *s=nullptr; return 0; }
void ca_thermal_stream_close(ca_thermal_stream *) {}
int ca_thermal_stream_configure(ca_thermal_stream *,const ca_config *) { return 0; }
int ca_thermal_stream_recording(ca_thermal_stream *,bool,const char *) { return 0; }
int ca_thermal_stream_capture(ca_thermal_stream *,const char *,const ca_survey_request &,ca_survey_result &,const std::atomic<uint64_t> &) { errno=ENOTSUP; return -1; }
void ca_thermal_stream_publish(ca_thermal_stream *,const uint16_t *,const timespec *,uint8_t,bool) {}
void ca_thermal_stream_source_timeout(ca_thermal_stream *, unsigned) {}
void ca_thermal_stream_publish_counts(ca_thermal_stream *,const uint16_t *,const timespec *,uint8_t,bool,uint64_t) {}
void ca_thermal_stream_publish_terrain(ca_thermal_stream *, const uint16_t *, uint64_t, const char *, uint8_t, float, const ca_metadata *, const timespec *) {}
unsigned ca_thermal_stream_port() { return 0; }
bool ca_thermal_stream_available() { return false; }
unsigned ca_thermal_stream_fps() { return 0; }
bool ca_thermal_stream_enabled() { return false; }
void ca_thermal_stream_enable(bool) {}
#endif
