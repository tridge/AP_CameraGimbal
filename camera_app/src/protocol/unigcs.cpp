#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/unigcs.h"
#include "camera_app/backend.h"
#include "camera_app/media.h"
#include "camera_app/mavlink_server.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/siyi.h"
#include "apcam/target.h"
#include "binlog_version.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#if __has_include(<net/if_dl.h>)
#include <net/if_dl.h>
#define CA_HAVE_IF_DL 1
#endif
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <new>
#include <stdlib.h>
#include <algorithm>

// UniGCS addresses the network camera endpoint as 34 even when the MCU
// transport uses 2C. Keep the network identity separate from the UART ID.
static constexpr uint8_t camera_address = 0x34;
#if APCAM_TARGET == APCAM_TARGET_MT11
static constexpr uint8_t product_id = 0x89;
#elif APCAM_TARGET == APCAM_TARGET_ZT30
static constexpr uint8_t product_id = 0x7b;
#elif APCAM_TARGET == APCAM_TARGET_A8
// Stock A8 cardv camera_sdk_get_ver_action returns 07 03 00 72.
// 73 is the gimbal product, also used as the public hardware-ID prefix.
static constexpr uint8_t product_id = 0x72;
#elif APCAM_TARGET == APCAM_TARGET_ZR10
static constexpr uint8_t product_id = 0x6b;
#endif

// One UniGCS control session owns the private gimbal route. Unlike camera
// requests, MCU replies do not echo sequence numbers and cannot be safely
// multiplexed between clients sharing source ID D0.
struct ca_unigcs {
    int listener=-1, discovery=-1, client=-1;
    sockaddr_in peer {};
    ca_private_parser parser {};
    ca_media *media=nullptr;
    ca_backend *backend=nullptr;
    ca_mavlink_server *mavlink=nullptr;
    bool manual=false;
    uint8_t source=0;
    bool long_format=false;
    uint16_t sequence=0;
    uint8_t output[CA_LONG_MAX_FRAME*8] {};
    size_t used=0, sent=0;
    uint64_t heartbeat=0, recording_since=0, zoom_tick=0, last_request=0;
    uint64_t multicast_join=0;
    uint64_t time_request_ms=0;
    bool time_pending=false;
    bool time_error_reported=false;
    int zoom_direction=0;
    bool zoom_reply=true;
    bool focusing=false;
    bool recording=false, reply_enabled=true;
    bool gimbal_requested[256] {};
    bool unsupported[256] {};
    bool tracking_enabled=false, tracking_reported=false, tracking_pending=false;
};

static uint64_t now_ms()
{
    timespec ts {}; clock_gettime(CLOCK_MONOTONIC,&ts);
    return uint64_t(ts.tv_sec)*1000+ts.tv_nsec/1000000;
}
static uint16_t u16(const uint8_t *p) { return p[0] | uint16_t(p[1])<<8; }
static void put16(uint8_t *p,unsigned v) { p[0]=v; p[1]=v>>8; }
// Zero is outside the valid module range and represents unavailable feedback.
// Native backends can report NaN before the first reply or after expiry.
static unsigned observed_zoom_tenths(const ca_media *media) {
    const float zoom=ca_media_zoom(media);
    return isfinite(zoom) && zoom>=1 && zoom<=6553.4f ? unsigned(zoom*10+0.5f) : 0;
}
static void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }

static void disconnect(ca_unigcs *s)
{
    ca_media_tracking_stop(s->media,CA_TRACK_OWNER_SIYI);
    s->tracking_enabled=s->tracking_reported=false;
    s->tracking_pending=false;
    if(APCAM_ZOOM_NATIVE_RATE && s->zoom_direction && s->backend)
        (void)ca_backend_set_zoom_rate(s->backend,0);
    if(s->focusing && s->media) (void)ca_media_manual_focus(s->media,0);
    s->focusing=false; s->zoom_direction=0;
    if(s->client>=0) close(s->client);
    s->client=-1; s->source=0; s->used=s->sent=0; s->long_format=false;
    s->time_pending=false; s->time_request_ms=0;
    memset(s->gimbal_requested,0,sizeof(s->gimbal_requested));
    ca_private_parser_init(&s->parser);
}
static void flush(ca_unigcs *s)
{
    while(s->client>=0 && s->sent<s->used) {
        ssize_t n=send(s->client,s->output+s->sent,s->used-s->sent,MSG_NOSIGNAL);
        if(n>0) { s->sent+=n; continue; }
        if(n<0 && errno==EINTR) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return;
        disconnect(s); return;
    }
    if(s->sent==s->used) s->sent=s->used=0;
}
static void queue(ca_unigcs *s,const uint8_t *p,size_t n)
{
    if(s->client<0) return;
    if(s->sent) {
        memmove(s->output,s->output+s->sent,s->used-s->sent);
        s->used-=s->sent; s->sent=0;
    }
    const int error=n>sizeof(s->output)-s->used ? -ENOBUFS : 0;
    ca_binlog_packet(true,s->long_format ? CA_PACKET_SIYI_LONG : CA_PACKET_MT11,CA_PACKET_TCP,ntohl(s->peer.sin_addr.s_addr),
                     ntohs(s->peer.sin_port),p,n,error);
    if(error) { disconnect(s); return; }
    memcpy(s->output+s->used,p,n); s->used+=n;
    flush(s);
}
static void reply(ca_unigcs *s,uint8_t cmd,const uint8_t *p,size_t n)
{
    if(!s->reply_enabled) return;
    uint8_t raw[CA_LONG_MAX_FRAME];
    size_t len=s->long_format ? ca_long_build(raw,sizeof(raw),2,s->sequence++,cmd,p,n) :
        ca_private_build(raw,sizeof(raw),0x0a,s->sequence++,camera_address,s->source,0x16,cmd,p,n);
    if(len) queue(s,raw,len);
}

