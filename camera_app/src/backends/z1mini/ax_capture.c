/* Z1-Mini IMX415 -> AX ISP -> simultaneous 1080p/4K H264 capture.
 * Requires exclusive ownership: no vendor main/ISP or other AX initializer.
 * Uses installed stock libraries and sensor defaults, never the vendor app.
 * Vendor startup POD configuration is retained for this initial ABI probe.
 */
#define _GNU_SOURCE
#include "ax_config.h"
#include "camera_app/overlay.h"
#include <sys/socket.h>
#include "ax_venc_api.h"
#include "ax_vin_api.h"
#include "ax_isp_3a_api.h"
#include <errno.h>
#include "native.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
static atomic_bool stopped;
static atomic_uint overlay_request, overlay_done[2];
static atomic_int overlay_error;
static atomic_uint overlay_sequence;
static unsigned overlay_acked;
static uint8_t overlay_request_bytes[sizeof(struct ca_z1_overlay_request)];
static size_t overlay_request_length;

static int created, isp_open, vin_started, dev_enabled, stream_on, enc_created[2], enc_started[2];
static pthread_t isp_thread;
static int thread_started;
static pthread_t exposure_thread;
static bool exposure_started;
static uint64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ULL + t.tv_nsec / 1000000;
}
static int temp(void) {
    int v = 999999;
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = 999999;
        fclose(f);
    }
    return v;
}
static void stop(int sig) {
    (void)sig;
    stopped = 1;
}
static void *sym(const char *n) {
    void *p = dlsym(RTLD_DEFAULT, n);
    if (!p) {
        fprintf(stderr, "missing %s: %s\n", n, dlerror());
        exit(2);
    }
    return p;
}
typedef int (*fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static int call(const char *n, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    int r = ((fn4)sym(n))(a, b, c, d);
    if (r || (!strstr(n, "ReleaseYuvFrame") && !strstr(n, "ReleaseStream")))
        printf("%s=%08x\n", n, r);
    return r;
}
#define CALL(n, a, b, c, d)                                                                        \
    do {                                                                                           \
        if (call(#n, (uintptr_t)(a), (uintptr_t)(b), (uintptr_t)(c), (uintptr_t)(d)))              \
            goto cleanup;                                                                          \
    } while (0)
static void *isp_run(void *unused) {
    (void)unused;
    fn4 run = sym("AX_ISP_Run");
    while (!stopped) {
        int r = run(0, 0, 0, 0);
        if (r && !stopped) {
            printf("ISP_Run=%08x\n", r);
            usleep(10000);
        }
    }
    return NULL;
}
static int exclusive_owner(void) {
    int lock = open("/tmp/z1mini-native.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0)
            close(lock);
        return -1;
    }
    DIR *d = opendir("/proc");
    if (!d) {
        close(lock);
        return -1;
    }
    struct dirent *e;
    int result = 0;
    while ((e = readdir(d))) {
        if (!*e->d_name || strspn(e->d_name, "0123456789") != strlen(e->d_name))
            continue;
        char path[300], name[32];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(name, sizeof(name), f) &&
            (!strcmp(name, "main\n") || !strcmp(name, "vendor-isp\n"))) {
            // An unreaped, single-thread zombie has released all hardware resources.
            char statpath[300], state[32];
            snprintf(statpath, sizeof(statpath), "/proc/%s/status", e->d_name);
            FILE *status = fopen(statpath, "r");
            bool zombie = false;
            if (status) {
                while (fgets(state, sizeof(state), status))
                    if (!strncmp(state, "State:", 6) && strchr(state, 'Z'))
                        zombie = true;
                fclose(status);
            }
            if (zombie) {
                snprintf(statpath, sizeof(statpath), "/proc/%s/task", e->d_name);
                DIR *tasks = opendir(statpath);
                unsigned count = 0;
                struct dirent *task;
                if (tasks) {
                    while ((task = readdir(tasks)))
                        if (task->d_name[0] != '.')
                            count++;
                    closedir(tasks);
                }
                if (count != 1)
                    zombie = false;
            }
            if (!zombie) {
                fprintf(stderr, "refusing concurrent vendor media PID %s\n", e->d_name);
                result = -1;
            }
        }
        fclose(f);
    }
    closedir(d);
    if (result)
        close(lock);
    // Keep the flock descriptor for the process lifetime on success.
    return result;
}
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
/* SDK queries run independently of video encoding and gimbal control. */
static void *exposure_poll(void *opaque)
{
    FILE *out=opaque;
    typedef AX_S32 (*status_fn)(AX_U8, AX_ISP_IQ_AE_STATUS_T *);
    typedef AX_S32 (*param_fn)(AX_U8, AX_ISP_IQ_AE_PARAM_T *);
    status_fn query=(status_fn)dlsym(RTLD_DEFAULT,"AX_ISP_IQ_GetAeStatus");
    param_fn param=(param_fn)dlsym(RTLD_DEFAULT,"AX_ISP_IQ_GetAeParam");
    while (!stopped) {
        struct ca_exposure s=ca_exposure_empty(0,mono_ms()*1000);
        AX_ISP_IQ_AE_STATUS_T status={0};
        s.result=query ? query(0,&status) : -ENOTSUP;
        if (!s.result) {
            s.shutter_us=status.tExpStatus.nShutter;
            s.analog_gain=status.tExpStatus.nAGain/1024.0f;
            s.digital_gain=status.tExpStatus.nDgain/1024.0f;
            s.isp_gain=status.tExpStatus.nIspGain/1024.0f;
            s.luma=status.tAlgStatus.nWeightedMeanLuma/1024.0f;
            s.target=status.tExpStatus.nSetPoint/1024.0f;
            s.error=s.target-s.luma;
            s.valid=CA_AE_SHUTTER|CA_AE_AGAIN|CA_AE_DGAIN|CA_AE_IGAIN|
                    CA_AE_LUMA|CA_AE_TARGET|CA_AE_ERROR;
        }
        AX_ISP_IQ_AE_PARAM_T p={0};
        int r=param ? param(0,&p) : -ENOTSUP;
        if (!r) { s.mode=p.nEnable ? CA_AE_AUTO : CA_AE_MANUAL; s.valid|=CA_AE_MODE; }
        if (!s.result) s.result=r;
        /* No vendor convergence/boundary flag in this SDK; do not invent one. */
        struct ca_z1_native_header h={CA_Z1_NATIVE_AE_MAGIC,sizeof(s),s.time_us,0,0};
        pthread_mutex_lock(&output_lock);
        bool ok=fwrite(&h,1,sizeof(h),out)==sizeof(h) && fwrite(&s,1,sizeof(s),out)==sizeof(s);
        pthread_mutex_unlock(&output_lock);
        if (!ok) { stopped=true; break; }
        usleep(200000);
    }
    return NULL;
}

/* The native AX path already owns uncompressed NV12 frames. Touch only the
 * small cross region, in uncached mappings, before giving the frame to VENC.
 * No full-frame copy, new processing stage or extra frame queue is required. */
static int draw_cross(AX_VIDEO_FRAME_S *frame, const struct ca_overlay_bitmap *b)
{
    void *(*map)(AX_U64,AX_U32)=dlsym(RTLD_DEFAULT,"AX_SYS_Mmap");
    AX_S32 (*unmap)(void *,AX_U32)=dlsym(RTLD_DEFAULT,"AX_SYS_Munmap");
    unsigned stride=frame->u32PicStride[0];
    unsigned uv_stride=frame->u32PicStride[1] ? frame->u32PicStride[1] : stride;
    if (!map || !unmap || !b->pixels || stride<frame->u32Width || uv_stride<frame->u32Width ||
        frame->enCompressMode!=AX_COMPRESS_MODE_NONE || frame->u32LeftPadding ||
        b->x+b->width>frame->u32Width || b->y+b->height>frame->u32Height) return ENOTSUP;
    unsigned y_size=(b->height-1)*stride+b->width;
    unsigned uv_size=(b->height/2-1)*uv_stride+b->width;
    AX_U64 uv_base=frame->u64PhyAddr[1] ? frame->u64PhyAddr[1] :
        frame->u64PhyAddr[0]+(AX_U64)stride*frame->u32Height;
    unsigned char *y=map(frame->u64PhyAddr[0]+(AX_U64)b->y*stride+b->x,y_size);
    if (!y || y==(void *)-1) return EIO;
    unsigned char *uv=map(uv_base+(AX_U64)(b->y/2)*uv_stride+b->x,uv_size);
    if (!uv || uv==(void *)-1) { unmap(y,y_size); return EIO; }
    for (unsigned row=0;row<b->height;row++) for (unsigned col=0;col<b->width;col++) {
        uint16_t pixel=b->pixels[row*b->width+col];
        if (!(pixel&0x8000)) continue;
        y[row*stride+col]=(pixel&0x7fff) ? 235 : 16;
        uv[(row/2)*uv_stride+(col&~1U)]=128;
        uv[(row/2)*uv_stride+(col&~1U)+1]=128;
    }
    int result=unmap(uv,uv_size);
    result |= unmap(y,y_size);
    return result ? EIO : 0;
}

static void overlay_control(unsigned channel, AX_VIDEO_FRAME_S *frame, FILE *out,
                            const struct ca_overlay_bitmap *cross, unsigned request)
{
    int error=(request&1) ? (cross ? draw_cross(frame,cross) : ENOMEM) : 0;
    pthread_mutex_lock(&output_lock);
    /* Both the bitmap and this frame belong to the snapshotted request.
     * Never label old work with a sequence received while drawing it. */
    if (request!=atomic_load(&overlay_request)) {
        pthread_mutex_unlock(&output_lock);
        return;
    }
    if (error) atomic_store(&overlay_error,error);
    atomic_store(&overlay_done[channel],request);
    if (request!=overlay_acked && atomic_load(&overlay_done[0])==request &&
        atomic_load(&overlay_done[1])==request) {
        struct ca_z1_native_header h={CA_Z1_NATIVE_OVERLAY_MAGIC,0,
            atomic_load(&overlay_sequence),
            atomic_load(&overlay_error),request&1};
        if (fwrite(&h,1,sizeof(h),out)!=sizeof(h)) stopped=true;
        overlay_acked=request;
    }
    pthread_mutex_unlock(&output_lock);
}

static void read_overlay_request(void)
{
    ssize_t bytes=recv(3,overlay_request_bytes+overlay_request_length,
                       sizeof(overlay_request_bytes)-overlay_request_length,MSG_DONTWAIT);
    if (bytes>0) overlay_request_length+=(size_t)bytes;
    if (overlay_request_length!=sizeof(overlay_request_bytes)) return;
    struct ca_z1_overlay_request request;
    memcpy(&request,overlay_request_bytes,sizeof(request));
    overlay_request_length=0;
    if (!request.sequence || request.desired>1 ||
        request.reserved[0] || request.reserved[1] || request.reserved[2]) return;
    pthread_mutex_lock(&output_lock);
    unsigned previous=atomic_load(&overlay_request);
    atomic_store(&overlay_error,0);
    atomic_store(&overlay_sequence,request.sequence);
    atomic_store(&overlay_request,((previous&~1U)+2)|request.desired);
    pthread_mutex_unlock(&output_lock);
}

struct overlay_cache {
    struct ca_overlay_bitmap bits[CA_OVERLAY_REGIONS];
    bool enabled;
    unsigned width, height, request;
};

static void overlay_frame(unsigned channel, AX_VIDEO_FRAME_S *frame, FILE *out,
                          struct overlay_cache *cache)
{
    unsigned state=atomic_load(&overlay_request);
    bool enabled=(state&1U)!=0;
    if (enabled!=cache->enabled || (enabled &&
        (cache->width!=frame->u32Width || cache->height!=frame->u32Height ||
         (!cache->bits[0].pixels && cache->request!=state)))) {
        ca_overlay_free(cache->bits);
        cache->enabled=enabled;
        cache->width=frame->u32Width;
        cache->height=frame->u32Height;
        cache->request=state;
        if (enabled) {
            struct ca_overlay_geometry geometry;
            ca_overlay_geometry(&geometry,frame->u32Width,frame->u32Height,true,false,0);
            if (ca_overlay_bitmaps(cache->bits,frame->u32Width,frame->u32Height,&geometry)<0)
                ca_overlay_free(cache->bits);
        }
    }
    overlay_control(channel,frame,out,
                    enabled && cache->bits[0].pixels ? &cache->bits[0] : NULL,state);
}

struct encoder_worker {
    unsigned channel, limit, frames;
    size_t bytes;
    FILE *out;
    bool framed, failed;
};
static void *encode(void *opaque) {
    struct encoder_worker *worker = opaque;
    unsigned c = worker->channel;
    bool framed = worker->framed;
    int r;
    struct overlay_cache cross={0};
    uint64_t deadline = framed ? UINT64_MAX : mono_ms() + 15000, first_pts = 0;
    while (worker->frames < worker->limit && !stopped && mono_ms() < deadline) {
        if (temp() >= 75000) {
            puts("thermal stop");
            goto failed;
        }
        unsigned vin = c ? 0 : 1, w = c ? 3840 : 1920, h = c ? 2160 : 1080;
        union {
            uint64_t align;
            unsigned char raw[2048];
            AX_IMG_INFO_T info;
        } f = {0};
        r = ((fn4)sym("AX_VIN_GetYuvFrame"))(0, vin, (uintptr_t)&f, 1000);
        if (r) {
            printf("GetYuvFrame vin=%u r=%08x\n", vin, r);
            goto failed;
        }
        AX_VIDEO_FRAME_S *v = &f.info.tFrameInfo.stVFrame;
        if (!worker->frames)
            printf("YUV vin=%u %ux%u stride=%u size=%u\n", vin, v->u32Width, v->u32Height,
                   v->u32PicStride[0], v->u32FrameSize);
        if (v->u32Width != w || v->u32Height != h || v->enImgFormat != AX_YUV420_SEMIPLANAR) {
            call("AX_VIN_ReleaseYuvFrame", 0, vin, (uintptr_t)&f, 0);
            goto failed;
        }
        if (framed) {
            if (c==0) read_overlay_request();
            overlay_frame(c,v,worker->out,&cross);
        }
        r = ((fn4)sym("AX_VENC_SendFrame"))(c, (uintptr_t)&f.info.tFrameInfo, 1000, 0);
        int released = call("AX_VIN_ReleaseYuvFrame", 0, vin, (uintptr_t)&f, 0);
        if (r || released) {
            printf("SendFrame=%08x\n", r);
            goto failed;
        }
        AX_VENC_STREAM_S stream = {0};
        r = ((fn4)sym("AX_VENC_GetStream"))(c, (uintptr_t)&stream, 1000, 0);
        if (r) {
            printf("GetStream=%08x\n", r);
            goto failed;
        }
        size_t n = stream.stPack.u32Len;
        uint64_t pts = stream.stPack.u64PTS;
        if (!worker->frames)
            first_pts = pts;
        int good = stream.stPack.pu8Addr && n && n < CA_Z1_NATIVE_MAX_FRAME;
        if (framed)
            pthread_mutex_lock(&output_lock);
        if (good && framed) {
            bool key = false;
            const uint8_t *data = stream.stPack.pu8Addr;
            for (size_t i = 0; i + 3 < n; i++)
                if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
                    (data[i + 3] & 31) == 5) {
                    key = true;
                    break;
                }
            struct ca_z1_native_header header = {CA_Z1_NATIVE_MAGIC, (uint32_t)n, pts, key, c};
            good = fwrite(&header, 1, sizeof(header), worker->out) == sizeof(header);
        }
        good = good && fwrite(stream.stPack.pu8Addr, 1, n, worker->out) == n;
        if (framed)
            pthread_mutex_unlock(&output_lock);
        r = call("AX_VENC_ReleaseStream", c, (uintptr_t)&stream, 0, 0);
        if (!good || r)
            goto failed;
        worker->frames++;
        worker->bytes += n;
        if (worker->frames % 30 == 0)
            printf("stream=%u frames=%u bytes=%zu temp=%d pts_span_us=%llu\n", c, worker->frames,
                   worker->bytes, temp(), (unsigned long long)(pts - first_pts));
    }
    ca_overlay_free(cross.bits);
    return NULL;
failed:
    worker->failed = !stopped;
    stopped = true;
    ca_overlay_free(cross.bits);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s fd:3 0 | output-prefix frame-count\n", argv[0]);
        return 2;
    }
    bool framed = !strcmp(argv[1], "fd:3");
    unsigned limit = (unsigned)atoi(argv[2]);
    if (!limit && framed)
        limit = ~0U;
    else if (!limit || limit > 300)
        return 2;
    setbuf(stdout, NULL);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGPIPE, SIG_IGN);
    int result = 1;
    unsigned frames[2] = {0};
    size_t bytes[2] = {0};
    FILE *out[2] = {NULL, NULL};
    char path[512];
    for (unsigned c = 0; c < 2; c++) {
        snprintf(path, sizeof(path), "%s.%s.h264", argv[1], c ? "4k" : "1080");
        out[c] = framed ? (c ? out[0] : fdopen(3, "wb")) : fopen(path, "wb");
        if (!out[c])
            return 2;
        if (framed)
            setbuf(out[c], NULL);
    }
    if (temp() >= 70000) {
        fprintf(stderr, "too hot or sensor unavailable\n");
        return 2;
    }
    if (exclusive_owner())
        return 2;
    const char *libs[] = {"libax_sys.so",
                          "libax_mipi.so",
                          "libax_proton.so",
                          "libax_3a.so",
                          "libax_interpreter_external.so",
                          "libsns_imx415.so",
                          "libax_venc.so"};
    for (unsigned i = 0; i < sizeof(libs) / sizeof(libs[0]); i++)
        if (!dlopen(libs[i], RTLD_LAZY | RTLD_GLOBAL)) {
            fprintf(stderr, "%s: %s\n", libs[i], dlerror());
            return 2;
        }

    unsigned char floorplan[16384] __attribute__((aligned(8))) = {0};
    memcpy(floorplan, cfg_common_pools, sizeof(cfg_common_pools));
    // Separate NV12 downscaled buffers prevent wasting scarce 4K blocks.
    uint32_t *small_pool = (uint32_t *)(floorplan + 128);
    small_pool[0] = 4096;
    small_pool[2] = 1920 * 1080 * 3 / 2;
    small_pool[4] = 6;
    unsigned char raw_pool[64] __attribute__((aligned(8)));
    memcpy(raw_pool, cfg_raw_pool, sizeof(raw_pool));
    uint32_t chn[21];
    memcpy(chn, cfg_chn, sizeof(chn));
    // Actual 2023 channel ABI is seven words, including an extra trailing word.
    chn[0] = 1;
    chn[5] = 3;
    chn[8] = 1920;
    chn[9] = 1080;
    chn[10] = 1920;
    chn[12] = 3;
    uint32_t pool_attr[7];
    memcpy(pool_attr, cfg_pool_attr, sizeof(pool_attr));
    uint32_t npumode = 3, bind[8] = {1, 0};
    CALL(AX_SYS_Init, 0, 0, 0, 0);
    CALL(AX_POOL_Exit, 0, 0, 0, 0);
    CALL(AX_VIN_Init, 0, 0, 0, 0);
    CALL(AX_MIPI_RX_Init, 0, 0, 0, 0);
    CALL(AX_NPU_SDK_EX_Init_with_attr, &npumode, 0, 0, 0);
    CALL(AX_POOL_SetConfig, floorplan, 0, 0, 0);
    CALL(AX_POOL_Init, 0, 0, 0, 0);
    // Runtime sensor table is 120 bytes. Reset at +4, bus setter at +44, as
    // recovered from sensor.Base.Reset; older SDK headers omit the reset slot.
    void **sensor = sym("gSnsimx415Obj");
    int r = ((fn4)sensor[11])(0, 0, 0, 0);
    printf("sensor_bus(0)=%08x\n", r);
    if (r)
        goto cleanup;
    r = ((fn4)sensor[1])(0, 0, 0, 0);
    printf("sensor_reset=%08x\n", r);
    if (r)
        goto cleanup;
    CALL(AX_VIN_Create, 0, 0, 0, 0);
    created = 1;
    CALL(AX_VIN_SetRunMode, 0, 0, 0, 0);
    CALL(AX_VIN_RegisterSensor, 0, sensor, 0, 0);
    CALL(AX_VIN_SetSnsAttr, 0, cfg_sns, 0, 0);
    CALL(AX_VIN_OpenSnsClk, 0, 0, 27000000, 0);
    CALL(AX_MIPI_RX_Reset, 0, 0, 0, 0);
    CALL(AX_MIPI_RX_SetAttr, 0, cfg_mipi, 0, 0);
    CALL(AX_VIN_SetDevAttr, 0, cfg_dev, 0, 0);
    CALL(AX_VIN_SetPipeAttr, 0, cfg_pipe, 0, 0);
    CALL(AX_VIN_SetChnAttr, 0, chn, 0, 0);
    CALL(AX_VIN_SetDevBindPipe, 0, bind, 0, 0);
    CALL(AX_ISP_Open, 0, 0, 0, 0);
    isp_open = 1;
    CALL(AX_ISP_ALG_AeRegisterSensor, 0, sensor, 0, 0);
    void *ae[] = {sym("AX_ISP_ALG_AeInit"), sym("AX_ISP_ALG_AeRun"), sym("AX_ISP_ALG_AeDeInit"),
                  sym("AX_ISP_ALG_AeCtrl")};
    void *awb[] = {sym("AX_ISP_ALG_AwbInit"), sym("AX_ISP_ALG_AwbRun"), sym("AX_ISP_ALG_AwbDeInit"),
                   NULL};
    CALL(AX_ISP_RegisterAeLibCallback, 0, ae, 0, 0);
    CALL(AX_ISP_RegisterAwbLibCallback, 0, awb, 0, 0);
    int pool = call("AX_POOL_CreatePool", (uintptr_t)raw_pool, 0, 0, 0);
    if (pool < 0)
        goto cleanup;
    pool_attr[5] = (uint32_t)pool;
    CALL(AX_VIN_SetPoolAttr, 0, pool_attr, 1, 0);
    CALL(AX_VIN_Start, 0, 0, 0, 0);
    vin_started = 1;
    CALL(AX_VIN_EnableDev, 0, 0, 0, 0);
    dev_enabled = 1;
    CALL(AX_VIN_StreamOn, 0, 0, 0, 0);
    stream_on = 1;
    // Match stock UpsideDown=true using the recovered sensor callback.
    r = ((fn4)sensor[10])(0, 3, 0, 0);
    printf("sensor_mirror_flip(3)=%08x\n", r);
    if (r)
        goto cleanup;
    if (pthread_create(&isp_thread, NULL, isp_run, NULL))
        goto cleanup;
    thread_started = 1;

    AX_VENC_MOD_ATTR_S mod = {.enVencType = VENC_VIDEO_ENCODER};
    CALL(AX_VENC_Init, &mod, 0, 0, 0);
    for (unsigned c = 0; c < 2; c++) {
        unsigned w = c ? 3840 : 1920, h = c ? 2160 : 1080;
        AX_VENC_CHN_ATTR_S attr = {0};
        attr.stVencAttr.enType = PT_H264;
        attr.stVencAttr.u32MaxPicWidth = w;
        attr.stVencAttr.u32MaxPicHeight = h;
        attr.stVencAttr.u32PicWidthSrc = w;
        attr.stVencAttr.u32PicHeightSrc = h;
        attr.stVencAttr.u32CropWidth = w;
        attr.stVencAttr.u32CropHeight = h;
        attr.stVencAttr.u32BufSize = 2 * w * h;
        attr.stVencAttr.enProfile = VENC_H264_MAIN_PROFILE;
        attr.stVencAttr.enLevel = VENC_H264_LEVEL_5_2;
        attr.stVencAttr.enLinkMode = AX_NONLINK_MODE;
        attr.stVencAttr.u8InFifoDepth = 2;
        attr.stVencAttr.u8OutFifoDepth = 2;
        attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
        attr.stRcAttr.s32FirstFrameStartQp = -1;
        attr.stRcAttr.stH264Cbr = (AX_VENC_H264_CBR_S){.u32Gop = 30,
                                                       .u32StatTime = 1,
                                                       .u32SrcFrameRate = 30,
                                                       .fr32DstFrameRate = 30,
                                                       .u32BitRate = c ? 24000 : 4000,
                                                       .u32MaxQp = 51,
                                                       .u32MinQp = 20,
                                                       .u32MaxIQp = 51,
                                                       .u32MinIQp = 20,
                                                       .u32MaxIprop = 10,
                                                       .u32MinIprop = 1,
                                                       .s32IntraQpDelta = -2};
        CALL(AX_VENC_CreateChn, c, &attr, 0, 0);
        enc_created[c] = 1;
        AX_VENC_RECV_PIC_PARAM_S recv = {.s32RecvPicNum = -1};
        CALL(AX_VENC_StartRecvFrame, c, &recv, 0, 0);
        enc_started[c] = 1;
    }
    if (framed) {
        struct ca_z1_native_header hello={CA_Z1_NATIVE_OVERLAY_MAGIC,0,CA_Z1_NATIVE_OVERLAY_READY,0,0};
        if (fwrite(&hello,1,sizeof(hello),out[0])!=sizeof(hello)) goto cleanup;
    }
    if (framed && !pthread_create(&exposure_thread,NULL,exposure_poll,out[0])) exposure_started=true;
    struct encoder_worker workers[2] = {0};
    pthread_t encoders[2];
    unsigned started = 0;
    for (unsigned c = 0; c < 2; c++) {
        workers[c] =
            (struct encoder_worker){.channel = c, .limit = limit, .out = out[c], .framed = framed};
        if (pthread_create(&encoders[c], NULL, encode, &workers[c])) {
            stopped = true;
            break;
        }
        started++;
    }
    for (unsigned c = 0; c < started; c++)
        pthread_join(encoders[c], NULL);
    for (unsigned c = 0; c < 2; c++) {
        frames[c] = workers[c].frames;
        bytes[c] = workers[c].bytes;
    }
    result = started == 2 && !workers[0].failed && !workers[1].failed &&
                     ((frames[0] == limit && frames[1] == limit) || (framed && stopped))
                 ? 0
                 : 1;

