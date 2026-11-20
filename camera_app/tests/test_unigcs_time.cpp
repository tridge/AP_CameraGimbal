// Exercise the private time exchange with clocks that cannot change host time.
#define clock_gettime test_clock_gettime
#define clock_settime test_clock_settime
#include "../src/protocol/unigcs.cpp"
#undef clock_gettime
#undef clock_settime
#include "apcam/APC_Timezone.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>

static time_t realtime;
static uint64_t monotonic=10000;
static unsigned set_calls;
static bool fail_set;
static unsigned failure_logs, forwarded;
static char success_log[256];
void ca_mavlink_server_suspend_gimbal(ca_mavlink_server *) {}
bool ca_mavlink_server_survey_owns_gimbal(const ca_mavlink_server *) { return false; }

int test_clock_gettime(clockid_t clock,timespec *value)
{
    assert(clock==CLOCK_REALTIME || clock==CLOCK_MONOTONIC);
    *value=clock==CLOCK_REALTIME ? timespec{realtime,0} :
        timespec{time_t(monotonic/1000),long(monotonic%1000)*1000000};
    return 0;
}
int test_clock_settime(clockid_t clock,const timespec *value)
{
    assert(clock==CLOCK_REALTIME && value->tv_nsec==0);
    ++set_calls;
    if(fail_set) { errno=EPERM; return -1; }
    realtime=value->tv_sec;
    return 0;
}
void ca_log(const char *format,...)
{
    if (strstr(format,"system time setting from UniGCS failed")) ++failure_logs;
    if (strstr(format,"system time set from UniGCS command")) {
        va_list ap; va_start(ap,format);
        vsnprintf(success_log,sizeof(success_log),format,ap); va_end(ap);
    }
}
void ca_binlog_packet(bool,uint8_t,uint8_t,uint32_t,uint16_t,const uint8_t *,size_t,int32_t) {}
int ca_backend_set_zoom_rate(ca_backend *,float) { return 0; }
int ca_media_manual_focus(ca_media *,int) { abort(); }
// Keep the real dispatcher and camera command switch linked. Unexpected
// media actions must fail the test rather than hide a routing regression.
const ca_config *ca_media_settings(const ca_media *) { static ca_config cfg {}; return &cfg; }
bool ca_media_recording(const ca_media *) { return false; }
int ca_media_configure(ca_media *,const ca_config *) { abort(); }
int ca_media_set_recording(ca_media *,bool) { abort(); }
int ca_media_get_thermal_palette(ca_media *,uint8_t *) { abort(); }
int ca_media_set_thermal_palette(ca_media *,uint8_t) { abort(); }
int ca_media_get_thermal_gain(ca_media *,uint8_t *) { abort(); }
int ca_media_set_thermal_gain(ca_media *,uint8_t) { abort(); }
bool ca_media_thermal_main(const ca_media *) { abort(); }
int ca_media_set_thermal_main(ca_media *,bool) { abort(); }
enum ca_media_lens ca_media_lens(const ca_media *) { abort(); }
int ca_media_set_lens(ca_media *,enum ca_media_lens) { abort(); }
unsigned ca_media_frame_rate(const ca_media *,bool) { abort(); }
int ca_media_autofocus(ca_media *,uint16_t,uint16_t) { abort(); }
float ca_media_zoom(const ca_media *) { abort(); }
int ca_media_capture_photo(ca_media *,ca_photo_scope) { abort(); }
// Polled by every update and disconnect; no tracking backend here.
bool ca_media_tracking_available(ca_media *) { return false; }
void ca_media_tracking_stop(ca_media *,ca_tracking_owner) {}
ca_tracking_status ca_media_tracking_status(ca_media *) { return {}; }
bool ca_media_side_by_side(const ca_media *) { return false; }
int ca_media_set_side_by_side(ca_media *,bool) { abort(); }
int ca_backend_set_zoom(ca_backend *,float) { abort(); }
int ca_backend_handle_private(ca_backend *,const ca_private_frame *f)
{
    assert(f->destination==0x2e && f->link==0x11);
    assert(f->control==0x08 || f->control==0x09);
    ++forwarded;
    return 0;
}