static bool time_needed()
{
    // Same validity threshold as MAVLink SYSTEM_TIME. Do not step a clock
    // already set by the flight controller or the public SIYI SDK.
    timespec now {};
    return clock_gettime(CLOCK_REALTIME,&now)==0 && now.tv_sec<1788220800;
}

static void request_time(ca_unigcs *s)
{
    if(s->client<0 || !s->source) return;
    if(!time_needed()) { s->time_pending=false; s->time_error_reported=false; return; }
    const uint64_t now=now_ms();
    if(s->time_pending && now-s->time_request_ms<1000) return;
    const uint8_t wanted=1;
    uint8_t raw[CA_LONG_MAX_FRAME];
    // This is a camera-originated request, not an unsolicited ACK. Wait for
    // the client's first command to establish its framing and routing ID.
    const size_t len=s->long_format ? ca_long_build(raw,sizeof(raw),1,s->sequence++,0x91,&wanted,1) :
        ca_private_build(raw,sizeof(raw),0x09,s->sequence++,camera_address,s->source,0x16,0x91,&wanted,1);
    s->time_pending=true; s->time_request_ms=now;
    if(len) queue(s,raw,len);
}

static void receive_time(ca_unigcs *s,const ca_private_frame *f)
{
#if defined(CAMERA_APP_ZT30_RAM) || defined(CAMERA_APP_ZT30_DEV)
    (void)s;(void)f;return;
#else
    const bool long_format=f->raw_length>=20 && f->raw[0]==0x55;
    if(s->client<0 || !s->source || !s->time_pending || f->source!=s->source ||
       long_format!=s->long_format || f->destination!=camera_address || f->link!=0x16 ||
       f->command!=0x91 || (f->control!=0x08 && f->control!=0x09 && f->control!=0x0a)) return;
    if(!time_needed()) { s->time_pending=false; s->time_error_reported=false; return; }
    // Stock stores a uint16 year and five calendar bytes in an eight-byte
    // struct. Accept its padded form and the packed seven-byte form.
    const uint8_t *p=f->payload;
    if(f->payload_length!=7 && f->payload_length!=8) return;
    const unsigned year=u16(p);
    if(year<2026 || year>2099 || p[2]<1 || p[2]>12 || p[3]<1 || p[3]>31 ||
       p[4]>23 || p[5]>59 || p[6]>59) return;
    tm calendar {};
    calendar.tm_year=year-1900; calendar.tm_mon=p[2]-1; calendar.tm_mday=p[3];
    calendar.tm_hour=p[4]; calendar.tm_min=p[5]; calendar.tm_sec=p[6];
    // UniGCS sends local wall time despite the vendor handler's UTC name.
    // Match stock firmware's mktime conversion using the configured camera
    // timezone, which must match the GCS. Let libc determine daylight saving.
    calendar.tm_isdst=-1;
    const time_t epoch=mktime(&calendar);
    tm checked {};
    if(epoch<1788220800 || !localtime_r(&epoch,&checked) ||
       checked.tm_year!=int(year)-1900 || checked.tm_mon!=p[2]-1 || checked.tm_mday!=p[3] ||
       checked.tm_hour!=p[4] || checked.tm_min!=p[5] || checked.tm_sec!=p[6]) return;
    const timespec wanted {epoch,0};
    if(clock_settime(CLOCK_REALTIME,&wanted)==0) {
        s->last_request=now_ms();
        s->time_pending=false; s->time_error_reported=false;
        char zone[64];
        if(!strftime(zone,sizeof(zone),"%Z (UTC%z)",&checked)) strcpy(zone,"unknown");
        ca_log("system time set from UniGCS command=0x91 source=0x%02x peer=%s epoch=%lld timezone=%s",
               f->source,inet_ntoa(s->peer.sin_addr),static_cast<long long>(epoch),zone);
    } else if(!s->time_error_reported) {
        // Suppress repeated failures, including across client reconnects,
        // until a supported time source establishes a valid clock.
        s->time_error_reported=true;
        ca_log("system time setting from UniGCS failed: %s",strerror(errno));
    }
#endif
}

static void image_slots(ca_unigcs *s,uint8_t *p)
{
#if !APCAM_HAVE_THERMAL_VIDEO
    (void)s;
    p[0]=p[1]=0;
#else
#ifdef CAMERA_APP_ZT30_DEV
    if(!ca_media_frame_rate(s->media,true)) { p[0]=p[1]=0;return; }
#endif
    // MT11's visible scene is Zoom across both physical RGB sensors. Reporting
    // Wide at low zoom selects a different UniGCS UI which hides tracking.
    p[0]=ca_media_side_by_side(s->media) ? 3 : ca_media_thermal_main(s->media) ? 2 :
        (!APCAM_ZOOM_SELECTS_LENS && ca_media_lens(s->media)==CA_MEDIA_LENS_WIDE ? 1 : 0);
    p[1]=p[0]==2 ? 0 : 2;
#endif
}

