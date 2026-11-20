#include "camera_app/rtsp.h"
#include "camera_app/video_metadata.h"
#include "camera_app/misb.h"
#include "camera_app/support_video.h"
#include "camera_app/log.h"
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <algorithm>

#include "EventLoop.h"
#include "H264Source.h"
#include "H265Source.h"
#include "MediaSession.h"
#include "RtspServer.h"

#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <cstring>
#include <unordered_set>
#include <vector>

static std::mutex satip_clients_mutex;
static std::unordered_set<const void *> satip_clients;

extern "C" void ca_rtsp_mark_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    satip_clients.insert(client);
}

extern "C" void ca_rtsp_unmark_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    satip_clients.erase(client);
}

extern "C" bool ca_rtsp_is_satip_client(const void *client)
{
    std::lock_guard<std::mutex> lock(satip_clients_mutex);
    return satip_clients.find(client) != satip_clients.end();
}

// Compound RTCP sender report + SDES/CNAME. Caller supplies at least 64 bytes.
// Values are host order; RTP octet counts exclude the RTP and TCP headers.
extern "C" size_t ca_rtsp_sender_report(uint8_t *out, uint32_t ssrc, uint64_t ntp,
                                        uint32_t timestamp, uint32_t packets,
                                        uint32_t octets)
{
    const auto put32 = [](uint8_t *p, uint32_t value) {
        for (unsigned i = 0; i < 4; i++) p[i] = value >> (24 - 8*i);
    };
    memset(out, 0, 64);
    out[0] = 0x80; out[1] = 200; out[3] = 6;
    put32(out+4, ssrc);
    put32(out+8, ntp >> 32);
    put32(out+12, ntp);
    put32(out+16, timestamp);
    put32(out+20, packets);
    put32(out+24, octets);
    char cname[24];
    const size_t name_length = snprintf(cname, sizeof(cname), "apcam-%08x", ssrc);
    const size_t sdes_length = (4 + 4 + 2 + name_length + 1 + 3) & ~size_t(3);
    out[28] = 0x81; out[29] = 202; out[31] = sdes_length/4 - 1;
    put32(out+32, ssrc);
    out[36] = 1; out[37] = name_length;
    memcpy(out+38, cname, name_length);
    return 28 + sdes_length;
}

static std::string base64(const std::vector<uint8_t> &data)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (size_t i = 0; i < data.size(); i += 3) {
        const unsigned n = (unsigned(data[i]) << 16) |
            (i + 1 < data.size() ? unsigned(data[i+1]) << 8 : 0) |
            (i + 2 < data.size() ? data[i+2] : 0);
        result += alphabet[(n >> 18) & 63];
        result += alphabet[(n >> 12) & 63];
        result += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
        result += i + 2 < data.size() ? alphabet[n & 63] : '=';
    }
    return result;
}

// Updated before PushFrame, which skips sources when no clients are attached.
// Aliases share the same parameters. DESCRIBE runs on the RTSP event thread.
class CodecParameters {
public:
    explicit CodecParameters(ca_video_codec codec) : codec_(codec) {}
    int update(const uint8_t *data, size_t size, ca_misb_ts *misb)
    {
        size_t offset = 0, nal, length;
        bool picture = false;
        std::lock_guard<std::mutex> lock(mutex_);
        while (ca_annexb_next(data, size, &offset, &nal, &length)) {
            if (!length) continue;
            picture |= ca_video_nal_is_vcl(codec_, data[nal]);
            // Cache small parameter sets without packaging an unused format.
            if (misb && ca_misb_ts_cache_nal(misb, codec_, data+nal, length)<0) return -1;
            // Bound the SDP size as well as cached encoder data.
            if (length < 2 || length > 512) continue;
            const unsigned type = codec_ == CA_VIDEO_H265 ? (data[nal] >> 1) & 63 : data[nal] & 31;
            std::vector<uint8_t> *parameter = nullptr;
            if (codec_ == CA_VIDEO_H265) {
                if (type == 32) parameter = &vps_;
                if (type == 33) parameter = &sps_;
                if (type == 34) parameter = &pps_;
            } else {
                if (type == 7 && length >= 4) parameter = &sps_;
                if (type == 8) parameter = &pps_;
            }
            if (parameter) parameter->assign(data + nal, data + nal + length);
        }
        return picture ? 1 : 0;
    }
    std::string attribute()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sps_.empty() || pps_.empty()) return "";
        if (codec_ == CA_VIDEO_H265) {
            if (vps_.empty()) return "";
            return "a=rtpmap:96 H265/90000\r\na=fmtp:96 sprop-vps=" + base64(vps_) +
                ";sprop-sps=" + base64(sps_) + ";sprop-pps=" + base64(pps_);
        }
        char profile[7];
        snprintf(profile, sizeof(profile), "%02x%02x%02x", sps_[1], sps_[2], sps_[3]);
        return std::string("a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=") +
            profile + ";sprop-parameter-sets=" + base64(sps_) + "," + base64(pps_);
    }