cleanup:
    stopped = 1;
    if (exposure_started) pthread_join(exposure_thread,NULL);
    for (unsigned c = 0; c < 2; c++) {
        if (enc_started[c])
            call("AX_VENC_StopRecvFrame", c, 0, 0, 0);
        if (enc_created[c])
            call("AX_VENC_DestroyChn", c, 0, 0, 0);
    }
    if (stream_on)
        call("AX_VIN_StreamOff", 0, 0, 0, 0);
    if (dev_enabled)
        call("AX_VIN_DisableDev", 0, 0, 0, 0);
    if (vin_started)
        call("AX_VIN_Stop", 0, 0, 0, 0);
    if (thread_started)
        pthread_join(isp_thread, NULL);
    if (isp_open) {
        call("AX_ISP_UnRegisterAeLibCallback", 0, 0, 0, 0);
        call("AX_ISP_UnRegisterAwbLibCallback", 0, 0, 0, 0);
        call("AX_ISP_ALG_AeUnRegisterSensor", 0, 0, 0, 0);
        call("AX_ISP_Close", 0, 0, 0, 0);
    }
    if (created) {
        call("AX_VIN_UnRegisterSensor", 0, 0, 0, 0);
        call("AX_VIN_CloseSnsClk", 0, 0, 0, 0);
        call("AX_VIN_Destory", 0, 0, 0, 0);
    }
    call("AX_VIN_Deinit", 0, 0, 0, 0);
    call("AX_MIPI_RX_DeInit", 0, 0, 0, 0);
    call("AX_POOL_Exit", 0, 0, 0, 0);
    for (unsigned c = 0; c < (framed ? 1U : 2U); c++)
        if (out[c] && fclose(out[c]))
            result = 1;
    printf("RESULT frames=%u,%u bytes=%zu,%zu result=%d temp=%d\n", frames[0], frames[1], bytes[0],
           bytes[1], result, temp());
    return result;
}