// Apply through the same validated parameter metadata as MAVLink, then persist
// the one field so unrelated settings changed by the web UI are not overwritten.
static bool parameter(ca_unigcs *s,const char *name,int value)
{
    int index=ca_config_param_find(name);
    ca_config old=*ca_media_settings(s->media), next=old;
    if(index<0 || ca_config_param_assign(&next,index,value)<0) return false;
    const bool palette=strcmp(name,"THERMAL_PALETTE")==0;
    uint8_t old_palette=0;
    if(palette && (ca_media_get_thermal_palette(s->media,&old_palette)<0 ||
                   ca_media_set_thermal_palette(s->media,value)<0)) return false;
    if(ca_media_configure(s->media,&next)<0) {
        if(palette) (void)ca_media_set_thermal_palette(s->media,old_palette);
        return false;
    }
    const char *path=getenv("CAMERA_APP_CONFIG");
    if(!path || !*path) path=CA_CONFIG_DEFAULT_PATH;
    if(ca_config_param_save(&next,path,index,value)<0) {
        (void)ca_media_configure(s->media,&old);
        if(palette) (void)ca_media_set_thermal_palette(s->media,old_palette);
        return false;
    }
    return true;
}

static void encoder(ca_unigcs *s,uint8_t id,uint8_t *out)
{
    const ca_config &cfg=*ca_media_settings(s->media);
    unsigned width,height;
    ca_video_resolution_size(id==0 ? cfg.recording_resolution : id==1 ? cfg.main_resolution : cfg.sub_resolution,&width,&height);
    bool thermal=APCAM_HAVE_THERMAL_VIDEO && (id==1 ? ca_media_thermal_main(s->media) : id==2 && !ca_media_thermal_main(s->media));
    if(id!=0 && thermal) { width=APCAM_THERMAL_STREAM_WIDTH; height=APCAM_THERMAL_STREAM_HEIGHT; }
    out[0]=id;
    out[1]=id==0 ? 1 : ((id==1 ? cfg.main_codec : cfg.sub_codec)==CA_VIDEO_H265 ? 2 : 1);
#ifdef CAMERA_APP_SITL
    // SITL routes the dedicated thermal encoder to either RTSP stream.
    // That source and the visible substream both use the substream codec.
    if(id!=0 && (thermal || ca_media_thermal_main(s->media)))
        out[1]=cfg.sub_codec==CA_VIDEO_H265 ? 2 : 1;
#endif
    put16(out+2,width); put16(out+4,height);
    put16(out+6,width>=3840 || height>=2160 ? 12000 : width>=1920 || height>=1080 ? 4096 : 2048);
#ifdef CAMERA_APP_ZT30_DEV
    put16(out+6,id==0?12000:thermal?1000:1570);
#endif
    out[8]=ca_media_frame_rate(s->media,thermal);
}

