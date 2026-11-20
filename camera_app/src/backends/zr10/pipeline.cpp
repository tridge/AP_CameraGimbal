#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include "../siyi/tracking_frame.h"
#include "mi_api.h"
#include "camera_app/log.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The stock kernel modules and sensor driver must already be loaded. All MI
 * resources belong to this process; the launcher excludes sycamera. */
#define JPEG_CHN 3U
#define CHANNELS 4U
static struct mi_api mi;
static bool tracking_port;
static CA_MI_TrackingFrames tracking_frames;
bool ca_zr10_tracking_available() { return tracking_frames.available(); }
bool ca_zr10_tracking_frame(ca_tracking_frame &frame) { return tracking_frames.read(frame); }
static int (*get_af_stats)(unsigned, void *);
static pthread_mutex_t focus_lock=PTHREAD_MUTEX_INITIALIZER;
static uint8_t focus_payload[27];
static bool focus_fresh;
static int hardware_lock=-1;
static void *handles[9];
static unsigned nhandles;
static bool sys_up,snr_up,vif_up,vif_port,vpe_created,vpe_up,front_bound;
static bool port_up[CHANNELS],created[CHANNELS],bound[CHANNELS],receiving[CHANNELS];
static int fds[CHANNELS]={-1,-1,-1,-1};
static unsigned devices[CHANNELS];
static uint64_t first_pts[CHANNELS],last_pts[CHANNELS];
static bool have_pts[CHANNELS],inverted;
static struct ca_zr10_pipeline_config cfg;
static i6_sys_bind vif={I6_SYS_MOD_VIF,0,0,0},vpe={I6_SYS_MOD_VPE,0,0,0};
#define CHECK(call) do { int r_=(call); if(r_) { ca_log("%s = 0x%x",#call,r_); errno=EIO;return -1; } } while(0)

