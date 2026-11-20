#include "../camera_app/src/backends/zt30/encoder_owner.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace ca_zt30;
static std::vector<std::string> calls;
static EncoderPacket *packs;
static unsigned ch=4,src=0,ready_packs=0;static bool bad=false,device_owned=true,started=false;
static std::vector<uint8_t> golden() {
    // Original dev8/ch4 CreateChn payload, without its8-byte envelope.
    const char *hex="0400000000200000001000000000000000000000000f000070080000000000000000000000000000070000001e00000000000000010000000000000000000000000000000000000000000000";
    std::vector<uint8_t> b;
    for(size_t i=0;i<std::strlen(hex);i+=2) {
        const auto digit=[](char x){return x<='9'?x-'0':x-'a'+10;};
        b.push_back(uint8_t((digit(hex[i])<<4)|digit(hex[i+1])));
    }
    assert(b.size()==76);
    const auto word=[&b](size_t offset,uint32_t value) { for(unsigned i=0;i<4;i++)b[offset+i]=value>>(8*i); };
    word(4,3840);word(8,2160);b[16]=1;
    word(20,ch==4?3840:ch==5?2560:640);word(24,ch==4?2160:ch==5?1440:512);
    word(44,1);word(48,1);word(52,90);return b;
}
static void ids(unsigned d,unsigned c) { assert(d==8 && c==ch); }
int main() {
    Api sys{};EncoderApi enc{};PacketMemory memory{};
    enc.create_device=[](uint32_t d,EncoderDevice *p) { assert(d==8 && p->max_width==3840 && p->max_height==2160);calls.emplace_back("device");return 0; };
    enc.destroy_device=[](uint32_t d) { assert(d==8);calls.emplace_back("destroy_device");return 0; };
    enc.create_channel=[](uint32_t d,uint32_t c,H26xChannel *p) { ids(d,c);assert(!memcmp(p,golden().data(),76));calls.emplace_back("channel");return 0; };
    enc.destroy_channel=[](uint32_t d,uint32_t c) { ids(d,c);calls.emplace_back("destroy_channel");return 0; };
    enc.set_input_source=[](uint32_t d,uint32_t c,const uint32_t *p) { ids(d,c);assert(!*p);return 0; };
    enc.start_once=[](uint32_t d,uint32_t c,const int32_t *p) { ids(d,c);assert(*p==1);started=true;ready_packs=2;calls.emplace_back("start");return 0; };
    enc.stop=[](uint32_t d,uint32_t c) { ids(d,c);started=false;calls.emplace_back("stop");return 0; };
    enc.query=[](uint32_t d,uint32_t c,EncoderStatus *p) { ids(d,c);assert(started);p->current_packs=ready_packs;return 0; };
    enc.get_stream=[](uint32_t d,uint32_t c,EncoderStream *p,int32_t timeout) {
        ids(d,c);assert(timeout==100 && p->count==2 && p->packets_address==99);
        packs[0].data_address=100;packs[0].length=2;
        packs[1].data_address=200;packs[1].length=2;
        // JPEG uses no H26x end-of-frame flags. Keep the SDK output intact.
        p->handle=321;p->sequence=42;p->codec_information[3]=88;return 0;
    };
    enc.release_stream=[](uint32_t d,uint32_t c,EncoderStream *p) {
        ids(d,c);assert(p->count==2 && p->packets_address==99 && p->handle==321 && p->sequence==42 && p->codec_information[3]==88);
        calls.emplace_back("release");ready_packs=0;memset(packs,0,32*sizeof(*packs));return 0;
    };
    sys.sys_bind=[](uint16_t soc,Port *p,Port *q,uint32_t a,uint32_t b,uint32_t type,uint32_t param) {
        assert(!soc && p->module==Scl && p->device==0 && p->channel==src && !p->port);
        assert(q->module==Venc && q->device==8 && q->channel==ch && !q->port);
        assert(a==30 && b==30 && type==FrameBind && !param);calls.emplace_back("bind");return 0;
    };
    sys.sys_unbind=[](uint16_t soc,Port *p,Port *q) { assert(!soc && p->channel==src && !p->port && q->device==8 && q->channel==ch);calls.emplace_back("unbind");return 0; };
    memory.pointer_slot=[](const void *p,uint32_t *slot) { packs=(EncoderPacket*)p;*slot=99;return true; };
    memory.copy=[](uint32_t address,uint8_t *p,size_t bytes) { assert(bytes==2 && (address==100 || address==200));p[0]=0xff;p[1]=address==100?0xd8:bad?0:0xd9;return true; };
    for(ch=4;ch<=6;ch++)for(bool corrupt:{false,true}) {
        calls.clear();src=ch==5?1:0;device_owned=ch==4;bad=corrupt;
        EncoderOwner owner(sys,enc,memory);
        assert(owner.open_jpeg(src?EncoderSource::Wide:EncoderSource::Zoom,ch,ch==4?3840:ch==5?2560:640,ch==4?2160:ch==5?1440:512,device_owned,ch==6)==0);
        assert(!owner.ready());
        uint8_t bytes[32]{};EncodedFrame frame{};frame.data=bytes;frame.capacity=sizeof(bytes);
        assert(owner.copy_frame(frame)==-1 && !owner.failed_call); // no stopped Query
        assert(owner.start_once()==0 && owner.ready());
        assert(owner.copy_frame(frame)==(bad?-1:1));
        if(!bad) { uint32_t pending=99;assert(owner.pending_packs(pending)==0 && pending==0);assert(frame.bytes==4 && frame.sequence==42);assert(owner.stop_once()==0);assert(owner.copy_frame(frame)==-1 && !owner.failed_call);assert(owner.start_once()==0);assert(owner.stop_once()==0); }
        assert(owner.close()==0);assert(owner.close()==0);
        unsigned devices=0,bindings=0,releases=0;
        for(const auto &call:calls) { devices+=call=="destroy_device";bindings+=call=="unbind";releases+=call=="release"; }
        assert(devices==unsigned(device_owned) && bindings==unsigned(ch!=6) && releases==1);
    }
    puts("ZT30 JPEG: captured ABI, device8 ownership, all sources, one-shot reuse and malformed packet release PASS");
}