static void camera_request(ca_unigcs *s,const ca_private_frame *f)
{
    const uint8_t *p=f->payload;
    const unsigned n=f->payload_length;
    uint8_t out[32] {};
    ca_config cfg=*ca_media_settings(s->media);
    switch(f->command) {
    case 0xf0: // Client location/heartbeat, not a request for a fabricated ACK.
        if(n==6) return;
        break;
    case 0x94:
        if(n) break;
        // UniGCS formats these bytes as vMAJOR.MINOR.PATCH. The camera
        // model is a separate byte; neither field can carry an "AP-" prefix.
        out[0]=CA_FIRMWARE_VERSION_PATCH;
        out[1]=CA_FIRMWARE_VERSION_MINOR;
        out[2]=CA_FIRMWARE_VERSION_MAJOR;
        out[3]=product_id;
        // AI type 87 exposes region tracking without the classifier/settings UI.
        if(ca_media_tracking_available(s->media)) { out[6]=1; out[7]=0x87; }
        reply(s,0x94,out,out[7]?8:4); return;
    case 0x80:
        if(n) break;
        out[0]=ca_media_recording(s->media) ? 1 : 0;
        if(out[0] && s->recording_since) put32(out+1,(now_ms()-s->recording_since)/1000);
        reply(s,0x80,out,5); return;
    case 0x81:
        if(n!=1 || p[0]>1) break;
        out[0]=p[0]; out[1]=ca_media_set_recording(s->media,p[0]!=0)==0;
        reply(s,0x81,out,2); return;
    case 0x85:
        if(n) break;
        // UniGCS knows a boolean; retain the MAVLink armed-only policy unless set.
        out[0]=cfg.autorecord==CA_AUTORECORD_ENABLED;
        reply(s,0x85,out,1); return;
    case 0x86:
        if(n!=1 || p[0]>1) break;
        if(!parameter(s,"REC_AUTOSTART",p[0])) break;
        reply(s,0x85,p,1); return;
    case 0x83:
        if(n!=1 || p[0]>2) break;
        encoder(s,p[0],out); reply(s,0x83,out,9); return;
    case 0xe1:
        if(n!=1 || p[0]!=10) break;
        out[0]=10; out[1]=cfg.brightness; out[2]=cfg.saturation; out[3]=cfg.contrast;
        out[4]=cfg.white_balance; out[5]=cfg.iso;
        // V1.0.5 repeats ISO in byte 6, despite EV being independently writable.
        out[6]=cfg.iso; out[7]=cfg.metering; out[8]=cfg.shutter;
        reply(s,0xe1,out,9); return;
    case 0xe3: {
        if(n!=3 || p[0]>7 || p[1]!=10) break;
        static const char *names[]={"IMG_BRIGHTNESS","IMG_SATURATION","IMG_CONTRAST",
            "IMG_WHITE_BAL","IMG_EXPOSURE","IMG_ISO","IMG_METERING","IMG_SHUTTER"};
        out[0]=p[0]; out[1]=10;
        out[2]=APCAM_HAVE_IMAGE_CONTROLS && parameter(s,names[p[0]],int8_t(p[2]));
        reply(s,0xe3,out,3); return;
    }
    case 0xe5:
        // Getter; AUTO is derived from shared exposure controls. A setter must
        // reproduce the stock auto/pro profile semantics before it is enabled.
        if(n!=1 || p[0]!=10) break;
        out[0]=(cfg.iso!=CA_ISO_AUTO || cfg.shutter!=CA_SHUTTER_AUTO);
        reply(s,0xe5,out,1); return;
    case 0x92:
        if(n) break;
        image_slots(s,out); reply(s,0x92,out,2); return;
    case 0x93: {
#if APCAM_HAVE_THERMAL_VIDEO
        if(n==2 && p[0]==3 && p[1]==0) {
            if(ca_media_set_side_by_side(s->media,true)<0) break;
            image_slots(s,out); reply(s,0x93,out,2); return;
        }
        if(n!=2 || !((p[0]<=1 && (p[1]==2 || p[1]==0)) || (p[0]==2 && p[1]==0))) break;
        const bool thermal=ca_media_thermal_main(s->media);
        int result=ca_media_set_thermal_main(s->media,p[0]==2);
        if(result==0 && p[0]!=2) result=ca_media_set_lens(s->media,p[0]==1 ? CA_MEDIA_LENS_WIDE : CA_MEDIA_LENS_ZOOM);
        if(result<0) { (void)ca_media_set_thermal_main(s->media,thermal); break; }
        image_slots(s,out); reply(s,0x93,out,2); return;
#else
        if(n!=2 || p[0]!=0 || p[1]!=0) break;
        image_slots(s,out); reply(s,0x93,out,2); return;
#endif
    }
    case 0xa4:
    case 0xbc:
        if(n) break;
        if((f->command==0xa4 ? ca_media_get_thermal_palette(s->media,out) :
                             ca_media_get_thermal_gain(s->media,out))<0) break;
        reply(s,f->command,out,1); return;
    case 0xa5:
        if(!APCAM_HAVE_THERMAL_PALETTE || n!=1 || p[0]>11 || p[0]==1) break;
        if(!parameter(s,"THERMAL_PALETTE",p[0])) break;
        reply(s,0xa5,p,1); return;
    case 0xbd:
        if(n!=1 || p[0]>1) break;
        if(ca_media_set_thermal_gain(s->media,p[0])<0) break;
        // Stock gain setter has no direct reply.
        return;
    case 0x97:
        if(n!=5 || p[0]!=1 || u16(p+1)>=1920 || u16(p+3)>=1080) break;
        out[0]=ca_media_autofocus(s->media,u16(p+1),u16(p+3))==0;
        reply(s,0x97,out,1); return;
    case 0x98:
        if(n!=1 || int8_t(p[0]) < -1 || int8_t(p[0])>1) break;
        if(APCAM_ZOOM_NATIVE_RATE && ca_backend_set_zoom_rate(s->backend,int8_t(p[0]))<0) break;
        if(!s->zoom_direction) s->zoom_tick=now_ms();
        s->zoom_direction=int8_t(p[0]); s->zoom_reply=s->reply_enabled;
        put16(out,observed_zoom_tenths(s->media));
        reply(s,0x98,out,2); return;
    case 0xc6:
        if(n && !(n==1 && p[0]==1)) break;
        out[0]=ca_media_capture_photo(s->media,cfg.photo_scope)==0;
        reply(s,0xc6,out,1); return;
    case 0x9f:
        // Legacy A8 UniGCS sends camera-mode 0 for a single photo, not C6/01.
        // Stock replies with mode 0, rather than C6's boolean success byte.
        // Other modes and the v3 command namespace are not aliases for capture.
        if(!s->long_format || n!=1 || p[0]!=0) break;
        if(ca_media_capture_photo(s->media,cfg.photo_scope)<0) {
            ca_log("UniGCS photo capture failed: %s",strerror(errno));
            return;
        }
        reply(s,0x9f,p,1); return;
    case 0xb4:
        if(n) break;
#ifdef CAMERA_APP_ZT30_DEV
        if(!ca_media_photo_available(s->media)) { out[0]=0;reply(s,0xb4,out,1);return; }
#endif
        out[0]=APCAM_HAVE_THERMAL_VIDEO ? (cfg.photo_scope==CA_PHOTO_SCOPE_ALL ? 0x0b : 0x08) : 1;
        reply(s,0xb4,out,1); return;
    case 0x99:
        if(n!=1 || (int8_t(p[0]) < -1 || int8_t(p[0])>1)) break;
        out[0]=ca_media_manual_focus(s->media,int8_t(p[0]))==0;
        if(out[0]) s->focusing=p[0]!=0;
        reply(s,0x99,out,1); return;
    case 0xd0:
        if(n!=1 || p[0]!=10) break;
        out[0]=10; put16(out+1,APCAM_ZOOM_CONTROL_MAX*10); put16(out+3,10);
#if APCAM_NUM_LENSES > 1
        put16(out+5,APCAM_LENS2_OPTICAL_ZOOM_MAX*10); put16(out+7,10);
#endif
        reply(s,0xd0,out,9); return;
    case 0xd1:
        if(n!=1 || p[0]!=10) break;
        out[0]=10; put16(out+1,observed_zoom_tenths(s->media));
        reply(s,0xd1,out,3); return;
    case 0xd2:
        if(n!=3 || p[0]!=10 || u16(p+1)<10 || u16(p+1)>unsigned(APCAM_ZOOM_CONTROL_MAX*10)) break;
        if(APCAM_ZOOM_NATIVE_RATE && s->zoom_direction)
            (void)ca_backend_set_zoom_rate(s->backend,0);
        s->zoom_direction=0;
        out[0]=p[0]; out[1]=ca_backend_set_zoom(s->backend,float(u16(p+1))*0.1f)==0;
        reply(s,0xd2,out,2); return;
    case 0xa2:
        if(n) break;
        out[0]=s->tracking_enabled;
        reply(s,0xa2,out,1); return;
    case 0xa3:
        if(n!=1 || p[0]>1) break;
        if(p[0] && !ca_media_tracking_available(s->media)) { out[1]=1; }
        else {
            s->tracking_enabled=p[0]!=0;
            if(!p[0]) ca_media_tracking_stop(s->media,CA_TRACK_OWNER_SIYI);
        }
        out[0]=s->tracking_enabled;
        reply(s,0xa3,out,2); return;
    case 0xaa: {
        if(n!=9 || p[0]>1) break;
        if(!p[0]) { ca_media_tracking_stop(s->media); out[0]=1; }
        else if(ca_mavlink_server_survey_owns_gimbal(s->mavlink)) out[0]=6;
        else if(!s->tracking_enabled) out[0]=2;
        else if(!ca_media_tracking_available(s->media)) out[0]=7;
        else if(ca_media_side_by_side(s->media)) out[0]=3;
        else if(s->manual) out[0]=6;
        else {
            unsigned width=0,height=0;
            uint8_t geometry[9]; encoder(s,1,geometry);
            width=u16(geometry+2); height=u16(geometry+4);
            const unsigned x0=u16(p+1),y0=u16(p+3),x1=u16(p+5),y1=u16(p+7);
            if(!width || !height || x0>width || x1>width || y0>height || y1>height) out[0]=0;
            else {
                ca_tracking_rect rect={float(std::min(x0,x1))/width,float(std::min(y0,y1))/height,
                    float(std::max(x0,x1))/width,float(std::max(y0,y1))/height};
                out[0]=ca_media_tracking_start(s->media,rect,CA_TRACK_OWNER_SIYI)==0?1:8;
                s->tracking_pending=out[0]==1;
            }
        }
        reply(s,0xaa,out,1); return;
    }
    case 0xd5:
        if(n!=1 || p[0]!=3) break;
        // Catalogue query: operation, model ID, class count, masks, and
        // NUL-terminated comma-separated names. No AI classes are available.
        out[0]=3;
        reply(s,0xd5,out,4); return;
    default:
        break;
    }
    if(!s->unsupported[f->command]) {
        s->unsupported[f->command]=true;
        ca_log("UniGCS unsupported/invalid camera command 16/%02x length=%u",f->command,n);
    }
}

