#pragma once
#include "mi/api.h"
#include "mi/encoder_abi.h"
#include <cstddef>
namespace ca_zt30 {
/* Single-use, single-threaded encoder consumer. RGB owns SYS and the scaler;
 * it must remain alive until this owner closes. No cached SDK fd is acquired.
 * All borrowed packet bytes are copied before releasing the filled prefix with unchanged SDK output fields.
 * A failed SDK operation poisons this owner. Recreate in a new process: SDK
 * channel state retains cached descriptors across DestroyChn/CreateChn.
 */
enum class EncoderCodec : uint32_t { H264=2, H265=3 };
enum class EncoderSource : uint32_t { Zoom=0, Wide=1 };
// ReadOnly is the qualified default: never calls SetH264Vui.
// Fixed30 is an explicit, unqualified live-write diagnostic.
enum class H264Timing { Fixed30, ReadOnly };
struct PacketMemory {
    bool (*pointer_slot)(const void*,uint32_t*);
    bool (*copy)(uint32_t,uint8_t*,size_t);
};
struct EncodedFrame {
    uint8_t *data=nullptr;
    size_t capacity=0,bytes=0;
    uint32_t sequence=0,packets=0;
    uint64_t pts[32]{};
};
struct EncoderOptions {
    unsigned width=1280,height=720,bitrate=1570000;
    unsigned channel=1,source_port=1;
    bool own_device=true;
};
class EncoderOwner {
public:
    EncoderOwner(Api &sys,EncoderApi &encoder,PacketMemory &memory):s(sys),a(encoder),m(memory) {}
    ~EncoderOwner() { close(); }
    EncoderOwner(const EncoderOwner&)=delete;
    EncoderOwner &operator=(const EncoderOwner&)=delete;
    int open(EncoderCodec codec=EncoderCodec::H265,H264Timing timing=H264Timing::ReadOnly,
             EncoderSource source=EncoderSource::Zoom,const EncoderOptions &options=EncoderOptions());
    // Additional software-input channel; caller owns SYS and VENC device 0.
    int rebind(EncoderSource); // Main thread; both source scalers must remain alive.
    int open_thermal(unsigned channel=2,EncoderCodec codec=EncoderCodec::H264);
    // JPEG device8: created once; channels never destroyed/recreated per shot.
    // Normal RGB input is SCL0/source/port0. Thermal uses software input.
    int open_jpeg(EncoderSource source,uint32_t channel,unsigned width,unsigned height,
                  bool own_device,bool software_input=false,unsigned quality=90);
    int start_once();
    int stop_once(); // H264 640x512/15fps, channel 2, no binds/device ownership
    // 1: copied frame, 0: no frame ready, -1: failure (raw_status retained).
    int copy_frame(EncodedFrame &output);
    int pending_packs(uint32_t &packs); // Query only, while started; no borrowed packet
    int read_h264_vui(H264Vui &output); // only on a ready H264 channel; failure poisons owner
    int close();
    bool ready() const { return started && (bound || software) && !unhealthy; }
    const char *failed_call=nullptr;
    int raw_status=0;
    bool have_default_vui() const { return got_default_vui; }
    const H264Vui &default_vui() const { return initial_vui; }
    const H264Vui &applied_vui() const { return verified_vui; }
private:
    bool check(int result,const char *call);
    void cleanup(int result,const char *call,int &first);
    Api &s;
    EncoderApi &a;
    PacketMemory &m;
    H264Vui initial_vui{},verified_vui{};
    bool got_default_vui=false,h264=false,jpeg=false;
    uint32_t source_channel=0,source_port=1,channel_id=1,device_id=0;
    bool software=false;
    bool used=false,unhealthy=false,device=false,channel=false,started=false,bound=false;
    EncoderPacket packets[32]{};
};
} // namespace ca_zt30
