#include "camera_app/rtsp.h"
#include "camera_app/misb.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <fcntl.h>
#include "RtpConnection.h"
#include <string>

static unsigned sei_calls,ts_calls;
static int counted_insert(ca_video_codec codec,const uint8_t *data,size_t size,uint64_t pts,
                          float fov,uint8_t **output,size_t *length)
{
    sei_calls++;
    return ca_video_metadata_insert(codec,data,size,pts,fov,output,length);
}
static int counted_mux(ca_misb_ts *mux,ca_video_codec codec,const uint8_t *data,size_t size,
                       uint64_t pts,bool key,const ca_metadata *metadata,const timespec *utc,
                       ca_misb_write_fn write,void *opaque)
{
    ts_calls++;
    return ca_misb_ts_write(mux,codec,data,size,pts,key,metadata,utc,write,opaque);
}
// Count the expensive operations in the real adapter; all RTSP connections,
// session demand checks, codecs and network processing remain production code.
#define ca_video_metadata_insert counted_insert
#define ca_misb_ts_write counted_mux
#include "../src/streaming/rtsp_xop.cpp"
#undef ca_video_metadata_insert
#undef ca_misb_ts_write

static unsigned free_port()
{
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(fd,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))==0);
    socklen_t size=sizeof(addr); assert(getsockname(fd,reinterpret_cast<sockaddr *>(&addr),&size)==0);
    unsigned port=ntohs(addr.sin_port); close(fd); return port;
}
struct Client {
    int fd; unsigned sequence=0; std::string uri,session; bool aggregate;
    Client(unsigned port,const char *path,bool describe=true) {
        aggregate=!describe;
        fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
        timeval timeout{2,0}; setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.sin_port=htons(port);
        assert(connect(fd,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))==0);
        uri="rtsp://127.0.0.1:"+std::to_string(port)+"/"+path;
        if(describe) request("DESCRIBE");
    }
    ~Client() { disconnect(); }
    void disconnect() { if(fd>=0) { shutdown(fd,SHUT_RDWR); close(fd); fd=-1; } }
    void read(void *data,size_t size) { ssize_t n=recv(fd,data,size,MSG_WAITALL); if(n!=ssize_t(size)) fprintf(stderr,"read fd=%d requested=%zu returned=%zd errno=%d\n",fd,size,n,errno); assert(n==ssize_t(size)); }
    void request(const char *method) {
        std::string text=std::string(method)+" "+uri+(std::string(method)=="SETUP" && !aggregate ? "/track0" : "")+" RTSP/1.0\r\nCSeq: "+std::to_string(++sequence)+"\r\n";
        if(std::string(method)=="DESCRIBE") text+="Accept: application/sdp\r\n";
        if(!session.empty()) text+="Session: "+session+"\r\n";
        if(std::string(method)=="SETUP") text+="Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
        text+="\r\n";
        assert(send(fd,text.data(),text.size(),0)==ssize_t(text.size()));
        text.clear(); char byte;
        for(;;) {
            read(&byte,1);
            if(text.empty() && byte=='$') {
                uint8_t header[3]; read(header,sizeof(header));
                std::vector<uint8_t> payload(header[1]*256+header[2]); read(payload.data(),payload.size());
                continue;
            }
            text+=byte;
            if(text.size()>=4 && text.compare(text.size()-4,4,"\r\n\r\n")==0) break;
        }
        assert(text.find("RTSP/1.0 200 ")==0);
        auto at=text.find("Session: ");
        if(at!=std::string::npos) { at+=9; session=text.substr(at,text.find_first_of(";\r",at)-at); }
        at=text.find("Content-Length: ");
        if(at!=std::string::npos) { std::vector<uint8_t> body(std::stoul(text.substr(at+16))); read(body.data(),body.size()); }
    }
    void play() { request("SETUP"); request("PLAY"); }
};
static void wait_idle(ca_rtsp *server,xop::MediaSessionId session)
{
    for(unsigned i=0;i<200 && server->server->HasPlayingClients(session);i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!server->server->HasPlayingClients(session));
}
static void tcp_socket_ownership_test(ca_rtsp *server)
{
    int listener=socket(AF_INET,SOCK_STREAM,0); assert(listener>=0);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(listener,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))==0);
    assert(listen(listener,1)==0);
    socklen_t size=sizeof(addr); assert(getsockname(listener,reinterpret_cast<sockaddr *>(&addr),&size)==0);
    int peer=socket(AF_INET,SOCK_STREAM,0); assert(peer>=0);
    assert(connect(peer,reinterpret_cast<sockaddr *>(&addr),sizeof(addr))==0);
    int fd=accept(listener,nullptr,nullptr); assert(fd>=0);
    auto owner=std::make_shared<xop::TcpConnection>(server->loop->GetTaskScheduler().get(),fd);
    auto rtp=std::make_shared<xop::RtpConnection>(owner);
    assert(rtp->SetupRtpOverTcp(xop::channel_0,0,1));
    assert(rtp->SetupRtpOverTcp(xop::channel_1,2,3));
    rtp.reset();
    // Both channels borrow the single RTSP connection. RTP destruction must
    // never close it (or another descriptor which has reused its number).
    assert(fcntl(fd,F_GETFD)>=0);
    char sent='x',received=0;
    assert(send(fd,&sent,1,0)==1 && recv(peer,&received,1,MSG_WAITALL)==1 && received==sent);
    owner->Disconnect(); owner.reset(); close(peer); close(listener);
    puts("PASS interleaved RTP cleanup preserves the TCP owner's socket");
}
int main()
{
    ca_video_metadata_set_format(CA_VIDEO_TELEMETRY_MISB);
    ca_rtsp *server=nullptr; unsigned port=free_port();
    assert(ca_rtsp_open(&server,port,"video1",CA_VIDEO_H264,10)==0);
    assert(ca_rtsp_add_alias(server,0,"legacy.264")==0);
    tcp_socket_ownership_test(server);
    uint8_t headers[]={0,0,0,1,0x67,0x42,0xc0,0x1f,0xda,0x01,0x40,0x16,0xe8,0x40,
                      0,0,0,1,0x68,0xce,0x06,0xe2};
    uint8_t idr[]={0,0,0,1,0x65,0x88,0x84};
    auto push=[&]() { assert(ca_rtsp_push_video(server,0,idr,sizeof(idr),true,60)==0); };
    assert(ca_rtsp_push_video(server,0,headers,sizeof(headers),false,60)==0);
    assert(!server->streams[0].misb.headers.empty());
    auto initial=server->streams[0].next_timestamp;
    for(unsigned i=0;i<20;i++) push();
    assert(server->streams[0].next_timestamp-initial==20*9000);
    assert(!sei_calls && !ts_calls);
    {
        Client misb(port,"video1.misb"); push(); assert(!sei_calls && !ts_calls);
        misb.request("SETUP"); push(); assert(!sei_calls && !ts_calls);
        misb.request("PLAY"); push(); assert(!sei_calls && ts_calls==1);
        assert(server->streams[0].misb.started);
        misb.request("TEARDOWN"); push(); assert(!sei_calls && ts_calls==1);
        assert(!server->streams[0].misb.started);
    }
    {
        Client regular(port,"video1"); regular.play(); push(); assert(sei_calls==1 && ts_calls==1);
        Client alias(port,"legacy.264"); alias.play(); push(); assert(sei_calls==2 && ts_calls==1);
        Client misb(port,"video1.misb"); misb.play(); push(); assert(sei_calls==3 && ts_calls==2);
        misb.disconnect(); wait_idle(server,server->streams[0].misb_session);
        push(); assert(sei_calls==4 && ts_calls==2);
    }
    wait_idle(server,server->streams[0].session_id);
    wait_idle(server,server->streams[0].aliases[0]);
    for(unsigned i=0;i<20;i++) push();
    assert(sei_calls==4 && ts_calls==2);
    // SETUP-before-DESCRIBE selects XOP's SAT>IP path. MP2T bytes must stay
    // untouched while the legacy H.264 endpoint retains Annex-B conversion.
    for(bool transport: {true,false}) {
        Client aggregate(port,transport?"video1.misb":"video1",false);
        aggregate.play(); push();
        uint8_t header[4]; aggregate.read(header,4);
        assert(header[0]=='$' && header[1]==0);
        std::vector<uint8_t> packet(header[2]*256+header[3]);
        aggregate.read(packet.data(),packet.size());
        if(transport) {
            assert((packet[1]&127)==33 && (packet.size()-12)%188==0);
            for(size_t at=12;at<packet.size();at+=188) assert(packet[at]==0x47);
        } else {
            assert(packet.size()>=17 && packet[12]==0 && packet[13]==0 && packet[14]==0 && packet[15]==1);
        }
    }
    ca_rtsp_close(server);
    puts("PASS idle/DESCRIBE/SETUP/TEARDOWN perform no live packaging; PLAY, aliases, concurrent formats and reconnect work");
}
