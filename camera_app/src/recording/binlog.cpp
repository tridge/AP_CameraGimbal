#include "apcam/compiler.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/binlog.h"
#include "camera_app/system_stats.h"
#include "camera_app/config.h"
#include "camera_app/backend.h"
#include "camera_app/log.h"
#include "apcam/target.h"
#include "binlog_version.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The control thread never opens, writes or syncs the SD card. Reserve queue
 * slots for lifecycle commands, and drop whole records on overload. */
#define QUEUE_SIZE 4096U
struct entry { uint16_t length; uint8_t kind; uint8_t data[255]; };
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t thread;
    struct entry *queue;
    char *root;
    unsigned head, count;
    uint32_t dropped, errors, packet_id;
    bool initialized, active, quit;
} logger = { .mutex=PTHREAD_MUTEX_INITIALIZER, .wake=PTHREAD_COND_INITIALIZER };
struct __attribute__((packed)) fmt_record {
    uint8_t sync1,sync2,id,type,length;
#if __GNUC__ >= 8
    char name[4] __attribute__((nonstring));
#else
    char name[4];
#endif
    char format[16],labels[64];
};
template<size_t N, size_t F, size_t L>
static constexpr fmt_record make_format(uint8_t id, uint8_t length,
                                       const char (&name)[N], const char (&format)[F], const char (&labels)[L])
{
    // DataFlash permits all 16 format bytes/64 label bytes, without a NUL.
    static_assert(N <= 5 && F <= 17 && L <= 65, "DataFlash FMT field too long");
    fmt_record r = {0xa3, 0x95, 128, id, length, {}, {}, {}};
    for (size_t i=0; i<N-1; i++) r.name[i]=name[i];
    for (size_t i=0; i<F-1; i++) r.format[i]=format[i];
    for (size_t i=0; i<L-1; i++) r.labels[i]=labels[i];
    return r;
}
#define FMT(id, type, name, format, labels) make_format(id, 3+sizeof(struct type), name, format, labels)
static const struct fmt_record formats[] = {
    {0xa3,0x95,128,128,89,"FMT","BBnNZ","Type,Length,Name,Format,Columns"},
    FMT(CA_LOG_SYS,ca_log_sys,"SYS","QffQQQB","TimeUS,CPUTemp,CPULoad,MemFree,MemAvail,SDFree,Valid"),
    FMT(CA_LOG_AE,ca_exposure,"AE","QBBHBBifffffff","TimeUS,Lens,Src,Valid,Mode,State,Result,US,AG,DG,IG,Y,Targ,Err"),
    FMT(CA_LOG_VEND,ca_log_vendor,"VEND","QBHZ","TimeUS,Opcode,Length,Payload"),
    FMT(CA_LOG_PARM,ca_log_parm,"PARM","QNf","TimeUS,Name,Value"),
    FMT(CA_LOG_MSG,ca_log_msg,"MSG","QZ","TimeUS,Message"),
    FMT(CA_LOG_POS,ca_log_pos,"POS","QILLfffffBB","TimeUS,BootMS,Lat,Lng,Alt,RelAlt,VN,VE,VD,SS,SC"),
    FMT(CA_LOG_ATT,ca_log_att,"ATT","QIBffffffBB","TimeUS,TBoot,Src,Roll,Pitch,Yaw,RollRate,PitchRate,YawRate,SS,SC"),
    FMT(CA_LOG_GIMB,ca_log_gimb,"GIMB","QQffffff","TimeUS,SampleUS,Roll,Pitch,Yaw,RollRate,PitchRate,YawRate"),
    FMT(CA_LOG_TRKF,ca_log_trkf,"TRKF","QQBfffffffff","TimeUS,FrameMS,St,Q,PE,YE,PF,YF,Roll,Pitch,Yaw,FOV"),
    FMT(CA_LOG_TFRA,ca_log_tfra,"TFRA","QQQ","TimeUS,PTS,SdkPTS"),
    FMT(CA_LOG_PIDP,ca_log_pid,"PIDP","Qfffffffffff","TimeUS,Tar,Act,Rate,FF,Err,P,I,D,Out,DT,Age"),
    FMT(CA_LOG_PIDY,ca_log_pid,"PIDY","Qfffffffffff","TimeUS,Tar,Act,Rate,FF,Err,P,I,D,Out,DT,Age"),
    FMT(CA_LOG_MODE,ca_log_mode,"MODE","QIBBBBBB","TimeUS,FlightMode,Armed,Mode,Method,YawLock,Recording,SysId"),
    FMT(CA_LOG_CMD,ca_log_cmd,"CMD","QHBBBfffffff","TimeUS,Cmd,SysId,CompId,Result,P1,P2,P3,P4,P5,P6,P7"),
    FMT(CA_LOG_MAVC,ca_log_mavc,"MAVC","QBBBBBHffffddfBB","TimeUS,TS,TC,SS,SC,Fr,Cmd,P1,P2,P3,P4,X,Y,Z,Res,WL"),
    FMT(CA_LOG_GMBC,ca_log_gmbc,"GMBC","QBBBBHfffffffB","TimeUS,TS,TC,SS,SC,Flg,Q1,Q2,Q3,Q4,VX,VY,VZ,Res"),
    FMT(CA_LOG_MAVP,ca_log_mavp,"MAVP","QBBBBBBNfBaa","TimeUS,TS,TC,SS,SC,PT,Ext,Name,Val,Res,Raw1,Raw2"),
    FMT(CA_LOG_MAVH,ca_log_mavh,"MAVH","QBBIBBBBB","TimeUS,SS,SC,Mode,Type,AP,Base,State,Ver"),
    FMT(CA_LOG_SIIN,ca_log_packet,"SIIN","QIBBIHBBHHHHiaaa","TimeUS,Id,Lnk,Proto,IP,Port,Src,Dst,Cmd,Seq,Len,Ofs,Res,D1,D2,D3"),
    FMT(CA_LOG_SIOU,ca_log_packet,"SIOU","QIBBIHBBHHHHiaaa","TimeUS,Id,Lnk,Proto,IP,Port,Src,Dst,Cmd,Seq,Len,Ofs,Res,D1,D2,D3"),
    FMT(CA_LOG_XFIN,ca_log_packet,"XFIN","QIBBIHBBHHHHiaaa","TimeUS,Id,Lnk,Proto,IP,Port,Src,Dst,Cmd,Seq,Len,Ofs,Res,D1,D2,D3"),
    FMT(CA_LOG_XFOU,ca_log_packet,"XFOU","QIBBIHBBHHHHiaaa","TimeUS,Id,Lnk,Proto,IP,Port,Src,Dst,Cmd,Seq,Len,Ofs,Res,D1,D2,D3"),
    FMT(CA_LOG_CAM,ca_log_cam,"CAM","QBiLLffff","TimeUS,Scope,Result,Lat,Lng,Alt,Roll,Pitch,Yaw"),
    FMT(CA_LOG_VID,ca_log_vid,"VID","QBiZ","TimeUS,Active,Result,Path"),
    FMT(CA_LOG_GCMD,ca_log_gcmd,"GCMD","QBffffi","TimeUS,Mode,Pitch,Yaw,WireP,WireY,Result"),
    FMT(CA_LOG_STAT,ca_log_stat,"STAT","QIII","TimeUS,Dropped,Queued,Errors"),
    FMT(CA_LOG_TIME,ca_log_time,"TIME","QQ","TimeUS,UTC"),
    FMT(CA_LOG_ROI,ca_log_roi,"ROI","QLLfB","TimeUS,Lat,Lng,Alt,Active"),
    FMT(CA_LOG_PRMA,ca_log_parm,"PRMA","QNf","TimeUS,Name,Value"),
};
APC_STATIC_ASSERT(sizeof(struct fmt_record)==89, "DataFlash FMT layout");
uint64_t ca_binlog_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000U + ts.tv_nsec/1000U;
}
static void enqueue(const struct entry *entry)
{
    logger.queue[(logger.head+logger.count)%QUEUE_SIZE]=*entry;
    logger.count++;
    pthread_cond_signal(&logger.wake);
}
bool ca_binlog_active(void)
{
    pthread_mutex_lock(&logger.mutex);
    bool active=logger.active;
    pthread_mutex_unlock(&logger.mutex);
    return active;
}
void ca_binlog_emit(uint8_t id, const void *data, size_t size)
{
    if (size>252) return;
    struct entry entry={.length=(uint16_t)(size+3),.kind=0};
    entry.data[0]=0xa3; entry.data[1]=0x95; entry.data[2]=id;
    memcpy(entry.data+3,data,size);
    pthread_mutex_lock(&logger.mutex);
    if (logger.active) {
        if (logger.count<QUEUE_SIZE-2) enqueue(&entry);
        else logger.dropped++;
    }
    pthread_mutex_unlock(&logger.mutex);
}
void ca_binlog_packet(bool outgoing, uint8_t protocol, uint8_t link,
                     uint32_t ip, uint16_t port, const uint8_t *data, size_t length, int32_t result)
{
    if (!data || !length || length > UINT16_MAX) return;
    const int saved_errno = errno;
    pthread_mutex_lock(&logger.mutex);
    if (!logger.active) { pthread_mutex_unlock(&logger.mutex); return; }
    ca_log_packet r = {.time_us=ca_binlog_time_us(), .packet_id=++logger.packet_id,
        .link=link, .protocol=protocol, .ip=ip, .port=port, .source=0, .destination=0,
        .command=UINT16_MAX, .sequence=UINT16_MAX, .length=(uint16_t)length, .offset=0, .result=result};
    const auto get16 = [](const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); };
    if (protocol == CA_PACKET_SIYI && length >= 10) {
        r.command=data[7]; r.sequence=get16(data+5);
    } else if (protocol == CA_PACKET_MT11 && length >= 14) {
        r.source=data[8]; r.destination=data[9]; r.command=data[11]; r.sequence=get16(data+6);
    } else if (protocol == CA_PACKET_SIYI_MCU && length >= 13) {
        r.source=data[7]; r.destination=data[8]; r.command=data[10]; r.sequence=get16(data+5);
    } else if (protocol == CA_PACKET_SIYI_LONG && length >= 20) {
        r.command=data[11]; r.sequence=get16(data+9);
    } else if (protocol == CA_PACKET_XFROBOT && length >= 72) {
        r.command=data[69];
    }
    const size_t count = (length+sizeof(r.data)-1)/sizeof(r.data);
    if (logger.count+count > QUEUE_SIZE-2) {
        logger.dropped += count;
    } else {
        const bool siyi = protocol <= CA_PACKET_SIYI_MCU || protocol == CA_PACKET_SIYI_LONG;
        entry e = {.length=uint16_t(sizeof(r)+3), .kind=0};
        e.data[0]=0xa3; e.data[1]=0x95;
        e.data[2]=siyi ? (outgoing ? CA_LOG_SIOU : CA_LOG_SIIN) : (outgoing ? CA_LOG_XFOU : CA_LOG_XFIN);
        for (size_t offset=0; offset<length; offset+=sizeof(r.data)) {
            r.offset=offset;
            const size_t n = length-offset < sizeof(r.data) ? length-offset : sizeof(r.data);
            memset(r.data, 0, sizeof(r.data));
            memcpy(r.data, data+offset, n);
            memcpy(e.data+3, &r, sizeof(r));
            enqueue(&e);
        }
    }
    pthread_mutex_unlock(&logger.mutex);
    errno=saved_errno;
}