private:
    ca_video_codec codec_;
    std::mutex mutex_;
    std::vector<uint8_t> vps_, sps_, pps_;
};

// Build SDP on each DESCRIBE: xop caches its first SDP forever, including
// missing encoder headers or the IP address of a different network interface.
std::string ca_rtsp_sdp(xop::MediaSession *session, const std::string &ip)
{
    std::string sdp = "v=0\r\no=- " + std::to_string(session->GetMediaSessionId()) +
        " 1 IN IP4 " + ip + "\r\ns=AP_CameraGimbal\r\nc=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\na=control:*\r\na=range:npt=0-\r\n";
    for (int ch = 0; ch < xop::MAX_MEDIA_CHANNEL; ch++) {
        auto *source = session->GetMediaSource(static_cast<xop::MediaChannelId>(ch));
        if (!source) continue;
        const std::string attribute = source->GetAttribute();
        if (attribute.empty()) return "";
        sdp += source->GetMediaDescription(0) + "\r\n" + attribute +
            "\r\na=control:track" + std::to_string(ch) + "\r\n";
    }
    return sdp;
}

/* xop's codec sources packetize one NAL and mark its final packet. Feed a
 * complete access unit here so only its final NAL carries the RTP marker. */
class AccessUnitSource : public xop::MediaSource {
public:
    AccessUnitSource(enum ca_video_codec codec, unsigned frame_rate,
                     std::shared_ptr<CodecParameters> parameters)
        : source_(codec == CA_VIDEO_H265
                      ? static_cast<xop::MediaSource *>(xop::H265Source::CreateNew(frame_rate))
                      : static_cast<xop::MediaSource *>(xop::H264Source::CreateNew(frame_rate))),
          parameters_(std::move(parameters))
    {
        media_type_ = source_->GetMediaType();
        payload_ = source_->GetPayloadType();
        clock_rate_ = source_->GetClockRate();
        source_->SetSendFrameCallback([this](xop::MediaChannelId channel, xop::RtpPacket packet) {
            packet.last = packet.last && last_nal_;
            packet.timestamp = timestamp_; // preserve zero at RTP clock wrap
            return send_frame_callback_ && send_frame_callback_(channel, packet);
        });
    }
    std::string GetMediaDescription(uint16_t port) override
    {
        return source_->GetMediaDescription(port);
    }
    std::string GetAttribute() override
    {
        const std::string attribute = parameters_->attribute();
        // A client can DESCRIBE before the first encoded key frame. Keep
        // that session usable with the codec's basic SDP and in-band headers;
        // later DESCRIBEs include parameter sets once the encoder supplies them.
        return attribute.empty() ? source_->GetAttribute() : attribute;
    }
    bool HandleFrame(xop::MediaChannelId channel, xop::AVFrame frame) override
    {
        timestamp_ = frame.timestamp;
        size_t offset = 0, nal, length;
        bool sent = false;
        while (ca_annexb_next(frame.buffer.get(), frame.size, &offset, &nal, &length)) {
            if (length == 0) continue;
            xop::AVFrame part;
            part.buffer = std::shared_ptr<uint8_t>(frame.buffer, frame.buffer.get() + nal);
            part.size = static_cast<uint32_t>(length);
            part.type = frame.type;
            part.timestamp = frame.timestamp;
            last_nal_ = offset == frame.size;
            if (!source_->HandleFrame(channel, part)) return false;
            sent = true;
        }
        return sent;
    }
private:
    std::unique_ptr<xop::MediaSource> source_;
    std::shared_ptr<CodecParameters> parameters_;
    bool last_nal_ = false;
    uint32_t timestamp_ = 0;
};

/* RFC 2250 MPEG-TS over RTP. Each payload holds whole 188-byte TS packets;
 * the marker signals discontinuities, not the end of a video access unit. */
