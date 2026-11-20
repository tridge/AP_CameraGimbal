/* Exercise the hardware routing used for A8 digital zoom. The realtime
 * sensor path cannot safely upscale a cropped image; SCL must read frames
 * from ISP port 1, including when recording at the full sensor resolution. */
#include "../src/backends/a8/pipeline.cpp"
#include "../src/backends/siyi/overlay.cpp"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

static unsigned bound, configured;
static m6_scl_port ports[CA_A8_VENC_COUNT+1];
void ca_log(const char *, ...) {}

static int bind_frame(unsigned short, m6_sys_bind *src, m6_sys_bind *dst,
                      unsigned src_rate, unsigned dst_rate, m6_sys_link type,
                      unsigned)
{
    assert(src_rate == 25 && dst_rate == 25);
    if (src->module == M6_SYS_MOD_VIF) {
        assert(dst->module == M6_SYS_MOD_ISP && dst->port == 0);
        assert(type == M6_SYS_LINK_REALTIME);
    } else if (src->module == M6_SYS_MOD_ISP) {
        assert(src->port == 1);
        assert(dst->module == M6_SYS_MOD_SCL && dst->device == 1 && dst->port == 0);
        assert(type == M6_SYS_LINK_FRAMEBASE);
    } else {
        assert(src->module == M6_SYS_MOD_SCL && src->device == 1);
        assert(dst->module == M6_SYS_MOD_VENC);
        assert(src->port == dst->channel && type == M6_SYS_LINK_FRAMEBASE);
    }
    bound++;
    return 0;
}

static int set_port(int dev, int chn, int port, m6_scl_port *config)
{
    assert(dev == 1 && chn == 0 && port >= 0 && port < 4);
    assert(config->pixFmt == M6_PIXFMT_YUV420SP);
    ports[port] = *config;
    configured++;
    return 0;
}

int main()
{
    auto &p = pipe_state;
    p.sys.fnBindExt = bind_frame;
    p.scl.fnSetPortConfig = set_port;
    assert(bind_front_end(&p) == 0 && bound == 2);
    p.stage = STAGE_VENC;
    p.plane.capt.width = 3840;
    p.plane.capt.height = 2160;
    const unsigned widths[] = {1920, 1280, 3840};
    const unsigned heights[] = {1080, 720, 2160};
    for (unsigned i = 0; i < 3; i++) {
        p.config.streams[i].width = widths[i];
        p.config.streams[i].height = heights[i];
        assert(bind_scl_venc(&p, i, M6_VENC_DEV_H26X_0, i) == 0);
        m6_sys_bind overlay = MI_DEST(i);
        assert(overlay.module == M6_SYS_MOD_SCL && overlay.device == 1);
        assert(overlay.channel == 0 && overlay.port == i);
    }
    assert(bound == 5);
    for (unsigned i=0;i<4;i++) p.port_enabled[i]=true;
    for (float zoom : {1.0f, 1.9f, 2.0f, 2.1f, 3.0f, 6.0f, 1.0f}) {
        configured = 0;
        assert(ca_a8_set_zoom(zoom) == 0 && configured == 4);
        assert(ports[3].output.width==320 && ports[3].output.height==180);
        assert(memcmp(&ports[3].crop, &ports[0].crop, sizeof(ports[0].crop)) == 0);
        for (unsigned i = 0; i < 3; i++) {
            assert(ports[i].output.width == widths[i]);
            assert(ports[i].output.height == heights[i]);
            assert(memcmp(&ports[i].crop, &ports[0].crop, sizeof(ports[i].crop)) == 0);
            if (zoom == 1) {
                assert(ports[i].crop.width == 0 && ports[i].crop.height == 0);
            } else {
                assert(ports[i].crop.width <= 3840 / zoom);
                assert(ports[i].crop.height <= 2160 / zoom);
                assert(ports[i].crop.width % 4 == 0 && ports[i].crop.height % 4 == 0);
                assert(ports[i].crop.x % 2 == 0 && ports[i].crop.y % 2 == 0);
            }
        }
    }
    assert(ca_a8_set_inverted(true) == 0);
    for (const auto &port : ports) assert(port.mirror && port.flip);
    puts("PASS A8 buffered zoom: ISP/SCL/encoder routing, crop, orientation and overlay destination");
}