static void request(void *opaque,const ca_private_frame *f)
{
    auto *s=static_cast<ca_unigcs *>(opaque);
    if(s->client<0) return;
    const bool long_format=f->raw_length>=20 && f->raw[0]==0x55;
    ca_binlog_packet(false,long_format ? CA_PACKET_SIYI_LONG : CA_PACKET_MT11,CA_PACKET_TCP,ntohl(s->peer.sin_addr.s_addr),
                     ntohs(s->peer.sin_port),f->raw,f->raw_length);
    if(f->source==0 || f->source==camera_address || f->source==0x2e) return;
    if(s->source && long_format!=s->long_format) return;
    if(s->source && f->source!=s->source) return;
    if(!((f->destination==camera_address && f->link==0x16) ||
         (f->destination==0x2e && f->link==0x11))) return;
    // Only a solicited time reply may use response framing. Other response
    // packets must never be dispatched as camera/gimbal commands.
    if(f->destination==camera_address && f->command==0x91 && s->source &&
       (f->control==0x08 || f->control==0x09 || f->control==0x0a)) {
        receive_time(s,f);
        return;
    }
    if(f->control!=0x09 && f->control!=0x08) return;
    s->source=f->source;
    s->long_format=long_format;
    s->last_request=now_ms();
    // Older network frames have a single command namespace. These two
    // A8 gimbal controls match native 11/9A and 11/9B payloads.
    ca_private_frame routed=*f;
    uint8_t routed_raw[CA_PRIVATE_MAX_FRAME];
    if(long_format && (f->command==0x9a || f->command==0x9b)) {
        routed.destination=0x2e; routed.link=0x11;
        routed.raw_length=ca_private_build(routed_raw,sizeof(routed_raw),routed.control,
            routed.sequence,routed.source,routed.destination,routed.link,
            routed.command,routed.payload,routed.payload_length);
        if(!routed.raw_length) return;
        routed.raw=routed_raw;
        f=&routed;
    }
    if(f->destination==camera_address) {
        s->reply_enabled=(f->control&1)!=0;
        camera_request(s,f);
        s->reply_enabled=true;
        return;
    }
    // Preserve web manual-control ownership even for unrecognised MCU commands.
    if(s->manual && !(f->payload_length==0 &&
       (f->command==0xa0 || f->command==0xb4 || f->command==0xc2))) return;
    // This is the MCU gimbal namespace: fail closed for unknown pointing
    // commands. Camera controls (zoom, palette, photos) were handled above.
    if(ca_mavlink_server_survey_owns_gimbal(s->mavlink) &&
       !(f->payload_length==0 && (f->command==0xa0 || f->command==0xb4 || f->command==0xc2)) &&
       f->command!=0xbb) return;
    s->gimbal_requested[f->command]=true;
    if(f->command==0x9a || f->command==0x9b) {
        ca_media_tracking_stop(s->media);
        ca_mavlink_server_suspend_gimbal(s->mavlink);
    }
    if(ca_backend_handle_private(s->backend,f)<0)
        ca_log("UniGCS gimbal forwarding failed: %s",strerror(errno));
}