class TransportStreamSource : public xop::MediaSource {
public:
    TransportStreamSource() { payload_=33; clock_rate_=90000; }
    std::string GetMediaDescription(uint16_t port) override {
        return "m=video " + std::to_string(port) + " RTP/AVP 33";
    }
    std::string GetAttribute() override { return "a=rtpmap:33 MP2T/90000"; }
    bool HandleFrame(xop::MediaChannelId channel,xop::AVFrame frame) override {
        if(frame.size%188) return false;
        for(size_t at=0;at<frame.size;) {
            const size_t n=std::min(size_t(7*188),size_t(frame.size)-at);
            xop::RtpPacket packet;
            packet.type=frame.type; packet.timestamp=frame.timestamp;
            packet.size=RTP_TCP_HEAD_SIZE+RTP_HEADER_SIZE+n;
            memcpy(packet.data.get()+RTP_TCP_HEAD_SIZE+RTP_HEADER_SIZE,frame.buffer.get()+at,n);
            if(!send_frame_callback_ || !send_frame_callback_(channel,packet)) return false;
            at+=n;
        }
        return true;
    }
};

struct ca_rtsp {
    struct stream {
        ca_support_video *publisher = nullptr;
        std::shared_ptr<CodecParameters> parameters;
        xop::MediaSessionId session_id;
        xop::MediaSessionId misb_session = 0;
        ca_misb_ts misb;
        uint64_t misb_pts = 0;
        uint32_t misb_last_timestamp = 0;
        bool misb_clock_started = false;
        std::vector<xop::MediaSessionId> aliases;
        enum ca_video_codec codec;
        unsigned frame_rate;
        uint32_t next_timestamp;
        uint32_t timestamp_step;
    };
    std::shared_ptr<xop::EventLoop> loop;
    std::shared_ptr<xop::RtspServer> server;
    std::vector<stream> streams;
    std::mutex mutex;
};

static bool add_stream(ca_rtsp *rtsp, xop::MediaSessionId session_id,
                       enum ca_video_codec codec, unsigned frame_rate,
                       std::shared_ptr<CodecParameters> parameters, const char *path)
{
    ca_rtsp::stream stream;
    stream.parameters = std::move(parameters);
    stream.session_id = session_id;
    stream.codec = codec;
    stream.frame_rate = frame_rate;
    stream.next_timestamp = codec == CA_VIDEO_H265
                                ? xop::H265Source::GetTimestamp()
                                : xop::H264Source::GetTimestamp();
    stream.timestamp_step = 90000U / frame_rate;
    if(ca_video_metadata_format()==CA_VIDEO_TELEMETRY_MISB) {
        const std::string name=std::string(path)+".misb";
        auto *session=xop::MediaSession::CreateNew(name);
        if(!session) return false;
        session->AddSource(xop::channel_0,new TransportStreamSource);
        stream.misb_session=rtsp->server->AddSession(session);
        if(!stream.misb_session) return false;
    }
    rtsp->streams.push_back(stream);
    return true;
}

extern "C" int ca_rtsp_open(struct ca_rtsp **result, unsigned port,
                            const char *path, enum ca_video_codec codec,
                            unsigned frame_rate)
{
    if (result == nullptr || path == nullptr || *path == '\0' ||
        frame_rate == 0) return -1;
    std::unique_ptr<ca_rtsp> rtsp(new (std::nothrow) ca_rtsp);
    if (!rtsp) return -1;
    rtsp->loop = std::make_shared<xop::EventLoop>();
    rtsp->server = xop::RtspServer::Create(rtsp->loop.get());
    if (!rtsp->server || !rtsp->server->Start("0.0.0.0", port)) return -1;
    xop::MediaSession *session = xop::MediaSession::CreateNew(path);
    if (session == nullptr) return -1;
    auto parameters = std::make_shared<CodecParameters>(codec);
    session->AddSource(xop::channel_0, new AccessUnitSource(codec, frame_rate, parameters));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    if(!add_stream(rtsp.get(), session_id, codec, frame_rate, parameters, path)) return -1;
    *result = rtsp.release();
    return 0;
}