static int claim_hardware(void)
{
    hardware_lock=open("/tmp/zr10-probes.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(hardware_lock<0)return -1;
    if(flock(hardware_lock,LOCK_EX|LOCK_NB))return -1;
    DIR *proc=opendir("/proc");if(!proc)return -1;
    struct dirent *entry;int busy=0;
    while(!busy && (entry=readdir(proc))) {
        char *end;long pid=strtol(entry->d_name,&end,10);
        if(*end || pid<=0 || pid==getpid())continue;
        char path[128],name[64]= {};
        snprintf(path,sizeof(path),"/proc/%ld/comm",pid);
        FILE *file=fopen(path,"r");
        if(file) { (void)fgets(name,sizeof(name),file);fclose(file); }
        if(!strncmp(name,"sycamera",8) || !strcmp(name,"camera-app\n")) { busy=1;break; }
        snprintf(path,sizeof(path),"/proc/%ld/fd",pid);
        DIR *fds_dir=opendir(path);
        if(!fds_dir) { if(errno!=ENOENT && errno!=ESRCH)busy=1;continue; }
        struct dirent *de;
        while((de=readdir(fds_dir))) {
            long fd=strtol(de->d_name,&end,10);if(*end || fd<0)continue;
            char target[PATH_MAX];snprintf(path,sizeof(path),"/proc/%ld/fd/%ld",pid,fd);
            ssize_t n=readlink(path,target,sizeof(target)-1);
            if(n<0) { if(errno!=ENOENT && errno!=ESRCH)busy=1;continue; }
            target[n]=0;
            if(!strncmp(target,"/dev/mi_",8) || !strncmp(target,"/dev/isp",8) ||
               !strcmp(target,"/dev/ttyS1")) {
                ca_log("ZR10 hardware owner pid=%ld device=%s",pid,target);
                busy=1;break;
            }
        }
        closedir(fds_dir);
    }
    closedir(proc);
    if(busy) { ca_log("ZR10 hardware is owned by another process");errno=EBUSY;return -1; }
    return 0;
}

static int load_sdk(void)
{
    const char *libs[]={"libcam_os_wrapper.so","libmi_sys.so","libmi_sensor.so",
        "libmi_vif.so","libispalgo.so","libcus3a.so","libmi_isp.so",
        "libmi_vpe.so","libmi_venc.so"};
    for(unsigned i=0;i<9;i++) {
        handles[nhandles]=dlopen(libs[i],RTLD_LAZY|RTLD_GLOBAL);
        if(!handles[nhandles]) { ca_log("%s: %s",libs[i],dlerror());errno=ENOENT;return -1; }
        nhandles++;
    }
    /* Resolve circular ISP/VPE imports after all libraries have been loaded. */
    for(unsigned i=0;i<9;i++) {
        void *h=dlopen(libs[i],RTLD_NOW|RTLD_GLOBAL);
        if(!h) { ca_log("%s: %s",libs[i],dlerror());errno=ENOENT;return -1; }
        dlclose(h);
    }
#define LOAD(lib,name,args) mi.name=(decltype(mi.name))dlsym(RTLD_DEFAULT,#name); \
    if(!mi.name) { ca_log("missing %s",#name);errno=ENOENT;return -1; }
    MI_FUNCTIONS(LOAD)
    get_af_stats=(int (*)(unsigned int, void*))(dlsym(RTLD_DEFAULT,"MI_ISP_CUS3A_GetAFStats"));
    if(!get_af_stats) { ca_log("missing MI_ISP_CUS3A_GetAFStats");errno=ENOENT;return -1; }
#undef LOAD
    return 0;
}

static void encoder_close(unsigned ch)
{
    i6_sys_bind source={I6_SYS_MOD_VPE,0,0,ch==JPEG_CHN ? CA_ZR10_MAIN_VENC:ch};
    i6_sys_bind dest={I6_SYS_MOD_VENC,devices[ch],ch,0};
#define CLEAN(call) do { int r_=(call); if(r_)ca_log("cleanup %s = 0x%x",#call,r_); } while(0)
    if(receiving[ch]) { CLEAN(mi.MI_VENC_StopRecvPic(ch));receiving[ch]=false; }
    if(fds[ch]>=0) { CLEAN(mi.MI_VENC_CloseFd(ch));fds[ch]=-1; }
    if(bound[ch]) { CLEAN(mi.MI_SYS_UnBindChnPort(&source,&dest));bound[ch]=false; }
    if(created[ch]) { CLEAN(mi.MI_VENC_DestroyChn(ch));created[ch]=false; }
    if(port_up[ch]) { CLEAN(mi.MI_VPE_DisablePort(0,ch));port_up[ch]=false; }
    have_pts[ch]=false;
}

void ca_zr10_pipeline_close(void)
{
    tracking_frames={};
    if(tracking_port) { (void)mi.MI_VPE_DisablePort(0,3); tracking_port=false; }
    for(unsigned i=CHANNELS;i>0;i--)encoder_close(i-1);
    if(front_bound) { CLEAN(mi.MI_SYS_UnBindChnPort(&vif,&vpe));front_bound=false; }
    if(vpe_up) { CLEAN(mi.MI_VPE_StopChannel(0));vpe_up=false; }
    if(vpe_created) { CLEAN(mi.MI_VPE_DestroyChannel(0));vpe_created=false; }
    if(vif_port) { CLEAN(mi.MI_VIF_DisableChnPort(0,0));vif_port=false; }
    if(vif_up) { CLEAN(mi.MI_VIF_DisableDev(0));vif_up=false; }
    if(snr_up) { CLEAN(mi.MI_SNR_Disable(0));snr_up=false; }
    if(sys_up) { CLEAN(mi.MI_SYS_Exit());sys_up=false; }
    while(nhandles)dlclose(handles[--nhandles]);
    memset(&mi,0,sizeof(mi));inverted=false;get_af_stats=NULL;
    pthread_mutex_lock(&focus_lock);focus_fresh=false;pthread_mutex_unlock(&focus_lock);
    if(hardware_lock>=0) { close(hardware_lock);hardware_lock=-1; }
}

static int sensor_start(void)
{
    unsigned count=0;int profile=-1;
    CHECK(mi.MI_SNR_SetPlaneMode(0,0));
    CHECK(mi.MI_SNR_QueryResCount(0,&count));
    if(count>256) { errno=EPROTO;return -1; }
    for(unsigned i=0;i<count;i++) {
        i6_snr_res r= {};
        CHECK(mi.MI_SNR_GetRes(0,i,&r));
        if(r.crop.width==2560 && r.crop.height==1440 && r.maxFps>=30)profile=i;
    }
    if(profile<0) { errno=ENODEV;return -1; }
    CHECK(mi.MI_SNR_SetRes(0,profile));
    CHECK(mi.MI_SNR_SetFps(0,CA_ZR10_FRAME_RATE));
    CHECK(mi.MI_SNR_Enable(0));snr_up=true;
    i6_snr_pad pad= {};i6_snr_plane plane= {};
    CHECK(mi.MI_SNR_GetPadInfo(0,&pad));
    CHECK(mi.MI_SNR_GetPlaneInfo(0,0,&plane));
    if(pad.intf!=I6_INTF_MIPI || plane.capt.width!=2560 || plane.capt.height!=1440 ||
       plane.bayer!=I6_BAYER_GR || plane.precision!=I6_PREC_10BPP) {
        ca_log("unexpected ZR10 sensor mode");errno=EPROTO;return -1;
    }
    i6_common_pixfmt raw=(i6_common_pixfmt)(I6_PIXFMT_RGB_BAYER+plane.precision*I6_BAYER_END+plane.bayer);
    zr10_vif_dev dev={.base={.intf=pad.intf,.work=I6_VIF_WORK_RGB_REALTIME,
        .hdr=I6_HDR_OFF,.edge=I6_EDGE_DOUBLE,.input=pad.intfAttr.mipi.input},.multiDevMap=1};
    CHECK(mi.MI_VIF_SetDevAttr(0,&dev));
    CHECK(mi.MI_VIF_EnableDev(0));vif_up=true;
    i6_vif_port vp={};
    vp.capt = plane.capt;
    vp.dest = {2560,1440};
    vp.field = 3;
    vp.pixFmt = raw;
    vp.frate = I6_VIF_FRATE_FULL;
    vp.frameLineCnt = 1440;
    CHECK(mi.MI_VIF_SetChnPortAttr(0,0,&vp));
    CHECK(mi.MI_VIF_EnableChnPort(0,0));vif_port=true;
    i6_vpe_chn vc={};
    vc.capt = {2560,1440};
    vc.pixFmt = raw;
    vc.hdr = I6_HDR_OFF;
    vc.sensor = I6_VPE_SENS_ID0;
    vc.mode = I6_VPE_MODE_REALTIME;
    CHECK(mi.MI_VPE_CreateChannel(0,&vc));vpe_created=true;
    i6_vpe_para param={};
    param.hdr = I6_HDR_OFF;
    param.level3DNR = 1;
    CHECK(mi.MI_VPE_SetChannelParam(0,&param));
    CHECK(mi.MI_VPE_StartChannel(0));vpe_up=true;
    CHECK(mi.MI_SYS_BindChnPort2(&vif,&vpe,30,30,I6_SYS_LINK_REALTIME,0));front_bound=true;
    return 0;
}

static int encoder_open(unsigned ch,const struct ca_zr10_stream *s,bool jpeg)
{
    i6_vpe_port port={.output={(short unsigned int)(s->width),(short unsigned int)(s->height)},.mirror=inverted,.flip=inverted,
        .pixFmt=I6_PIXFMT_YUV420SP};
    /* JPEG has the main stream's dimensions. Share its established VPE
     * output, as on A8, instead of creating a transient scaler output. */
    if(!jpeg) {
        CHECK(mi.MI_VPE_SetPortMode(0,ch,&port));
        CHECK(mi.MI_VPE_EnablePort(0,ch));port_up[ch]=true;
    }
    i6_venc_chn attr= {};
    attr.attrib.codec=jpeg ? I6_VENC_CODEC_MJPG :
        s->codec==CA_VIDEO_H264 ? I6_VENC_CODEC_H264:I6_VENC_CODEC_H265;
    if(jpeg) {
        attr.attrib.mjpg=(i6_venc_attr_mjpg){.maxWidth=s->width,.maxHeight=s->height,
            .bufSize=s->width*s->height*2,.byFrame=1,.width=s->width,.height=s->height};
        attr.rate.mode=I6_VENC_RATEMODE_MJPGQP;
        attr.rate.mjpgQp=(i6_venc_rate_mjpgqp){.fpsNum=30,.fpsDen=1,.quality=cfg.jpeg_quality};
    } else {
        attr.attrib.h264=(i6_venc_attr_h26x){.maxWidth=s->width,.maxHeight=s->height,
            .bufSize=s->width*s->height,.profile=(unsigned)(s->codec==CA_VIDEO_H264 ? 1:0),
            .byFrame=1,.width=s->width,.height=s->height};
        attr.rate.mode=s->codec==CA_VIDEO_H264 ? I6_VENC_RATEMODE_H264CBR:I6_VENC_RATEMODE_H265CBR;
        attr.rate.h264Cbr=(i6_venc_rate_h26xcbr){.gop=30,.statTime=1,.fpsNum=30,
            .fpsDen=1,.bitrate=s->bit_rate_kbps*1000};
    }
    CHECK(mi.MI_VENC_CreateChn(ch,&attr));created[ch]=true;
    CHECK(mi.MI_VENC_GetChnDevid(ch,&devices[ch]));
    i6_sys_bind source={I6_SYS_MOD_VPE,0,0,ch==JPEG_CHN ? CA_ZR10_MAIN_VENC:ch};
    i6_sys_bind dest={I6_SYS_MOD_VENC,devices[ch],ch,0};
    CHECK(mi.MI_SYS_BindChnPort2(&source,&dest,30,30,I6_SYS_LINK_FRAMEBASE,0));bound[ch]=true;
    CHECK(mi.MI_VENC_StartRecvPic(ch));receiving[ch]=true;
    fds[ch]=mi.MI_VENC_GetFd(ch);
    if(fds[ch]<0) { errno=EIO;return -1; }
    return 0;
}

int ca_zr10_pipeline_open(const struct ca_zr10_pipeline_config *config)
{
    if(!config || nhandles) { errno=EINVAL;return -1; }
    for(unsigned i=0;i<CA_ZR10_VENC_COUNT;i++) {
        const struct ca_zr10_stream *s=&config->streams[i];
        if(!((s->width==1280 && s->height==720) || (s->width==1920 && s->height==1080) ||
             (s->width==2560 && s->height==1440)) || (s->codec!=CA_VIDEO_H264 && s->codec!=CA_VIDEO_H265)) {
            errno=EINVAL;return -1;
        }
    }
    cfg=*config;
#ifndef CA_ZR10_FAKE_SDK
    if(claim_hardware())goto fail;
#endif
    if(load_sdk())goto fail;
    if(mi.MI_SYS_Init()) { errno=EIO;goto fail; }sys_up=true;
    if(sensor_start())goto fail;
    for(unsigned i=0;i<CA_ZR10_VENC_COUNT;i++)if(encoder_open(i,&cfg.streams[i],false))goto fail;
    {
        i6_vpe_port port={.output={320,180},.mirror=0,.flip=0,.pixFmt=I6_PIXFMT_YUV420SP};
        if(mi.MI_VPE_SetPortMode(0,3,&port)==0 && mi.MI_VPE_EnablePort(0,3)==0) {
            tracking_port=true;
            if(!tracking_frames.open(I6_SYS_MOD_VPE,0,false)) ca_log("ZR10 tracking frame depth unavailable");
        } else ca_log("ZR10 tracking VPE output unavailable");
    }
    return 0;
fail:
    ca_zr10_pipeline_close();return -1;
}

int ca_zr10_venc_fd(unsigned ch) { return ch<CHANNELS ? fds[ch]:-1; }
int ca_zr10_venc_request_idr(unsigned ch)
{
    if(ch>=CA_ZR10_VENC_COUNT || !receiving[ch]) { errno=EINVAL;return -1; }
    CHECK(mi.MI_VENC_RequestIdr(ch,1));return 0;
}
int ca_zr10_load_isp_bin(const char *path)
{
    CHECK(mi.MI_ISP_API_CmdLoadBinFile(0,(char *)path,1234));
    for(unsigned i=0;i<CA_ZR10_VENC_COUNT;i++)(void)ca_zr10_venc_request_idr(i);
    return 0;
}

int ca_zr10_venc_get(unsigned ch,uint8_t **data,size_t *length,uint64_t *pts_us)
{
    if(ch>=CHANNELS || !data || !length || !pts_us || !receiving[ch]) { errno=EINVAL;return -1; }
    *data=NULL;*length=0;
    i6_venc_stat stat= {};CHECK(mi.MI_VENC_Query(ch,&stat));
    if(!stat.curPacks)return 0;
    if(stat.curPacks>256) { errno=EPROTO;return -1; }
    i6_venc_pack *packs=(i6_venc_pack*)(calloc(stat.curPacks,sizeof(*packs)));
    if(!packs)return -1;
    i6_venc_strm stream={.packet=packs,.count=stat.curPacks};
    int r=mi.MI_VENC_GetStream(ch,&stream,0);
    if(r) { free(packs);errno=EIO;return -1; }
    size_t total=0, offset=0;uint8_t *buffer=NULL;
    if(!stream.count || stream.count>stat.curPacks)goto release;
    for(unsigned i=0;i<stream.count;i++) {
        if(!packs[i].data || packs[i].offset>packs[i].length ||
           packs[i].length>16U*1024*1024-total)goto release;
        total+=packs[i].length-packs[i].offset;
    }
    if(!total)goto release;
    buffer=(uint8_t*)(malloc(total));if(!buffer)goto release;
    offset=0;
    for(unsigned i=0;i<stream.count;i++) {
        size_t n=packs[i].length-packs[i].offset;
        memcpy(buffer+offset,packs[i].data+packs[i].offset,n);offset+=n;
    }
release:
    {
        uint64_t pts=packs[0].timestamp;
        r=mi.MI_VENC_ReleaseStream(ch,&stream);free(packs);
        if(r || !buffer) { free(buffer);errno=EIO;return -1; }
        if(have_pts[ch] && pts<last_pts[ch]) { free(buffer);errno=EPROTO;return -1; }
        if(!have_pts[ch]) { first_pts[ch]=pts;have_pts[ch]=true; }
        last_pts[ch]=pts;
        *data=buffer;*length=total;*pts_us=pts-first_pts[ch];
        return 1;
    }
}

int ca_zr10_capture_jpeg(uint8_t **jpeg,size_t *length)
{
    int result=-1;
    if(!jpeg || !length || !vpe_up) { errno=EINVAL;return -1; }
    *jpeg=NULL;*length=0;
    /* Separate, transient JPEG encoder; video channels continue running. */
    if(encoder_open(JPEG_CHN,&cfg.streams[CA_ZR10_MAIN_VENC],true))goto out;
    for(unsigned i=0;i<30;i++) {
        struct pollfd fd={.fd=fds[JPEG_CHN],.events=POLLIN};
        int r=poll(&fd,1,100);
        if(r<0 && errno==EINTR)continue;
        if(r<0)break;
        if(fd.revents&POLLIN) {
            uint64_t pts;
            r=ca_zr10_venc_get(JPEG_CHN,jpeg,length,&pts);
            if(r>0) { result=0;break; }
            if(r<0)break;
        }
    }
out:
    encoder_close(JPEG_CHN);
    if(result)errno=EIO;
    return result;
}

int ca_zr10_set_inverted(bool value)
{
    if(!vpe_up) { errno=EINVAL;return -1; }
    for(unsigned i=0;i<CA_ZR10_VENC_COUNT;i++) {
        i6_vpe_port port={.output={(short unsigned int)(cfg.streams[i].width),(short unsigned int)(cfg.streams[i].height)},
            .mirror=value,.flip=value,.pixFmt=I6_PIXFMT_YUV420SP};
        int r=mi.MI_VPE_SetPortMode(0,i,&port);
        if(r) {
            ca_log("orientation port %u = 0x%x",i,r);
            /* Keep every successfully changed port at the previous orientation. */
            for(unsigned j=0;j<i;j++) {
                i6_vpe_port old={.output={(short unsigned int)(cfg.streams[j].width),(short unsigned int)(cfg.streams[j].height)},
                    .mirror=inverted,.flip=inverted,.pixFmt=I6_PIXFMT_YUV420SP};
                CLEAN(mi.MI_VPE_SetPortMode(0,j,&old));
            }
            errno=EIO;return -1;
        }
    }
    if(tracking_port) {
        i6_vpe_port port={.output={320,180},.mirror=value,.flip=value,.pixFmt=I6_PIXFMT_YUV420SP};
        if(mi.MI_VPE_SetPortMode(0,3,&port)) { tracking_frames={}; ca_log("ZR10 tracking orientation unavailable"); }
    }
    inverted=value;return 0;
}

/* The stock AF callback forwards the first of 16 432-byte ISP regions to the
 * lens MCU. Fields are five 40/32-bit accumulators and a 24-bit pixel count.
 * MI_ISP_CUS3A_GetAFStats declares an output size of 0x1b00 in the stock SDK. */
void ca_zr10_sample_focus(void)
{
    uint8_t stats[6912];
    if(!get_af_stats || get_af_stats(0,stats))return;
    static const unsigned offset[]={0,80,160,224,304,384};
    static const unsigned length[]={5,5,4,5,5,3};
    pthread_mutex_lock(&focus_lock);
    unsigned out=0;
    for(unsigned i=0;i<6;i++) {
        memcpy(focus_payload+out,stats+offset[i],length[i]);out+=length[i];
    }
    focus_fresh=true;
    pthread_mutex_unlock(&focus_lock);
}

int ca_zr10_take_focus(uint8_t payload[27])
{
    pthread_mutex_lock(&focus_lock);
    int fresh=focus_fresh;
    if(fresh) { memcpy(payload,focus_payload,27);focus_fresh=false; }
    pthread_mutex_unlock(&focus_lock);
    return fresh;
}
