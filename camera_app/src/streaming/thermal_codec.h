#pragma once
#include <stdint.h>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

class ThermalCodec {
public:
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    ~ThermalCodec() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); }
    bool open(AVCodecID id = AV_CODEC_ID_FFV1) {
        const AVCodec *codec = avcodec_find_encoder(id);
        if (!codec || !(context = avcodec_alloc_context3(codec))) return false;
        context->width = 640; context->height = 512;
        context->pix_fmt = AV_PIX_FMT_GRAY16LE;
        context->time_base = {1, 1000}; context->gop_size = 1;
        context->thread_count = 2;
        if (id == AV_CODEC_ID_FFV1) {
            context->level = 3;
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            av_opt_set_int(context->priv_data, "coder", 1, 0);
            av_opt_set_int(context->priv_data, "slicecrc", 1, 0);
        }
        if (avcodec_open2(context, codec, nullptr) < 0) return false;
        frame = av_frame_alloc(); packet = av_packet_alloc();
        if (!frame || !packet) return false;
        frame->format = context->pix_fmt; frame->width = 640; frame->height = 512;
        return av_frame_get_buffer(frame, 32) >= 0;
    }
    bool encode(const uint16_t *pixels, int64_t pts, std::vector<uint8_t> &out) {
        if (av_frame_make_writable(frame) < 0) return false;
        for (unsigned y = 0; y < 512; y++) {
            auto *row = frame->data[0] + y * frame->linesize[0];
            for (unsigned x = 0; x < 640; x++) {
                uint16_t v = pixels[y * 640 + x];
                row[2*x] = v; row[2*x+1] = v >> 8;
            }
        }
        frame->pts = pts;
        if (avcodec_send_frame(context, frame) < 0 || avcodec_receive_packet(context, packet) < 0) return false;
        out.assign(packet->data, packet->data + packet->size);
        av_packet_unref(packet);
        return true;
    }
};