static unsigned requests;
static void check_request(void *opaque,const ca_private_frame *f)
{
    auto *s=static_cast<ca_unigcs *>(opaque);
    assert(f->command==0x91 && f->control==0x09);
    assert(f->payload_length==1 && f->payload[0]==1);
    if(!s->long_format) {
        assert(f->source==0x34 && f->destination==s->source && f->link==0x16);
    }
    ++requests;
}
static void read_request(ca_unigcs &s,int peer,bool expected)
{
    uint8_t wire[256];
    const ssize_t n=recv(peer,wire,sizeof(wire),MSG_DONTWAIT);
    if(!expected) { assert(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)); return; }
    assert(n>0);
    ca_private_parser parser {};
    const unsigned before=requests;
    ca_private_network_parser_feed(&parser,wire,n,check_request,&s);
    assert(requests==before+1 && parser.length==0);
}
static void send_time(ca_unigcs &s,const uint8_t *date,size_t n,uint8_t control=0x0a,
                      uint8_t source=0xd0,uint8_t dest=0x34,uint8_t link=0x16,bool wrong_format=false)
{
    uint8_t wire[64];
    const size_t size=(s.long_format!=wrong_format) ?
        ca_long_build(wire,sizeof(wire),control&3,123,0x91,date,n) :
        ca_private_build(wire,sizeof(wire),control,123,source,dest,link,0x91,date,n);
    assert(size);
    ca_private_parser parser {};
    // Include stream fragmentation rather than bypassing the CRC parser.
    ca_private_network_parser_feed(&parser,wire,5,request,&s);
    ca_private_network_parser_feed(&parser,wire+5,size-5,request,&s);
    assert(parser.length==0);
}
static void test_exchange(bool old,time_t expected,time_t expected_leap)
{
    int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    ca_unigcs s;
    s.client=sockets[0]; s.long_format=old;
    realtime=0; set_calls=0; fail_set=false;
    const uint8_t valid[]={0xea,0x07,9,27,6,43,50,0}; // Reported A8/UniGCS local time
    request_time(&s); read_request(s,sockets[1],false); // Wait for a known peer/format.
    // Establish the session through a normal no-ACK client command.
    uint8_t hello[64], locale[6] {};
    const size_t hello_size=old ? ca_long_build(hello,sizeof(hello),0,1,0xf0,locale,6) :
        ca_private_build(hello,sizeof(hello),0x08,1,0xd0,0x34,0x16,0xf0,locale,6);
    ca_private_parser hello_parser {};
    ca_private_network_parser_feed(&hello_parser,hello,hello_size,request,&s);
    assert(s.source==0xd0 && s.long_format==old && s.last_request==monotonic);
    send_time(s,valid,8); assert(set_calls==0); // No unsolicited clock changes.
    request_time(&s); read_request(s,sockets[1],true);
    assert(s.time_pending);
    request_time(&s); read_request(s,sockets[1],false);
    monotonic+=999; request_time(&s); read_request(s,sockets[1],false);
    ++monotonic; request_time(&s); read_request(s,sockets[1],true);
    send_time(s,valid,8,0x0a,0xd0,0x34,0x16,true); assert(set_calls==0);
    if(!old) {
        send_time(s,valid,8,0x0a,0xd1);
        send_time(s,valid,8,0x0a,0xd0,0x2e);
        send_time(s,valid,8,0x0a,0xd0,0x34,0x11);
        send_time(s,valid,8,0x0b);
        // Response-framed time packets must never become gimbal commands.
        const unsigned before=forwarded;
        send_time(s,valid,8,0x0a,0xd0,0x2e,0x11);
        assert(forwarded==before);
        // Ordinary no-ACK/ACK gimbal commands retain their routing.
        send_time(s,valid,8,0x08,0xd0,0x2e,0x11);
        send_time(s,valid,8,0x09,0xd0,0x2e,0x11);
        assert(forwarded==before+2 && s.gimbal_requested[0x91]);
        assert(set_calls==0);
    }
    for(size_t n: {size_t(0),size_t(1),size_t(6),size_t(9)}) {
        uint8_t bad[9] {}; memcpy(bad,valid,sizeof(valid)); send_time(s,bad,n);
    }
    const uint8_t invalid[][8]={
        {0xea,7,0,25,0,45,31,0}, {0xea,7,13,25,0,45,31,0},
        {0xea,7,9,0,0,45,31,0}, {0xea,7,9,31,0,45,31,0},
        {0xeb,7,2,29,0,45,31,0}, {0xea,7,9,25,24,45,31,0},
        {0xea,7,9,25,0,60,31,0}, {0xea,7,9,25,0,45,60,0},
        {0xb2,7,1,1,0,0,0,0}, {0xea,7,8,30,23,59,59,0},
        {0x34,0x08,1,1,0,0,0,0}, // 2100: first rejected year
        {0x88,0x13,1,1,0,0,0,0}, // 5000: catches a weakened 9999 bound
        {0xff,0xff,9,25,0,45,31,0}};
    for(const auto &bad:invalid) send_time(s,bad,sizeof(bad));
    assert(set_calls==0 && s.time_pending);
    const uint64_t idle=s.last_request;
    monotonic+=10;
    fail_set=true; send_time(s,valid,8);
    assert(set_calls==1 && realtime==0 && s.time_pending && s.last_request==idle);
    monotonic+=1000; request_time(&s); read_request(s,sockets[1],true);
    fail_set=false; send_time(s,valid,8);
    assert(set_calls==2 && realtime==expected && !s.time_pending);
    assert(s.last_request==monotonic);
    assert(strstr(success_log,"source=0xd0") && strstr(success_log,"timezone="));
    tm shown {}; assert(localtime_r(&realtime,&shown));
    char text[32];
    assert(strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S",&shown));
    assert(!strcmp(text,"2026-09-27 06:43:50")); // UTC+10 must not display 16:43:50.
    monotonic+=1000; request_time(&s); read_request(s,sockets[1],false);
    send_time(s,valid,8); assert(set_calls==2);
    // MAVLink/public SDK may set the clock while a request is outstanding.
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    realtime=1800000000; send_time(s,valid,8);
    assert(realtime==1800000000 && set_calls==2 && !s.time_pending);
    // Also accept a packed reply sent using request/no-ACK framing.
    for(uint8_t control: {uint8_t(0x08),uint8_t(0x09)}) {
        realtime=0; request_time(&s); read_request(s,sockets[1],true);
        send_time(s,valid,7,control); assert(realtime==expected);
    }
    if(old) {
        // Actual UniGCS reply from 192.168.144.69, A8 log 00000519.BIN,
        // SIIN Id=1224. Seven calendar bytes, no timezone/DST byte.
        const uint8_t captured[]={
            0x55,0x66,0xaa,0xbb,0x00,0x07,0x00,0x00,0x00,0x02,0x00,0x91,
            0xee,0xd1,0xdf,0xcc,0xea,0x07,0x09,0x1b,0x06,0x2b,0x32,
            0xbb,0xa9,0x3a,0x56};
        realtime=0; request_time(&s); read_request(s,sockets[1],true);
        ca_private_parser parser {};
        const unsigned before=set_calls;
        ca_private_network_parser_feed(&parser,captured,sizeof(captured),request,&s);
        assert(parser.length==0 && set_calls==before+1 && realtime==expected);
    }
    // A real leap day is valid, including when a reserved byte is nonzero.
    const uint8_t leap[]={0xec,7,2,29,12,34,56,0xa5};
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    send_time(s,leap,8); assert(realtime==expected_leap);
    realtime=0; request_time(&s); read_request(s,sockets[1],true);
    disconnect(&s); assert(!s.time_pending && s.time_request_ms==0 && s.source==0);
    close(sockets[1]);
}
static void test_failures(bool old)
{
    assert(APC_Timezone::apply("GMT")==0);
    ca_unigcs s;
    int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    assert(fcntl(sockets[0],F_SETFL,O_NONBLOCK)==0);
    s.client=sockets[0]; s.source=0xd0; s.long_format=old;
    s.last_request=monotonic;
    const uint64_t idle=s.last_request;
    const uint8_t date[]={0xea,7,9,27,6,43,50};
    realtime=0; set_calls=0; failure_logs=0; fail_set=true;
    // Repeated replies must neither fill the log nor keep the session alive.
    for(unsigned i=0;i<10000;i++) {
        request_time(&s); read_request(s,sockets[1],true);
        send_time(s,date,sizeof(date));
        assert(s.last_request==idle && set_calls==i+1 && failure_logs==1);
        monotonic+=1000;
    }
    ca_unigcs_update(&s,nullptr,false);
    assert(s.client==-1 && !s.time_pending && !s.source);
    close(sockets[1]);

    // A reconnect while the same failure persists must not restart log spam.
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    s.client=sockets[0]; s.source=0xd0; s.long_format=old;
    request_time(&s); read_request(s,sockets[1],true);
    send_time(s,date,sizeof(date)); assert(failure_logs==1);
    // Retrying can still recover; a later, separate failure is reported again.
    monotonic+=1000; fail_set=false;
    request_time(&s); read_request(s,sockets[1],true);
    send_time(s,date,sizeof(date));
    assert(realtime==1790491430 && !s.time_pending && s.last_request==monotonic);
    realtime=0; fail_set=true;
    request_time(&s); read_request(s,sockets[1],true);
    send_time(s,date,sizeof(date)); assert(failure_logs==2);
    // Recovery by another time source also ends the failure period.
    realtime=1790491430;
    request_time(&s); read_request(s,sockets[1],false);
    realtime=0;
    request_time(&s); read_request(s,sockets[1],true);
    send_time(s,date,sizeof(date)); assert(failure_logs==3);
    disconnect(&s); close(sockets[1]); fail_set=false;
}

static void test_calendar_boundaries(bool old)
{
    int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    ca_unigcs s;
    s.client=sockets[0]; s.source=0xd0; s.long_format=old;
    realtime=0; set_calls=0; fail_set=false;
    assert(APC_Timezone::apply("Australia/Sydney")==0);
    request_time(&s); read_request(s,sockets[1],true);
    const uint8_t gap[]={0xeb,7,10,3,2,30,0};
    send_time(s,gap,sizeof(gap)); assert(set_calls==0);
    const uint8_t repeated[]={0xeb,7,4,4,2,30,0};
    send_time(s,repeated,sizeof(repeated));
    // The protocol has no offset/DST field: either occurrence round-trips.
    assert(set_calls==1 && (realtime==1806766200 || realtime==1806769800));
    assert(APC_Timezone::apply("GMT")==0);
    realtime=0;
    request_time(&s); read_request(s,sockets[1],true);
    const uint8_t last_year[]={0x33,8,12,31,23,59,59}; // 2099
    send_time(s,last_year,sizeof(last_year));
    if(sizeof(time_t)>=8) {
        tm checked {};
        assert(set_calls==2 && localtime_r(&realtime,&checked));
        assert(checked.tm_year==199 && checked.tm_mon==11 && checked.tm_mday==31);
    } else assert(realtime==0); // ARMv7 time_t cannot represent this date.
    disconnect(&s); close(sockets[1]);
}

int main()
{
    // Known epoch values, independent of the conversion being tested. In the
    // DST zone September is standard time, while February is summer time.
    const struct { const char *zone; time_t september, february; } cases[]={
        {"UTC0",1790491430,1835440496},
        {"GMT-10",1790455430,1835404496},
        {"Australia/Sydney",1790455430,1835400896},
        {"GMT+8",1790520230,1835469296},
        {"AEST-10AEDT,M10.1.0,M4.1.0/3",1790455430,1835400896},
    };
    for(const auto &c:cases) {
        assert(APC_Timezone::apply(c.zone)==0);
        test_exchange(false,c.september,c.february);
        test_exchange(true,c.september,c.february);
    }
    for(bool old: {false,true}) {
        test_failures(old);
        test_calendar_boundaries(old);
    }
    puts("PASS UniGCS time: production dispatch, v3/legacy routing, timezone/DST, bounded failure logging and idle timeout");
}