void ca_unigcs_emit(void *opaque,const ca_private_frame *f)
{
    auto *s=static_cast<ca_unigcs *>(opaque);
    if(!s || s->client<0 || !s->source || f->source!=0x2e ||
       f->destination!=s->source || f->link!=0x11) return;
    if(!s->gimbal_requested[f->command] &&
       !(s->gimbal_requested[0xbb] && (f->command==0x89 || f->command==0xb0))) return;
    // A8/ZR10 UART replies use v2; the UniGCS network session uses v3.
    uint8_t raw[CA_LONG_MAX_FRAME];
    size_t size=s->long_format ? ca_long_build(raw,sizeof(raw),f->control&3,
        f->sequence,f->command,f->payload,f->payload_length) :
        ca_private_build(raw,sizeof(raw),f->control,f->sequence,
            f->source,f->destination,f->link,f->command,f->payload,f->payload_length);
    if(size) queue(s,raw,size);
}

static int bind_port(unsigned port,int type)
{
    int fd=socket(AF_INET,type,0);
    if(fd<0) return -1;
    if(fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) { close(fd); return -1; }
    // Do not share the discovery socket with the stock product_upgrade daemon.
    if(type==SOCK_STREAM) { int one=1; (void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)); }
    sockaddr_in a {}; a.sin_family=AF_INET; a.sin_port=htons(port); a.sin_addr.s_addr=INADDR_ANY;
    if(bind(fd,reinterpret_cast<sockaddr *>(&a),sizeof(a))<0) { int e=errno; close(fd); errno=e; return -1; }
    return fd;
}