void ca_binlog_message(const char *text)
{
    struct ca_log_msg r={.time_us=ca_binlog_time_us()};
    snprintf(r.text,sizeof(r.text),"%s",text);
    ca_binlog_emit(CA_LOG_MSG,&r,sizeof(r));
}
void ca_binlog_parameter(const char *name,float value,bool applied)
{
    struct ca_log_parm r={.time_us=ca_binlog_time_us()};
    r.value = value;
    memcpy(r.name,name,strnlen(name,sizeof(r.name)));
    ca_binlog_emit(applied ? CA_LOG_PRMA : CA_LOG_PARM,&r,sizeof(r));
}
void ca_binlog_feedback(const struct ca_gimbal_attitude *a)
{
    const float deg=57.295779513f;
    CA_BINLOG(CA_LOG_GIMB,ca_log_gimb,.sample_us=a->timestamp_ms*1000U,
        .roll=a->roll_rad*deg,.pitch=a->pitch_rad*deg,.yaw=a->yaw_rad*deg,
        .rollrate=a->roll_rate_rad_s*deg,.pitchrate=a->pitch_rate_rad_s*deg,.yawrate=a->yaw_rate_rad_s*deg);
}
void ca_binlog_vendor(uint8_t opcode, const uint8_t *payload, uint16_t length)
{
    struct ca_log_vendor r={.time_us=ca_binlog_time_us(),.opcode=opcode,.length=length};
    const char hex[]="0123456789abcdef";
    for (unsigned i=0;i<length && i<31;i++) {
        r.payload[2*i]=hex[payload[i]>>4];
        r.payload[2*i+1]=hex[payload[i]&15];
    }
    ca_binlog_emit(CA_LOG_VEND,&r,sizeof(r));
}
void ca_binlog_stats(void)
{
    struct ca_log_stat r={.time_us=ca_binlog_time_us()};
    pthread_mutex_lock(&logger.mutex);
    r.dropped=logger.dropped; r.queued=logger.count; r.errors=logger.errors;
    pthread_mutex_unlock(&logger.mutex);
    ca_binlog_emit(CA_LOG_STAT,&r,sizeof(r));
}
bool ca_binlog_start(const struct ca_config *config)
{
    pthread_mutex_lock(&logger.mutex);
    if (!logger.initialized || logger.active || logger.count>=QUEUE_SIZE/2) {
        pthread_mutex_unlock(&logger.mutex); return false;
    }
    struct entry entry={};
    entry.kind = 1;
    enqueue(&entry);
    logger.active=true;
    pthread_mutex_unlock(&logger.mutex);
    ca_binlog_message("AP_CameraGimbal " APCAM_MODEL_NAME " " CA_FIRMWARE_VERSION " " CA_FIRMWARE_GIT_HASH);
    for (size_t i=0;i<ca_config_param_count();i++)
        ca_binlog_parameter(ca_config_param_name(i),ca_config_param_get(config,i),false);
    return true;
}
void ca_binlog_stop(void)
{
    pthread_mutex_lock(&logger.mutex);
    if (logger.active) {
        logger.active=false;
        struct entry entry={};
        entry.kind = 2;
        enqueue(&entry);
    }
    pthread_mutex_unlock(&logger.mutex);
}
static int write_all(int fd,const void *data,size_t size)
{
    const uint8_t *p=(const uint8_t*)(data);
    while (size) {
        ssize_t n=write(fd,p,size);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        size-=n; p+=n;
    }
    return 0;
}
static int open_log(void)
{
    if (mkdir(logger.root,0755)<0 && errno!=EEXIST) return -1;
    DIR *dir=opendir(logger.root);
    if (!dir) return -1;
    unsigned maximum=0;
    struct dirent *ent;
    while ((ent=readdir(dir))) {
        char *end;
        unsigned long n=strtoul(ent->d_name,&end,10);
        if (end!=ent->d_name && strcasecmp(end,".BIN")==0 && n<=99999999 && n>maximum) maximum=(unsigned)n;
    }
    closedir(dir);
    char path[4096];
    int fd=-1;
    while (maximum<99999999) {
        if (snprintf(path,sizeof(path),"%s/%08u.BIN",logger.root,++maximum)>=(int)sizeof(path)) { errno=ENAMETOOLONG; return -1; }
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) return -1;
    if (write_all(fd,formats,sizeof(formats))<0) { close(fd); return -1; }
    ca_log("BIN logging started: %s",path);
    char last[4096],number[24];
    if (snprintf(last,sizeof(last),"%s/LASTLOG.TXT",logger.root)<(int)sizeof(last)) {
        int index=open(last,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
        if (index>=0) { int n=snprintf(number,sizeof(number),"%u\r\n",maximum); (void)write_all(index,number,(size_t)n); close(index); }
    }
    return fd;
}
static void *writer(void *unused)
{
    (void)unused;
    int fd=-1;
    uint8_t buffer[65536];
    size_t used=0;
    uint64_t flushed=ca_binlog_time_us(),synced=flushed,system_sampled=flushed;
    struct ca_system_stats system_stats= {};
    for (;;) {
        struct entry entry= {};
        pthread_mutex_lock(&logger.mutex);
        if (!logger.count && !logger.quit) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME,&until);
            until.tv_nsec+=100000000;
            if (until.tv_nsec>=1000000000) { until.tv_sec++; until.tv_nsec-=1000000000; }
            pthread_cond_timedwait(&logger.wake,&logger.mutex,&until);
        }
        bool available=logger.count!=0,quit=logger.quit && !available;
        if (available) { entry=logger.queue[logger.head]; logger.head=(logger.head+1)%QUEUE_SIZE; logger.count--; }
        pthread_mutex_unlock(&logger.mutex);
        uint64_t now=ca_binlog_time_us();
        bool lifecycle=available && entry.kind!=0;
        bool failed=false;
        if (fd>=0 && used && (lifecycle || quit || used+entry.length>sizeof(buffer) || now-flushed>=250000)) {
            failed=write_all(fd,buffer,used)<0; used=0; flushed=now;
        }
        if (fd>=0 && (quit || lifecycle || now-synced>=2000000)) {
            if (fdatasync(fd)<0) failed=true;
            synced=now;
        }
        if (lifecycle || quit) { if (fd>=0) close(fd); fd=-1; }
        if (available && entry.kind==1) {
            fd=open_log();
            if (fd<0) failed=true;
            else {
                struct ca_log_sys baseline;
                system_stats=(struct ca_system_stats){0};
                ca_system_stats_sample(&system_stats,fd,&baseline);
                system_sampled=baseline.time_us;
            }
        }
        if (fd>=0 && !failed && now>=system_sampled && now-system_sampled>=5000000U) {
            struct __attribute__((packed)) { uint8_t sync[3]; struct ca_log_sys data; }
                record={.sync={0xa3,0x95,CA_LOG_SYS}};
            ca_system_stats_sample(&system_stats,fd,&record.data);
            system_sampled=record.data.time_us;
            /* Reserve room for both this record and the queued entry below. */
            if (used+sizeof(record)+entry.length>sizeof(buffer)) {
                failed=write_all(fd,buffer,used)<0;
                used=0; flushed=now;
            }
            if (!failed) { memcpy(buffer+used,&record,sizeof(record)); used+=sizeof(record); }
        }
        if (failed) {
            int error=errno;
            if (fd>=0) close(fd);
            fd=-1; used=0;
            pthread_mutex_lock(&logger.mutex);
            logger.errors++; logger.active=false;
            /* Discard this failed session; a later policy retry starts a new file. */
            logger.count=0;
            pthread_mutex_unlock(&logger.mutex);
            ca_log("BIN logging failed: %s",strerror(error));
        }
        if (available && entry.kind==0 && fd>=0) { memcpy(buffer+used,entry.data,entry.length); used+=entry.length; }
        if (quit) break;
    }
    return NULL;
}
int ca_binlog_init(const char *root)
{
    logger.root=strdup(root);
    logger.queue=(struct entry*)(calloc(QUEUE_SIZE,sizeof(*logger.queue)));
    if (!logger.root || !logger.queue) { free(logger.root); free(logger.queue); return -1; }
    logger.quit=false;
    int err=pthread_create(&logger.thread,NULL,writer,NULL);
    if (err) { free(logger.root); free(logger.queue); errno=err; return -1; }
    logger.initialized=true;
    return 0;
}
void ca_binlog_close(void)
{
    if (!logger.initialized) return;
    ca_binlog_stop();
    pthread_mutex_lock(&logger.mutex);
    logger.quit=true;
    pthread_cond_signal(&logger.wake);
    pthread_mutex_unlock(&logger.mutex);
    pthread_join(logger.thread,NULL);
    logger.initialized=false;
    free(logger.queue); free(logger.root);
}
