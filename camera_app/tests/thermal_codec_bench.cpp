// Standalone MT11/host FFV1 throughput and bit-exactness probe.
#include "../src/streaming/thermal_codec.h"
#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

static double ms() {
    return std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main(int argc, char **argv) {
    if (argc > 2) { fprintf(stderr,"usage: %s [640x512-gray16le.bin]\n",argv[0]); return 2; }
    std::vector<uint16_t> pixels(640U*512U);
    if (argc == 2) {
        FILE *f=fopen(argv[1],"rb");
        if (!f) { perror(argv[1]); return 1; }
        for (auto &v:pixels) {
            int lo=fgetc(f), hi=fgetc(f);
            if (lo<0 || hi<0) { fprintf(stderr,"truncated raw frame\n"); fclose(f); return 1; }
            v=lo|(hi<<8);
        }
        bool extra=fgetc(f)!=EOF; fclose(f);
        if (extra) { fprintf(stderr,"expected exactly one 655360-byte frame\n"); return 1; }
    }
    ThermalCodec encoder;
    if (!encoder.open()) { fprintf(stderr,"FFV1 encoder open failed\n"); return 1; }
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_FFV1);
    AVCodecContext *decoder=avcodec_alloc_context3(codec);
    AVCodecParameters *params=avcodec_parameters_alloc();
    if (!decoder || !params || avcodec_parameters_from_context(params,encoder.context)<0 ||
        avcodec_parameters_to_context(decoder,params)<0 || avcodec_open2(decoder,codec,nullptr)<0) return 1;
    avcodec_parameters_free(&params);
    AVPacket *packet=av_packet_alloc(); AVFrame *frame=av_frame_alloc();
    if (!packet || !frame) return 1;
    std::vector<double> durations;
    size_t bytes=0, maximum=0;
    double decode_ms=0;
    rusage before{},after{}; getrusage(RUSAGE_SELF,&before);
    for (unsigned n=0;n<100;n++) {
        if (argc==1) for (size_t i=0;i<pixels.size();i++) pixels[i]=uint16_t(i+n*257U);
        std::vector<uint8_t> encoded;
        double begin=ms();
        if (!encoder.encode(pixels.data(),n*200,encoded)) return 1;
        durations.push_back(ms()-begin); bytes+=encoded.size(); maximum=std::max(maximum,encoded.size());
        if (av_new_packet(packet,encoded.size())<0) return 1;
        memcpy(packet->data,encoded.data(),encoded.size());
        begin=ms();
        if (avcodec_send_packet(decoder,packet)<0 || avcodec_receive_frame(decoder,frame)<0) return 1;
        decode_ms+=ms()-begin;
        if (frame->format!=AV_PIX_FMT_GRAY16LE || frame->width!=640 || frame->height!=512) return 1;
        for (unsigned y=0;y<512;y++) for (unsigned x=0;x<640;x++) {
            const auto *p=frame->data[0]+y*frame->linesize[0]+2*x;
            if (pixels[y*640+x] != (p[0]|(p[1]<<8))) {
                fprintf(stderr,"mismatch frame=%u pixel=%u,%u\n",n,x,y); return 1;
            }
        }
        av_packet_unref(packet); av_frame_unref(frame);
    }
    getrusage(RUSAGE_SELF,&after);
    auto seconds=[](timeval t) { return t.tv_sec+t.tv_usec*1.0e-6; };
    std::sort(durations.begin(),durations.end());
    printf("FFV1 gray16le: 100/100 frames bit-exact (%s)\n",argc==2?argv[1]:"all 65536 sample codes");
    printf("encode median %.3f ms p95 %.3f ms max %.3f ms; decode mean %.3f ms\n",
        durations[50],durations[94],durations[99],decode_ms/100);
    printf("encoded mean %zu max %zu bytes/frame; ratio %.3f; CPU %.3f s; peak RSS %ld KiB\n",
        bytes/100,maximum,65536000.0/bytes,
        seconds(after.ru_utime)+seconds(after.ru_stime)-seconds(before.ru_utime)-seconds(before.ru_stime),after.ru_maxrss);
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&decoder);
    return 0;
}