static bool identity(in_addr local,uint8_t *out)
{
    ifaddrs *interfaces=nullptr;
    if(getifaddrs(&interfaces)<0) return false;
    bool found=false;
    out[0]=product_id;
    for(auto *i=interfaces;i;i=i->ifa_next) {
        if(!i->ifa_addr || i->ifa_addr->sa_family!=AF_INET || !i->ifa_netmask) continue;
        auto *a=reinterpret_cast<sockaddr_in *>(i->ifa_addr);
        if(a->sin_addr.s_addr!=local.s_addr) continue;
        auto *mask=reinterpret_cast<sockaddr_in *>(i->ifa_netmask);
        uint32_t broadcast=local.s_addr | ~mask->sin_addr.s_addr;
        memcpy(out+1,&local.s_addr,4); memcpy(out+5,&mask->sin_addr.s_addr,4); memcpy(out+9,&broadcast,4);
        bool have_mac=false;
#ifdef SIOCGIFHWADDR
        // Linux and Cygwin expose the interface MAC through this ioctl.
        // Cygwin has neither sockaddr_ll nor BSD's sockaddr_dl.
        int fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd>=0) {
            ifreq request {};
            strncpy(request.ifr_name,i->ifa_name,sizeof(request.ifr_name)-1);
            if(ioctl(fd,SIOCGIFHWADDR,&request)==0) {
                memcpy(out+13,request.ifr_hwaddr.sa_data,6);
                have_mac=true;
            }
            close(fd);
        }
#elif defined(CA_HAVE_IF_DL)
        for(auto *m=interfaces;m;m=m->ifa_next) {
            if(!m->ifa_addr || strcmp(i->ifa_name,m->ifa_name)) continue;
            if(m->ifa_addr->sa_family==AF_LINK) {
                auto *link=reinterpret_cast<sockaddr_dl *>(m->ifa_addr);
                if(link->sdl_alen==6) {
                    memcpy(out+13,LLADDR(link),6);
                    have_mac=true;
                }
            }
        }
#endif
#ifdef CAMERA_APP_SITL
        // Loopback and virtual adapters may have no Ethernet address. Give
        // SITL a stable locally administered identity instead of all zeros.
        static const uint8_t zero_mac[6] {};
        if(!have_mac || !memcmp(out+13,zero_mac,6)) {
            out[13]=0x02; out[14]=0;
            memcpy(out+15,&local.s_addr,4);
            have_mac=true;
        }
#endif
        found=have_mac; break;
    }
    freeifaddrs(interfaces); return found;
}
static void discover(ca_unigcs *s)
{
    // Bounded work so a flood cannot starve flight/gimbal telemetry.
    for(unsigned iteration=0;iteration<16;iteration++) {
        uint8_t raw[256]; sockaddr_in peer {};
        iovec iov {raw,sizeof(raw)};
        alignas(cmsghdr) char control[128] {};
        msghdr message {};
        message.msg_name=&peer; message.msg_namelen=sizeof(peer);
        message.msg_iov=&iov; message.msg_iovlen=1;
        message.msg_control=control; message.msg_controllen=sizeof(control);
        ssize_t n=recvmsg(s->discovery,&message,0);
        if(n<0) return;
        if(n!=14 || raw[0]!=0xaa || raw[1]!=0x09 || raw[2]!=3 || u16(raw+3)!=0 ||
           raw[5]!=ca_crc8_maxim(raw,5) || raw[9]!=camera_address || raw[10]!=0xf0 || raw[11]!=1 ||
           u16(raw+12)!=ca_crc16(raw,12)) continue;
        // Ask the routing table which local address reaches this client. This
        // avoids advertising the physical camera's default address in SITL.
        int route=socket(AF_INET,SOCK_DGRAM,0); if(route<0) continue;
        sockaddr_in local {}; socklen_t local_len=sizeof(local);
        int result=connect(route,reinterpret_cast<sockaddr *>(&peer),sizeof(peer));
        if(result==0) result=getsockname(route,reinterpret_cast<sockaddr *>(&local),&local_len);
        close(route); if(result<0) continue;
#ifdef __linux__
        // Multihomed SITL hosts must advertise and reply from the interface on
        // which discovery arrived, even if their default route differs.
        in_pktinfo received_info {};
        for(cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
            if(c->cmsg_level==IPPROTO_IP && c->cmsg_type==IP_PKTINFO &&
               c->cmsg_len>=CMSG_LEN(sizeof(received_info))) {
                memcpy(&received_info,CMSG_DATA(c),sizeof(received_info));
                if(received_info.ipi_spec_dst.s_addr) local.sin_addr=received_info.ipi_spec_dst;
            }
        }
#endif
        uint8_t payload[19] {}, response[33];
        if(!identity(local.sin_addr,payload)) continue;
        size_t size=ca_private_build(response,sizeof(response),0x0a,s->sequence++,camera_address,raw[8],0xf0,1,payload,sizeof(payload));
        ca_binlog_packet(false,CA_PACKET_MT11,CA_PACKET_UDP,ntohl(peer.sin_addr.s_addr),ntohs(peer.sin_port),raw,n);
        iov={response,size};
        message.msg_iov=&iov; message.msg_controllen=0;
#ifdef __linux__
        if(received_info.ipi_ifindex) {
            message.msg_controllen=CMSG_SPACE(sizeof(in_pktinfo));
            auto *c=CMSG_FIRSTHDR(&message);
            c->cmsg_level=IPPROTO_IP; c->cmsg_type=IP_PKTINFO;
            c->cmsg_len=CMSG_LEN(sizeof(in_pktinfo));
            received_info.ipi_addr.s_addr=0;
            memcpy(CMSG_DATA(c),&received_info,sizeof(received_info));
        }
#endif
        ssize_t sent=sendmsg(s->discovery,&message,0);
        ca_binlog_packet(true,CA_PACKET_MT11,CA_PACKET_UDP,ntohl(peer.sin_addr.s_addr),ntohs(peer.sin_port),response,size,sent==ssize_t(size) ? 0 : -errno);
    }
}

static void join_multicast(ca_unigcs *s)
{
    s->multicast_join=now_ms();
    ifaddrs *interfaces=nullptr;
    if(getifaddrs(&interfaces)!=0) return;
    for(auto *i=interfaces;i;i=i->ifa_next) {
        if(!i->ifa_addr || i->ifa_addr->sa_family!=AF_INET || !(i->ifa_flags&IFF_UP) || !(i->ifa_flags&IFF_MULTICAST)) continue;
        ip_mreq group {}; inet_pton(AF_INET,"224.0.0.1",&group.imr_multiaddr);
        group.imr_interface=reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr;
        if(setsockopt(s->discovery,IPPROTO_IP,IP_ADD_MEMBERSHIP,&group,sizeof(group))<0 && errno!=EADDRINUSE)
            ca_log("UniGCS multicast join %s: %s",i->ifa_name,strerror(errno));
    }
    freeifaddrs(interfaces);
}

