#pragma once
#include "mi/native_sdk.h"
#include "encoder_owner.h"
#include "pts_clock.h"
#include <atomic>
#include <memory>
#include <pthread.h>
namespace ca_zt30 {
// One extra hardware-encoded output from the primary RGB scaler. Its channel
// is created once per process and consumed by exactly one bounded-copy worker.
// Keep the existing H264 browser feed independent from RTSP/recording formats.
class VideoOutput {
public:
    using Frame=bool(*)(void*,const EncodedFrame&,uint64_t,bool);
    VideoOutput(NativeSdk &s,PacketMemory &m):sdk(s),encoder(s.api,s.encoder,m),buffer(new(std::nothrow) uint8_t[4U*1024*1024]) {}
    ~VideoOutput();
    int start(EncoderSource,unsigned scaler_port,unsigned encoder_channel,
              EncoderCodec,unsigned width,unsigned height,unsigned bitrate,Frame,void*);
    int select_source(EncoderSource source) { return encoder.rebind(source); }
    int close();
    bool healthy() const { return !failed.load(); }
private:
    static uint64_t now();
    void consume();
    NativeSdk &sdk;
    EncoderOwner encoder;
    std::unique_ptr<uint8_t[]> buffer;
    unsigned source=0,port=0;
    bool port_enabled=false,thread_started=false,used=false;
    EncoderCodec codec=EncoderCodec::H264;
    pthread_t thread{};
    std::atomic<bool> stop{false},failed{false};
    std::atomic<unsigned> frames{0};
    Frame callback=nullptr;
    void *opaque=nullptr;
    PtsClock clock;
};
}
