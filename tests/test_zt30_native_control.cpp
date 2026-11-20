/* Actual shared driver with a simulated MCU wire and a stalled PTY. No SDK. */
#include "../camera_app/src/backends/zt30/control.cpp"
#include <cassert>
#include <vector>
#include <array>
#include <pty.h>
void ca_log(const char*,...) {}
void ca_binlog_emit(uint8_t,const void*,size_t) {}
uint64_t ca_binlog_time_us() { return monotonic_ms()*1000; }
void ca_binlog_feedback(const ca_gimbal_attitude*) {}
void ca_binlog_vendor(uint8_t,const uint8_t*,uint16_t) {}
void ca_binlog_packet(bool,uint8_t,uint8_t,uint32_t,uint16_t,const uint8_t*,size_t,int32_t) {}
extern "C" ssize_t __real_write(int,const void*,size_t);
static int short_fd=-1;static bool write_blocked=false;static unsigned short_prefix=0;
extern "C" ssize_t __wrap_write(int fd,const void *data,size_t length) {
    if (fd==short_fd && write_blocked) {
        if (!short_prefix) { short_prefix=5;return __real_write(fd,data,length<5?length:5); }
        errno=EAGAIN;return -1;
    }
    return __real_write(fd,data,length);
}
static unsigned private_replies;
static void private_emit(void*,const ca_private_frame *p) {
    assert(p->sequence==97 && p->control==10 && p->source==0x2e && p->destination==0x2c);
    assert(p->payload_length==1 && p->payload[0]==1);private_replies++;
}
static std::vector<uint8_t> emitted;
static void emit(void*,const uint8_t *data,size_t n) { emitted.assign(data,data+n); }
static void expect_local(uint8_t opcode,std::initializer_list<uint8_t> values) {
    ca_siyi_packet packet{};size_t consumed=0;
    assert(ca_siyi_parse_one(emitted.data(),emitted.size(),&packet,&consumed)==1);
    assert(consumed==emitted.size() && packet.opcode==opcode && packet.payload_length==values.size());
    assert(std::equal(values.begin(),values.end(),packet.payload));emitted.clear();
}
static bool camera_thermal=false,camera_wide=true,lens_fail=false;
static uint8_t camera_palette=0;
static int mounting(void*,bool inverted) { return inverted?-1:0; }
static std::vector<uint8_t> take(int fd) {
    uint8_t wire[512];ssize_t n=recv(fd,wire,sizeof(wire),0);assert(n>=13);
    assert(wire[4]==ca_crc8_maxim(wire,4));assert(ca_crc16(wire,n-2)==(wire[n-2]|(uint16_t(wire[n-1])<<8)));
    return {wire,wire+n};
}
static bool empty(int fd) { char c;ssize_t n=recv(fd,&c,1,MSG_PEEK);return n<0 && errno==EAGAIN; }
static ca_siyi_packet sdk(const std::vector<uint8_t>& wire) {
    ca_siyi_packet p{};
    if (wire[10]==0x06) {
        assert(wire.size()==15 && wire[1]==8 && wire[7]==0x2c && wire[8]==0x2e && wire[9]==0x6b);
        p.opcode=7;p.payload=wire.data()+11;p.payload_length=2;return p;
    }
    assert(wire[10]==0x16);size_t consumed=0;
    assert(ca_siyi_parse_one(wire.data()+11,wire.size()-13,&p,&consumed)==1);
    assert(consumed==wire.size()-13);return p;
}
static void reply(ca_backend *b,int fd,uint8_t sub,const uint8_t *data,size_t n) {
    std::vector<uint8_t> v(n+13);v[0]=0xaa;v[1]=0x0a;v[2]=2;v[3]=n;v[4]=ca_crc8_maxim(v.data(),4);
    v[5]=43;v[7]=0x2e;v[8]=0x2c;v[9]=0x6b;v[10]=sub;memcpy(v.data()+11,data,n);
    uint16_t crc=ca_crc16(v.data(),v.size()-2);v[v.size()-2]=crc;v.back()=crc>>8;
    assert(send(fd,v.data(),v.size(),0)==ssize_t(v.size()));assert(ca_backend_handle_fd(b)==0);
}
static void attitude(ca_backend *b,int fd,int16_t pitch=0,int16_t yaw=0,int16_t yaw_rate=0) {
    uint8_t payload[12]{};payload[0]=yaw;payload[1]=(uint16_t)yaw>>8;payload[6]=yaw_rate;payload[7]=(uint16_t)yaw_rate>>8;payload[2]=pitch;payload[3]=(uint16_t)pitch>>8;
    uint8_t packet[32];size_t n=ca_siyi_build(packet,sizeof(packet),2,991,0x0d,payload,sizeof(payload));
    reply(b,fd,0x16,packet,n);
}
static int command(ca_backend *b,uint8_t op,const uint8_t *p=nullptr,size_t n=0) {
    uint8_t packet[128];n=ca_siyi_build(packet,sizeof(packet),1,72,op,p,n);
    assert(n);return ca_backend_handle_siyi(b,packet,n);
}
int main() {
    ca_zt30::ZoomFeedback zoom;
    assert(!std::isfinite(zoom.get(1000)) && zoom.hfov(false,1000)==0);
    assert(std::fabs(zoom.hfov(true,1000)-84.5f)<.001f);
    assert(zoom.observe(1,0,1000) && zoom.get(1000)==1);
    assert(std::fabs(zoom.hfov(false,1000)-58.1f)<.001f);
    assert(zoom.observe(30,0,1000) && std::fabs(zoom.hfov(false,1000)-2.14f)<.001f);
    for (auto invalid: {std::array<uint8_t,2>{0,0},{31,0},{30,1},{1,10},{255,255}})
        assert(!zoom.observe(invalid[0],invalid[1],2000));
    assert(zoom.get(3500)==30 && !std::isfinite(zoom.get(3501)) && zoom.hfov(false,3501)==0);
    assert(!std::isfinite(zoom.get(999)));zoom.reset();assert(!std::isfinite(zoom.get(1000)));

    static_assert(APCAM_NUM_STREAMS==2 && APCAM_HAVE_THERMAL_VIDEO && APCAM_STREAM1_LENS_MASK==3);
    static_assert(APCAM_HAVE_PHOTO && !APCAM_HAVE_ZOOM && !APCAM_HAVE_FOCUS && !APCAM_HAVE_THERMAL && !APCAM_HAVE_LIDAR && !APCAM_HAVE_OPTICAL_ZOOM && APCAM_HAVE_IMAGE_CONTROLS);
    for (unsigned i=0;i<9;i++) {
        assert(apcam_feedback[0].matrix[i]==(i==8?-1:i%4==0?1:0));
        assert(apcam_angle_command[0].matrix[i]==(i==8?-1:i%4==0?1:0));
    }
    for (unsigned i=0;i<3;i++) assert(!apcam_feedback[0].offset[i] && !apcam_angle_command[0].offset[i]);
    for (unsigned i=0;i<9;i++) {
        assert(apcam_feedback[1].matrix[i]==(i%4==0?1:0));
        assert(apcam_angle_command[1].matrix[i]==(i%4==0?1:0));
    }
    for (float yaw : {-180.0f,180.0f}) {
        float input[3]={0,0,yaw},canonical[3],wire[3];
        apcam_inverse_transform(&apcam_angle_command[0],input,canonical,false);
        assert(canonical[2]==-yaw);apcam_transform(&apcam_angle_command[0],canonical,wire,false);
        assert(wire[2]==yaw);
    }
    int fds[2];assert(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0,fds)==0);
    auto *b=static_cast<ca_backend*>(calloc(1,sizeof(ca_backend)));assert(b);
    b->uart_fd=fds[0];b->datagram_transport=true;b->emit=emit;b->inverted_set=mounting;b->orientation=CA_MOUNT_AUTO;
    assert(send_startup(b)==0 && !std::isfinite(ca_zt30::zoom_feedback().get(monotonic_ms())));
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==7 && p.payload_length==2 && !p.payload[0] && !p.payload[1]);}
    {auto v=take(fds[1]);assert(sdk(v).opcode==1);}
    assert(take(fds[1])[10]==0x15);assert(take(fds[1])[10]==0x17);
    {auto v=take(fds[1]);assert(sdk(v).opcode==0x0d);}assert(empty(fds[1]));
    const uint8_t unqualified_pose[6]={1,0,2,0,3,0};reply(b,fds[1],0x50,unqualified_pose,6);assert(!b->have_attitude);
    // Incoming MCU lens commands must not be echoed or delegated to actuators.
    const uint8_t incoming_lens=1;
    for (uint8_t sub:{0x04,0x05}) {
        reply(b,fds[1],sub,&incoming_lens,1);
        assert(empty(fds[1]) && !std::isfinite(ca_zt30::zoom_feedback().get(monotonic_ms())));
    }

    // Source/palette commands are camera-local, including rollback on lens failure.
    assert(command(b,0x10)<0 && empty(fds[1]));
    b->thermal_main_get=[](void*){return camera_thermal;};
    b->thermal_main_set=[](void*,bool value){camera_thermal=value;return 0;};
    b->lens_wide=[](void*){return camera_wide;};
    b->lens_set=[](void*,bool value){if(lens_fail)return -1;camera_wide=value;return 0;};
    assert(command(b,0x10)==0);expect_local(0x10,{1,2});
    for(const auto &selection:{std::array<uint8_t,2>{2,0},{0,2},{1,2}}) {
        assert(command(b,0x11,selection.data(),2)==0);expect_local(0x11,{selection[0],selection[1]});
        assert(command(b,0x10)==0);expect_local(0x10,{selection[0],selection[1]});
    }
    const uint8_t thermal_source[2]={2,0};
    assert(command(b,0x11,thermal_source,2)==0);expect_local(0x11,{2,0});
    lens_fail=true;uint8_t zoom_mode=3;
    assert(command(b,0x11,&zoom_mode,1)<0 && camera_thermal && camera_wide);
    const uint8_t zoom_source[2]={0,2};
    assert(command(b,0x11,zoom_source,2)<0 && camera_thermal && camera_wide);
    lens_fail=false;
    for(uint8_t mode:{3,5}) {
        assert(command(b,0x11,&mode,1)==0);expect_local(0x11,{mode});
        assert(!camera_thermal && camera_wide==(mode==5));
    }
    for(const auto &bad:{std::vector<uint8_t>{},{0},{2},{0,0},{2,2},{3,0},{0,2,0}})
        assert(command(b,0x11,bad.data(),bad.size())<0);
    b->thermal_palette_get=[](void*,uint8_t *value){*value=camera_palette;return 0;};
    b->thermal_palette_set=[](void*,uint8_t value){if(value>11)return -1;camera_palette=value;return 0;};
    assert(command(b,0x1a)==0);expect_local(0x1a,{0});
    uint8_t palette=4;assert(command(b,0x1b,&palette,1)==0);expect_local(0x1b,{4});
    assert(command(b,0x1a)==0);expect_local(0x1a,{4});
    palette=255;assert(command(b,0x1b,&palette,1)<0 && camera_palette==4);
    assert(command(b,0x1b)<0 && command(b,0x1a,&palette,1)<0 && empty(fds[1]));

    assert(!ca_backend_has_gimbal(b));ca_gimbal_attitude a{};assert(!ca_backend_gimbal_attitude(b,&a));
    assert(ca_backend_set_gimbal_rates(b,0,.1f)<0 && empty(fds[1]));
    assert(ca_backend_set_gimbal_rates(b,0,0)==0);take(fds[1]);
    uint8_t one=1;
    for (uint8_t op:{0x04,0x05,0x06,0x0f,0x38,0x55})assert(command(b,op,&one,1)<0 && empty(fds[1]));
    ca_private_frame private_cmd{};private_cmd.destination=0x2e;private_cmd.link=0x11;private_cmd.command=0x9b;
    private_cmd.payload=&one;private_cmd.payload_length=1;private_cmd.control=8;
    assert(ca_backend_handle_private(b,&private_cmd)<0 && empty(fds[1]));
    b->reply_to_mcu=true;
    uint8_t delegated=4;assert(command(b,0x0c,&delegated,1)<0 && empty(fds[1]));
    assert(command(b,0x07,&one,1)<0 && empty(fds[1]));b->reply_to_mcu=false;
    reply(b,fds[1],0x17,&one,1);attitude(b,fds[1]);assert(ca_backend_has_gimbal(b));
    uint8_t changed_mount=2;reply(b,fds[1],0x17,&changed_mount,1);
    assert(!ca_backend_has_gimbal(b));reply(b,fds[1],0x17,&one,1);assert(ca_backend_has_gimbal(b));
    assert(command(b,0x18)==0);{auto v=take(fds[1]);assert(sdk(v).opcode==0x18);}
    const uint8_t raw_zoom[2]={2,5};uint8_t zoom_packet[32];
    size_t zoom_length=ca_siyi_build(zoom_packet,sizeof(zoom_packet),1,991,0x18,raw_zoom,2);
    reply(b,fds[1],0x16,zoom_packet,zoom_length);assert(!std::isfinite(ca_zt30::zoom_feedback().get(monotonic_ms())));
    zoom_length=ca_siyi_build(zoom_packet,sizeof(zoom_packet),2,991,0x18,raw_zoom,2);
    reply(b,fds[1],0x16,zoom_packet,zoom_length);assert(ca_zt30::zoom_feedback().get(monotonic_ms())==2.5f && empty(fds[1]));
    for (auto invalid: {std::vector<uint8_t>{1},{1,0,0},{0,0},{31,0},{1,10},{30,1}}) {
        zoom_length=ca_siyi_build(zoom_packet,sizeof(zoom_packet),2,991,0x18,invalid.data(),invalid.size());
        reply(b,fds[1],0x16,zoom_packet,zoom_length);
        assert(ca_zt30::zoom_feedback().get(monotonic_ms())==2.5f && empty(fds[1]));
    }

    b->attitude_request_ms=0;ca_backend_periodic(b);
    {auto v=take(fds[1]);assert(sdk(v).opcode==0x0d);}
    {auto v=take(fds[1]);assert(sdk(v).opcode==0x18);}assert(empty(fds[1]));

    uint8_t follow=4;assert(command(b,0x0c,&follow,1)==0);{auto v=take(fds[1]);assert(v[10]==0x15 && v[11]==1);}
    // Absolute targets use the declared physical envelope, independent of the
    // current pose. A distant target must not silently disable ROI/survey.
    assert(ca_backend_set_gimbal_angles(b,-1.0f,0)==0);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==0x0e &&
        (int16_t)(p.payload[2]|(uint16_t(p.payload[3])<<8))==-573);}
    assert(ca_backend_set_gimbal_angles(b,1.0f,0)==0);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==0x0e &&
        (int16_t)(p.payload[2]|(uint16_t(p.payload[3])<<8))==250);}
    assert(ca_backend_set_gimbal_angles(b,NAN,0)<0 && empty(fds[1]));
    assert(ca_backend_set_gimbal_angles(b,0,.01f)==0);{auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==0x0e && (int16_t)(p.payload[0]|(uint16_t(p.payload[1])<<8))==-6);}
    attitude(b,fds[1],0,-10,-20);assert(ca_backend_gimbal_attitude(b,&a));
    assert(fabs(a.yaw_rad-PI_F/180)<.0001 && fabs(a.yaw_rate_rad_s-2*PI_F/180)<.0001);
    const uint8_t sdk_angle[4]={10,0,0,0};assert(command(b,0x0e,sdk_angle,4)==0);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==0x0e && p.payload[0]==10 && !p.payload[1]);}
    attitude(b,fds[1]);
    // A lost 3-degree target must retry despite stationary pitch-rate bias.
    b->attitude.pitch_rate_rad_s=2*PI_F/180;
    assert(ca_backend_set_gimbal_angles(b,0,3*PI_F/180)==0);take(fds[1]);
    b->angle_target.sent_ms=monotonic_ms()-1001;
    assert(ca_backend_set_gimbal_angles(b,0,3*PI_F/180)==0);take(fds[1]);
    // Once reached, preserve suppression rather than restarting the controller.
    b->attitude.yaw_rad=3*PI_F/180;b->angle_target.sent_ms=monotonic_ms()-1001;
    assert(ca_backend_set_gimbal_angles(b,0,3*PI_F/180)==0 && empty(fds[1]));
    attitude(b,fds[1]);
    // The web/MAVLink value is degrees/s, distinct from raw SDK percent.
    struct RateCase { int dps, yaw, pitch; };
    for (const RateCase &rate : {RateCase{-60,-45,-45}, {-30,-24,-25},
            {-5,-8,-8}, {-3,-6,-6}, {-2,-6,-5}, {0,0,0},
            {2,5,5}, {3,6,6}, {5,8,8}, {30,24,25}, {60,45,45}}) {
        assert(ca_backend_set_gimbal_rates(b,rate.dps*PI_F/180,rate.dps*PI_F/180)==0);
        auto v=take(fds[1]);auto p=sdk(v);
        assert(p.opcode==7 && (int8_t)p.payload[0]==rate.yaw && (int8_t)p.payload[1]==rate.pitch);
    }
    const uint8_t raw_thirty[2]={30,(uint8_t)(int8_t)-30};
    assert(command(b,0x07,raw_thirty,2)==0);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.payload[0]==30 && (int8_t)p.payload[1]==-30);}
    const uint64_t old_stop=b->rate_stop_ms;
    usleep(20000);
    assert(ca_backend_set_gimbal_rates(b,-10,10)==0);
    assert(b->rate_stop_ms>old_stop);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==7 && p.payload[0]==45 && (int8_t)p.payload[1]==-45);}
    usleep(180000);ca_backend_periodic(b);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==7 && !p.payload[0] && !p.payload[1]);}
    {auto v=take(fds[1]);assert(sdk(v).opcode==0x0d);}assert(empty(fds[1]));
    ca_backend_request_gimbal_attitude(b);ca_backend_request_gimbal_attitude(b);assert(empty(fds[1]));
    attitude(b,fds[1]);uint8_t fpv=5;assert(command(b,0x0c,&fpv,1)==0);
    {auto v=take(fds[1]);assert(v[10]==0x15 && v[11]==2);}
    const uint8_t pulse[2]={5,0};assert(command(b,0x07,pulse,2)==0);
    {auto v=take(fds[1]);assert(v[10]==0x15 && v[11]==1);}
    {auto v=take(fds[1]);assert(sdk(v).opcode==7);}
    // A failed stop remains armed; after queue recovery periodic() retries it.
    std::array<uint8_t,256> junkwire{};
    while(send(fds[0],junkwire.data(),junkwire.size(),0)>0) {}
    assert(dev_rates(b,0,0)<0 && b->rate_stop_ms);
    uint8_t discard[512];while(recv(fds[1],discard,sizeof(discard),0)>0) {}
    usleep(180000);ca_backend_periodic(b);
    {auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==7 && !p.payload[0] && !p.payload[1]);}
    {auto v=take(fds[1]);assert(sdk(v).opcode==0x0d);}assert(!b->rate_stop_ms && empty(fds[1]));
    attitude(b,fds[1]);b->private_emit=private_emit;
    const uint8_t private_rates[2]={128,100};private_cmd.command=0x9a;private_cmd.payload=private_rates;
    private_cmd.payload_length=2;private_cmd.control=9;private_cmd.sequence=97;private_cmd.source=0x2c;
    assert(ca_backend_handle_private(b,&private_cmd)==0 && private_replies==1);
    {auto v=take(fds[1]);auto p=sdk(v);assert((int8_t)p.payload[0]==-45 && p.payload[1]==45);}
    assert(dev_rates(b,0,0)==0);take(fds[1]);
    b->attitude.timestamp_ms=monotonic_ms()-1001;assert(!ca_backend_has_gimbal(b));
    assert(ca_backend_set_gimbal_rates(b,0,.1f)<0 && empty(fds[1]));
    assert(ca_backend_set_gimbal_rates(b,0,0)==0);take(fds[1]);
    b->orientation=CA_MOUNT_UPRIGHT;b->mounting_direction_known=false;uint8_t two=2;reply(b,fds[1],0x17,&two,1);attitude(b,fds[1],1800);
    assert(!ca_backend_has_gimbal(b) && !ca_backend_gimbal_attitude(b,&a));
    assert(ca_backend_set_gimbal_angles(b,0,0)<0 && empty(fds[1]));
    assert(ca_backend_set_gimbal_neutral(b)<0 && empty(fds[1]));
    ca_backend_close(b);{auto v=take(fds[1]);auto p=sdk(v);assert(p.opcode==7 && !p.payload[0] && !p.payload[1]);}close(fds[1]);
    int master,slave;assert(openpty(&master,&slave,nullptr,nullptr,nullptr)==0);assert(configure_uart(slave)==0);
    assert(fcntl(slave,F_SETFL,O_NONBLOCK)==0);std::array<uint8_t,4096> junk{};
    while(write(slave,junk.data(),junk.size())>0) {}
    const uint64_t start=monotonic_ms();assert(write_all(slave,junk.data(),junk.size())<0);
    assert(monotonic_ms()-start<150);close(master);close(slave);
    int pipefds[2];assert(pipe2(pipefds,O_NONBLOCK)==0);
    auto *u=static_cast<ca_backend*>(calloc(1,sizeof(ca_backend)));assert(u);
    u->uart_fd=pipefds[1];u->mounting_direction_known=true;u->mounting_direction=1;u->gimbal_mode=1;
    u->have_attitude=true;u->attitude.timestamp_ms=monotonic_ms();
    short_fd=pipefds[1];write_blocked=true;
    assert(dev_rates(u,5,0)<0 && u->tx_offset==5 && u->tx_length>5 && u->rate_stop_ms);
    const size_t first_length=u->tx_length;write_blocked=false;
    assert(dev_rates(u,0,0)==0 && !u->tx_length && !u->rate_stop_ms);
    uint8_t wire[512];ssize_t count=read(pipefds[0],wire,sizeof(wire));assert(count==ssize_t(first_length*2));
    for (size_t pos:{size_t(0),first_length}) {
        assert(wire[pos]==0xaa && ca_crc16(wire+pos,first_length-2)==(wire[pos+first_length-2]|(uint16_t(wire[pos+first_length-1])<<8)));
        assert(first_length==15 && wire[pos+1]==8 && wire[pos+10]==6);
        assert(wire[pos+11]==(pos?0:5) && !wire[pos+12]);
    }
    short_fd=-1;ca_backend_close(u);close(pipefds[0]);
    // Exercise real UART open/flock/reopen; configured upright is still untrusted.
    char device[128];assert(openpty(&master,&slave,device,nullptr,nullptr)==0);close(slave);
    ca_backend_config config{};config.name="zt30";config.uart_device=device;config.emit=emit;
    config.recording_set=mounting;config.recording_get=[](void*) { return false; };
    config.zoom_set=[](void*,float) { return -1; };config.zoom_get=[](void*) { return NAN; };
    config.lens_set=mounting;config.lens_wide=[](void*) { return false; };
    config.photo_capture=[](void*) { return -1; };
    config.thermal_gain_get=[](void*,uint8_t*) { return -1; };
    config.thermal_gain_set=[](void*,uint8_t) { return -1; };
    config.thermal_palette_get=config.thermal_gain_get;config.thermal_palette_set=config.thermal_gain_set;
    config.inverted_set=mounting;config.orientation=CA_MOUNT_UPRIGHT;
    assert(ca_zt30::zoom_feedback().observe(2,0,monotonic_ms()));
    ca_backend *opened=nullptr,*second=nullptr;assert(ca_backend_open(&opened,&config)==0);
    assert(!std::isfinite(ca_zt30::zoom_feedback().get(monotonic_ms())));
    assert(opened->orientation==CA_MOUNT_AUTO && !ca_backend_has_gimbal(opened));
    assert(ca_backend_open(&second,&config)<0 && errno==EWOULDBLOCK);
    ca_backend_close(opened);assert(ca_backend_open(&opened,&config)==0);ca_backend_close(opened);close(master);
    puts("ZT30 native control: startup, unknown/inverted/stale feedback, motion limits/stop timer, rejected features, shutdown and stalled UART PASS");
}