int ca_unigcs_open(ca_unigcs **out,ca_media *media,unsigned tcp_port,unsigned discovery_port)
{
    if(!out || !media || !tcp_port || tcp_port>65535 || !discovery_port || discovery_port>65535) { errno=EINVAL; return -1; }
    auto *s=new(std::nothrow) ca_unigcs;
    if(!s) { errno=ENOMEM; return -1; }
    s->media=media;
    s->listener=bind_port(tcp_port,SOCK_STREAM);
    if(s->listener<0 || listen(s->listener,4)<0) { int e=errno; ca_unigcs_close(s); errno=e; return -1; }
    s->discovery=bind_port(discovery_port,SOCK_DGRAM);
    if(s->discovery<0 && errno!=EADDRINUSE) { int e=errno; ca_unigcs_close(s); errno=e; return -1; }
    if(s->discovery<0) ca_log("UniGCS discovery UDP %u already owned (stock product_upgrade may provide it)",discovery_port);
    else {
#ifdef __linux__
        int one=1;
        if(setsockopt(s->discovery,IPPROTO_IP,IP_PKTINFO,&one,sizeof(one))<0) {
            int error=errno; ca_unigcs_close(s); errno=error; return -1;
        }
#endif
        join_multicast(s);
    }
    ca_log("UniGCS private TCP %u discovery UDP %u",tcp_port,discovery_port);
    *out=s; return 0;
}

void ca_unigcs_update(ca_unigcs *s,ca_backend *backend,bool manual,ca_mavlink_server *mavlink)
{
    if(!s) return;
    s->backend=backend; s->manual=manual;
    s->mavlink=mavlink;
    bool recording=ca_media_recording(s->media);
    if(recording!=s->recording) { s->recording_since=recording ? now_ms() : 0; s->recording=recording; }
    if(s->discovery>=0) {
        // Ethernet/DHCP can become ready after camera-app starts. Rejoining
        // existing memberships is harmless (EADDRINUSE); also recover after
        // an interface is removed and recreated.
        if(now_ms()-s->multicast_join>=5000) join_multicast(s);
        discover(s);
    }
    sockaddr_in peer {}; socklen_t len=sizeof(peer);
    int fd=accept(s->listener,reinterpret_cast<sockaddr *>(&peer),&len);
    if(fd>=0) {
        if(s->client>=0 || fcntl(fd,F_SETFL,O_NONBLOCK)<0 || fcntl(fd,F_SETFD,FD_CLOEXEC)<0) close(fd);
        else {
            s->client=fd; s->peer=peer; s->heartbeat=s->last_request=now_ms();
            int one=1; (void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
            ca_log("UniGCS connected from %s:%u",inet_ntoa(peer.sin_addr),ntohs(peer.sin_port));
        }
    }
    flush(s);
    for(unsigned i=0;i<8 && s->client>=0;i++) {
        uint8_t data[2048]; ssize_t n=recv(s->client,data,sizeof(data),0);
        if(n>0) { ca_private_network_parser_feed(&s->parser,data,n,request,s); continue; }
        if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) disconnect(s);
        break;
    }
    if(s->client>=0 && now_ms()-s->last_request>=5000) disconnect(s);
    const auto tracking=ca_media_tracking_status(s->media);
    const bool tracked=tracking.owner==CA_TRACK_OWNER_SIYI &&
        (tracking.state==CA_TRACK_ACTIVE || tracking.state==CA_TRACK_COASTING);
    if(tracked!=s->tracking_reported || (s->tracking_pending && tracking.state==CA_TRACK_LOST)) {
        const uint8_t state=tracked?0:1;
        reply(s,0xac,&state,1); s->tracking_reported=tracked; s->tracking_pending=false;
    }
    request_time(s);
    if(s->client>=0 && s->zoom_direction && now_ms()-s->zoom_tick>=50) {
        s->zoom_tick=now_ms();
        // MT11/A8 have absolute zoom only. Match the public SDK's 0.1x steps
        // at 20 Hz; never accumulate catch-up steps after a stalled loop.
        const float target=std::max(1.0f,std::min(float(APCAM_ZOOM_CONTROL_MAX),
            ca_media_zoom(s->media)+0.1f*s->zoom_direction));
        const int result=APCAM_ZOOM_NATIVE_RATE
            ? ca_backend_set_zoom_rate(s->backend,s->zoom_direction)
            : ca_backend_set_zoom(s->backend,target);
        if(result<0) {
            if(APCAM_ZOOM_NATIVE_RATE) (void)ca_backend_set_zoom_rate(s->backend,0);
            s->zoom_direction=0;
        }
        uint8_t zoom[2]; put16(zoom,observed_zoom_tenths(s->media));
        if(s->zoom_reply) reply(s,0x98,zoom,2);
    }
    if(s->client>=0 && s->source && now_ms()-s->heartbeat>=1000) {
        const uint8_t present=1; reply(s,0xf0,&present,1); s->heartbeat=now_ms();
    }
}
void ca_unigcs_close(ca_unigcs *s)
{
    if(!s) return;
    disconnect(s);
    if(s->listener>=0) close(s->listener);
    if(s->discovery>=0) close(s->discovery);
    delete s;
}