extern "C" int ca_rtsp_add_video(struct ca_rtsp *rtsp, const char *path,
                                  enum ca_video_codec codec,
                                  unsigned frame_rate, unsigned *stream_id)
{
    if (rtsp == nullptr || path == nullptr || *path == '\0' ||
        frame_rate == 0 || stream_id == nullptr) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    xop::MediaSession *session = xop::MediaSession::CreateNew(path);
    if (session == nullptr) return -1;
    auto parameters = std::make_shared<CodecParameters>(codec);
    session->AddSource(xop::channel_0, new AccessUnitSource(codec, frame_rate, parameters));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    *stream_id = static_cast<unsigned>(rtsp->streams.size());
    if(!add_stream(rtsp, session_id, codec, frame_rate, parameters, path)) return -1;
    return 0;
}

extern "C" int ca_rtsp_add_alias(struct ca_rtsp *rtsp, unsigned stream_id,
                                  const char *alias)
{
    if (rtsp == nullptr || alias == nullptr) return -1;
    if (*alias == '\0') return 0;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    ca_rtsp::stream &stream = rtsp->streams[stream_id];
    xop::MediaSession *session = xop::MediaSession::CreateNew(alias);
    if (session == nullptr) return -1;
    session->AddSource(xop::channel_0, new AccessUnitSource(stream.codec, stream.frame_rate, stream.parameters));
    xop::MediaSessionId session_id = rtsp->server->AddSession(session);
    if (session_id == 0) return -1;
    stream.aliases.push_back(session_id);
    return 0;
}

extern "C" void ca_rtsp_add_config_aliases(struct ca_rtsp *rtsp,
                                           const struct ca_config *settings)
{
    if (rtsp == nullptr || settings == nullptr) return;
    const char *aliases[2] = {settings->main_alias, settings->sub_alias};
    for (unsigned i = 0; i < 2; i++) {
        if (ca_rtsp_add_alias(rtsp, i, aliases[i]) < 0)
            ca_log("cannot serve /video%u as /%s", i + 1U, aliases[i]);
    }
}

static int collect_ts(void *opaque,const void *data,size_t length)
{
    auto &bytes=*static_cast<std::vector<uint8_t> *>(opaque);
    const auto *p=static_cast<const uint8_t *>(data);
    bytes.insert(bytes.end(),p,p+length);
    return 0;
}

static int push_video(struct ca_rtsp *rtsp, unsigned stream_id,
                      const uint8_t *data, size_t length, bool key_frame,
                      float hfov_deg, bool timed, uint32_t timestamp)
{
    if (rtsp == nullptr || data == nullptr || length == 0) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    ca_rtsp::stream &stream = rtsp->streams[stream_id];
    int picture=stream.parameters->update(data, length, stream.misb_session ? &stream.misb : nullptr);
    if(picture<0) return -1;
    if (timed) stream.next_timestamp = timestamp;
    const uint32_t stamp=stream.next_timestamp;
    // Advance the source clock even when every live output is idle.
    if(picture) stream.next_timestamp += stream.timestamp_step;
    bool elementary=stream.publisher || rtsp->server->HasPlayingClients(stream.session_id);
    for(auto alias:stream.aliases) elementary |= rtsp->server->HasPlayingClients(alias);
    if(elementary) {
        uint8_t *annotated=nullptr;
        size_t annotated_length=0;
        int inserted=ca_video_metadata_insert(stream.codec,data,length,stamp,hfov_deg,
                                               &annotated,&annotated_length);
        if(inserted<0) return -1;
        if(!inserted) {
            annotated=static_cast<uint8_t *>(std::malloc(length));
            if(!annotated) return -1;
            std::memcpy(annotated,data,length); annotated_length=length;
        }
        xop::AVFrame frame;
        frame.buffer=std::shared_ptr<uint8_t>(annotated,std::free);
        if(annotated_length>std::numeric_limits<uint32_t>::max()) return -1;
        frame.size=annotated_length;
        frame.type=key_frame ? xop::VIDEO_FRAME_I : xop::VIDEO_FRAME_P;
        frame.timestamp=stamp;
        ca_support_video_push(stream.publisher,annotated,annotated_length,stamp,key_frame);
        rtsp->server->PushFrame(stream.session_id,xop::channel_0,frame);
        for(auto alias:stream.aliases) rtsp->server->PushFrame(alias,xop::channel_0,frame);
    }
    if(stream.misb_session) {
        if(!stream.misb_clock_started) stream.misb_pts=stamp;
        else stream.misb_pts+=uint32_t(stamp-stream.misb_last_timestamp);
        stream.misb_last_timestamp=stamp; stream.misb_clock_started=true;
        if(!rtsp->server->HasPlayingClients(stream.misb_session)) {
            // A new viewer must start on an IDR with the cached headers.
            stream.misb.started=false;
            return 0;
        }
        ca_metadata metadata;
        timespec utc;
        ca_metadata_snapshot(&metadata); metadata.hfov_deg=hfov_deg;
        auto bytes=std::make_shared<std::vector<uint8_t>>();
        if(clock_gettime(CLOCK_REALTIME,&utc)<0 ||
           ca_misb_ts_write(&stream.misb,stream.codec,data,length,stream.misb_pts,
                            key_frame,&metadata,&utc,collect_ts,bytes.get())<0) return -1;
        if(!bytes->empty()) {
            if(bytes->size()>std::numeric_limits<uint32_t>::max()) return -1;
            xop::AVFrame ts;
            ts.buffer=std::shared_ptr<uint8_t>(bytes,bytes->data());
            ts.size=bytes->size();
            ts.type=key_frame ? xop::VIDEO_FRAME_I : xop::VIDEO_FRAME_P;
            ts.timestamp=stamp;
            rtsp->server->PushFrame(stream.misb_session,xop::channel_0,ts);
        }
    }
    return 0;
}

extern "C" int ca_rtsp_push_h264_stream(struct ca_rtsp *rtsp, unsigned stream_id,
                                          const uint8_t *data, size_t length,
                                          bool key_frame, float hfov_deg)
{
    return push_video(rtsp, stream_id, data, length, key_frame, hfov_deg, false, 0);
}

extern "C" int ca_rtsp_push_video_timed(struct ca_rtsp *rtsp, unsigned stream_id,
                                         const uint8_t *data, size_t length,
                                         bool key_frame, float hfov_deg, uint64_t pts_us)
{
    uint32_t timestamp = static_cast<uint32_t>((pts_us / 1000000U) * 90000U +
                                              (pts_us % 1000000U) * 9U / 100U);
    return push_video(rtsp, stream_id, data, length, key_frame, hfov_deg, true, timestamp);
}

extern "C" int ca_rtsp_push_video(struct ca_rtsp *rtsp, unsigned stream_id,
                                    const uint8_t *data, size_t length,
                                    bool key_frame, float hfov_deg)
{
    return ca_rtsp_push_h264_stream(rtsp, stream_id, data, length, key_frame, hfov_deg);
}

extern "C" int ca_rtsp_push_h264(struct ca_rtsp *rtsp,
                                  const uint8_t *data, size_t length,
                                  bool key_frame, float hfov_deg)
{
    return ca_rtsp_push_h264_stream(rtsp, 0, data, length, key_frame, hfov_deg);
}

extern "C" int ca_rtsp_set_frame_rate(struct ca_rtsp *rtsp,
                                        unsigned stream_id,
                                        unsigned frame_rate)
{
    if (rtsp == nullptr || frame_rate == 0) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    if (stream_id >= rtsp->streams.size()) return -1;
    rtsp->streams[stream_id].timestamp_step = 90000U / frame_rate;
    return 0;
}

extern "C" int ca_rtsp_support_proxy(struct ca_rtsp *rtsp,
                                        const struct ca_support_config *config)
{
    if (rtsp == nullptr || config == nullptr) return -1;
    std::lock_guard<std::mutex> lock(rtsp->mutex);
    for (size_t i = 0; i < rtsp->streams.size(); i++) {
        auto &stream = rtsp->streams[i];
        if (ca_support_video_open(&stream.publisher, config, i, stream.codec) < 0) return -1;
    }
    return 0;
}

extern "C" void ca_rtsp_close(struct ca_rtsp *rtsp)
{
    if (rtsp == nullptr) return;
    for (const auto &stream : rtsp->streams) ca_support_video_close(stream.publisher);
    {
        std::lock_guard<std::mutex> lock(rtsp->mutex);
        if (rtsp->server) {
            for (const ca_rtsp::stream &stream : rtsp->streams) {
                rtsp->server->RemoveSession(stream.session_id);
                if(stream.misb_session) rtsp->server->RemoveSession(stream.misb_session);
                for (xop::MediaSessionId alias : stream.aliases)
                    rtsp->server->RemoveSession(alias);
            }
        }
        rtsp->server.reset();
        rtsp->loop.reset();
    }
    delete rtsp;
}
