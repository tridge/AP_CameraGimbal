#include "camera_app/misb.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace {
constexpr unsigned video_pid=0x101, klv_pid=0x102, pmt_pid=0x1000;

/* SPS reader used only for displayed dimensions. Malformed/unsupported SPS
 * leaves geometry unknown; it cannot change the video bytes or mux bounds. */
struct Bits {
    std::vector<uint8_t> bytes;
    size_t bit=0;
    bool valid=true;
    Bits(const uint8_t *data,size_t n) {
        unsigned zeros=0;
        for(size_t i=0;i<n;i++) {
            if(zeros==2 && data[i]==3) { zeros=0; continue; }
            bytes.push_back(data[i]); zeros=data[i]==0 ? zeros+1 : 0;
        }
    }
    uint32_t get(unsigned n) {
        if(n>32 || bit+n>bytes.size()*8) { valid=false; return 0; }
        uint32_t value=0;
        while(n--) { value=(value<<1)|((bytes[bit/8]>>(7-bit%8))&1); bit++; }
        return value;
    }
    void skip(unsigned n) { while(n>32) { get(32); n-=32; } get(n); }
    uint32_t ue() {
        unsigned zeros=0;
        while(valid && !get(1)) if(++zeros>30) { valid=false; return 0; }
        return ((1U<<zeros)-1)+get(zeros);
    }
    int32_t se() { uint32_t n=ue(); return n&1 ? int32_t((n+1)/2) : -int32_t(n/2); }
};

int parameter_set_id(ca_video_codec codec,const uint8_t *nal,size_t n)
{
    const unsigned header=codec==CA_VIDEO_H265 ? 2 : 1;
    if(n<=header) return -1;
    const unsigned type=codec==CA_VIDEO_H265 ? (nal[0]>>1)&63 : nal[0]&31;
    if(codec==CA_VIDEO_H264 && type!=7 && type!=8) return -1;
    if(codec==CA_VIDEO_H265 && (type<32 || type>34)) return -1;
    Bits b(nal+header,n-header);
    unsigned id,limit;
    if(codec==CA_VIDEO_H264) {
        switch(nal[0]&31) {
        case 7: b.skip(24); id=b.ue(); limit=32; break;
        case 8: id=b.ue(); limit=256; break;
        default: return -1;
        }
    } else if(codec==CA_VIDEO_H265) {
        switch((nal[0]>>1)&63) {
        case 32: id=b.get(4); limit=16; break;
        case 33: {
            b.get(4); unsigned layers=b.get(3); b.get(1);
            if(layers>6) return -1;
            b.skip(96);
            unsigned profile[8]{},level[8]{};
            for(unsigned i=0;i<layers;i++) { profile[i]=b.get(1); level[i]=b.get(1); }
            if(layers) b.skip(2*(8-layers));
            for(unsigned i=0;i<layers;i++) { if(profile[i]) b.skip(88); if(level[i]) b.skip(8); }
            id=b.ue(); limit=16; break;
        }
        case 34: id=b.ue(); limit=64; break;
        default: return -1;
        }
    } else return -1;
    return b.valid && id<limit ? int(id) : -1;
}

void dimensions(ca_misb_ts &mux,ca_video_codec codec,const uint8_t *nal,size_t n)
{
    if(n<4 || n>4096) return;
    Bits b(nal+(codec==CA_VIDEO_H265 ? 2 : 1),n-(codec==CA_VIDEO_H265 ? 2 : 1));
    uint64_t w,h,left=0,right=0,top=0,bottom=0;
    unsigned chroma=1,separate=0,sx=2,sy=2;
    if(codec==CA_VIDEO_H265) {
        b.get(4); unsigned layers=b.get(3); b.get(1);
        b.skip(96); // general_profile_tier_level and general_level_idc
        unsigned profile[8]{},level[8]{};
        for(unsigned i=0;i<layers;i++) { profile[i]=b.get(1); level[i]=b.get(1); }
        if(layers) b.skip(2*(8-layers));
        for(unsigned i=0;i<layers;i++) { if(profile[i]) b.skip(88); if(level[i]) b.skip(8); }
        b.ue(); chroma=b.ue(); if(chroma==3) separate=b.get(1);
        w=b.ue(); h=b.ue();
        if(b.get(1)) { left=b.ue(); right=b.ue(); top=b.ue(); bottom=b.ue(); }
        sx=chroma==1 || chroma==2 ? 2 : 1;
        sy=chroma==1 ? 2 : 1;
        if(separate) sx=sy=1;
    } else {
        unsigned profile=b.get(8); b.skip(16); b.ue();
        if(profile==100 || profile==110 || profile==122 || profile==244 ||
           profile==44 || profile==83 || profile==86 || profile==118 ||
           profile==128 || profile==138 || profile==139 || profile==134 || profile==135) {
            chroma=b.ue(); if(chroma==3) separate=b.get(1);
            b.ue(); b.ue(); b.get(1);
            if(b.get(1)) for(unsigned i=0;i<(chroma==3 ? 12U : 8U);i++) {
                if(!b.get(1)) continue;
                int last=8,next=8;
                for(unsigned j=0;j<(i<6 ? 16U : 64U);j++) {
                    if(next) next=(last+int64_t(b.se())+256)%256;
                    if(next) last=next;
                }
            }
        }
        b.ue(); unsigned order=b.ue();
        if(order==0) b.ue();
        else if(order==1) {
            b.get(1); b.se(); b.se(); unsigned count=b.ue();
            if(count>256) return;
            while(count--) b.se();
        } else if(order>2) return;
        b.ue(); b.get(1); w=(uint64_t(b.ue())+1)*16; h=uint64_t(b.ue())+1;
        unsigned frame=b.get(1); h*=16*(2-frame);
        if(!frame) b.get(1);
        b.get(1);
        if(b.get(1)) { left=b.ue(); right=b.ue(); top=b.ue(); bottom=b.ue(); }
        unsigned array=separate ? 0 : chroma;
        sx=array==1 || array==2 ? 2 : 1;
        sy=(array==1 ? 2 : 1)*(2-frame);
    }
    if(!b.valid || chroma>3 || w<=sx*(left+right) || h<=sy*(top+bottom)) return;
    w-=sx*(left+right); h-=sy*(top+bottom);
    if(w>16384 || h>16384) return;
    mux.width=w; mux.height=h;
}

uint32_t crc(const uint8_t *p,size_t n)
{
    uint32_t value=0xffffffff;
    while(n--) {
        value^=uint32_t(*p++)<<24;
        for(unsigned i=0;i<8;i++) value=(value<<1)^((value&0x80000000) ? 0x04c11db7 : 0);
    }
    return value;
}

int section(ca_misb_ts &mux,unsigned pid,unsigned counter,std::vector<uint8_t> data,
             ca_misb_write_fn write,void *opaque)
{
    const size_t length=data.size()+4-3;
    data[1]=0xb0|(length>>8); data[2]=length;
    const uint32_t check=crc(data.data(),data.size());
    for(unsigned i=0;i<4;i++) data.push_back(check>>(24-8*i));
    if(data.size()>183) { errno=EOVERFLOW; return -1; }
    uint8_t packet[188]; memset(packet,0xff,sizeof(packet));
    packet[0]=0x47; packet[1]=0x40|(pid>>8); packet[2]=pid;
    packet[3]=0x10|(mux.continuity[counter]++&15); packet[4]=0;
    memcpy(packet+5,data.data(),data.size());
    return write(opaque,packet,sizeof(packet));
}

void put_pcr(uint8_t *out,uint64_t pts)
{
    pts&=UINT64_C(0x1ffffffff);
    out[0]=pts>>25; out[1]=pts>>17; out[2]=pts>>9; out[3]=pts>>1;
    out[4]=((pts&1)<<7)|0x7e; out[5]=0;
}

int pes(ca_misb_ts &mux,unsigned pid,unsigned counter,uint8_t id,
         const std::vector<uint8_t> &payload,uint64_t pts,bool key,
         ca_misb_write_fn write,void *opaque)
{
    uint8_t header[14]={0,0,1,id,0,0,0x84,0x80,5};
    if(id!=0xe0) { size_t n=payload.size()+8; header[4]=n>>8; header[5]=n; }
    uint64_t time=pts&UINT64_C(0x1ffffffff);
    header[9]=0x21|((time>>29)&0x0e); header[10]=time>>22;
    header[11]=1|((time>>14)&0xfe); header[12]=time>>7;
    header[13]=1|((time<<1)&0xfe);
    size_t at=0,total=sizeof(header)+payload.size();
    while(at<total) {
        uint8_t packet[188]; memset(packet,0xff,sizeof(packet));
        packet[0]=0x47; packet[1]=(pid>>8)|(at==0 ? 0x40 : 0); packet[2]=pid;
        packet[3]=0x10|(mux.continuity[counter]++&15);
        const bool pcr=at==0 && pid==video_pid;
        size_t n=std::min(total-at,size_t(pcr ? 176 : 184)),off=4;
        if(n<184) {
            packet[3]|=0x20; packet[4]=183-n; off=188-n;
            if(packet[4]) packet[5]=(pcr ? 0x10 : 0)|(key && !at ? 0x40 : 0);
            if(pcr) put_pcr(packet+6,pts);
        }
        for(size_t i=0;i<n;i++,at++) packet[off+i]=at<sizeof(header) ? header[at] : payload[at-sizeof(header)];
        if(write(opaque,packet,sizeof(packet))) return -1;
    }
    return 0;
}
}

int ca_video_cache_parameter_set(std::vector<uint8_t> &headers,ca_video_codec codec,
                                  const uint8_t *nal,size_t length)
{
    if(!nal || !length || length>4096) return 0;
    int id=parameter_set_id(codec,nal,length);
    if(id<0) return 0;
    const unsigned type=codec==CA_VIDEO_H265 ? (nal[0]>>1)&63 : nal[0]&31;
    size_t at=0,old,n,begin=headers.size(),end=begin;
    while(ca_annexb_next(headers.data(),headers.size(),&at,&old,&n)) {
        const unsigned old_type=codec==CA_VIDEO_H265 ? (headers[old]>>1)&63 : headers[old]&31;
        if(type!=old_type || id!=parameter_set_id(codec,headers.data()+old,n)) continue;
        if(n==length && !memcmp(headers.data()+old,nal,n)) return 0;
        begin=old-4; end=at; break; // Cache entries always use four-byte start codes.
    }
    if(headers.size()-(end-begin)+4+length>65536) { errno=EOVERFLOW; return -1; }
    headers.erase(headers.begin()+begin,headers.begin()+end);
    headers.insert(headers.begin()+begin,{0,0,0,1});
    headers.insert(headers.begin()+begin+4,nal,nal+length);
    return 1;
}

int ca_misb_ts_cache_nal(ca_misb_ts *mux,ca_video_codec codec,
                         const uint8_t *nal,size_t length)
{
    if(!mux) { errno=EINVAL; return -1; }
    int changed=ca_video_cache_parameter_set(mux->headers,codec,nal,length);
    if(changed<0) return changed;
    mux->headers_pending|=changed>0;
    if(nal && length) {
        const unsigned type=codec==CA_VIDEO_H265 ? (nal[0]>>1)&63 : nal[0]&31;
        if(type==unsigned(codec==CA_VIDEO_H265 ? 33 : 7)) dimensions(*mux,codec,nal,length);
    }
    return changed;
}

int ca_misb_ts_write(ca_misb_ts *state,ca_video_codec codec,
                      const uint8_t *data,size_t length,uint64_t pts,bool key,
                      const ca_metadata *metadata,const timespec *utc,
                      ca_misb_write_fn write,void *opaque)
{
    if(!state || !data || !length || !metadata || !utc || !write ||
       (codec!=CA_VIDEO_H264 && codec!=CA_VIDEO_H265)) { errno=EINVAL; return -1; }
    ca_misb_ts &mux=*state;
    size_t offset=0,nal,n;
    bool picture=false,random_access=false;
    while(ca_annexb_next(data,length,&offset,&nal,&n)) {
        if(!n) continue;
        const unsigned type=codec==CA_VIDEO_H265 ? (data[nal]>>1)&63 : data[nal]&31;
        picture|=ca_video_nal_is_vcl(codec,data[nal]);
        random_access|=codec==CA_VIDEO_H265 ? type>=16 && type<=21 : type==5;
        if(ca_misb_ts_cache_nal(&mux,codec,data+nal,n)<0) return -1;
    }
    key=random_access; // Encoder hints must not hide an IDR or invent one.
    if(!picture || (!mux.started && !key)) return 0;
    uint8_t klv[CA_MISB_KLV_MAX];
    size_t klv_size=ca_misb_klv(metadata,utc,mux.width,mux.height,klv,sizeof(klv));
    if(!klv_size) { errno=EINVAL; return -1; }
    // Preserve the complete APCG snapshot alongside standard KLV. MISB-only
    // viewers can ignore this unregistered SEI; our tools recover unrestricted
    // attitude, sample ages and other fields at the same precision as APCG.
    char json[CA_VIDEO_METADATA_JSON_MAX];
    uint8_t sei[CA_VIDEO_METADATA_SEI_MAX];
    size_t json_size=ca_video_metadata_json(metadata,utc,pts,json,sizeof(json));
    size_t sei_size=ca_video_metadata_sei(codec,json,json_size,sei,sizeof(sei));
    if(!sei_size) { errno=EOVERFLOW; return -1; }
    // Program 1: video + synchronous metadata (stream_type 0x15, stream_id
    // 0xfc). Declarations and AU cells follow MISB ST 1402, not RP 217.
    if(section(mux,0,0,{0,0,0,0,1,0xc1,0,0,0,1,0xf0,0},write,opaque)) return -1;
    const uint8_t descriptors[]={
        0x05,4,'K','L','V','A',
        0x26,9,0x01,0x00,0xff,'K','L','V','A',0,0x0f,
        // 256 kbit/s input budget, 4 KiB metadata buffer, unspecified output.
        0x27,9,0xc0,0x02,0x80,0xc0,0x00,0x04,0xc0,0x00,0x00,
    };
    std::vector<uint8_t> pmt={2,0,0,0,1,0xc1,0,0,0xe1,0x01,0xf0,0,
        uint8_t(codec==CA_VIDEO_H265 ? 0x24 : 0x1b),0xe1,0x01,0xf0,0,
        0x15,0xe1,0x02,0xf0,uint8_t(sizeof(descriptors))};
    pmt.insert(pmt.end(),descriptors,descriptors+sizeof(descriptors));
    if(section(mux,pmt_pid,1,pmt,write,opaque)) return -1;
    // PCR-only packets bound the PCR interval for low-rate thermal streams.
    // A source timestamp discontinuity starts a new clock at the next frame.
    if(mux.started && pts>mux.last_pts && pts-mux.last_pts<90000)
        for(uint64_t clock=mux.last_pts+1800;clock+1800<pts;clock+=1800) {
            uint8_t packet[188]; memset(packet,0xff,sizeof(packet));
            packet[0]=0x47; packet[1]=video_pid>>8; packet[2]=video_pid&255;
            packet[3]=0x20|((mux.continuity[2]-1)&15);
            packet[4]=183; packet[5]=0x10; put_pcr(packet+6,clock);
            if(write(opaque,packet,sizeof(packet))) return -1;
        }
    std::vector<uint8_t> video;
    {
        const uint8_t h264_aud[]={0,0,0,1,9,0xf0},h265_aud[]={0,0,0,1,70,1,0x50};
        if(codec==CA_VIDEO_H265) video.insert(video.end(),h265_aud,h265_aud+sizeof(h265_aud));
        else video.insert(video.end(),h264_aud,h264_aud+sizeof(h264_aud));
    }
    if(key || mux.headers_pending) video.insert(video.end(),mux.headers.begin(),mux.headers.end());
    offset=0;
    bool inserted=false;
    while(ca_annexb_next(data,length,&offset,&nal,&n)) {
        if(!n) continue;
        const unsigned type=codec==CA_VIDEO_H265 ? (data[nal]>>1)&63 : data[nal]&31;
        if(type==unsigned(codec==CA_VIDEO_H265 ? 35 : 9)) continue;
        if(!inserted && ca_video_nal_is_vcl(codec,data[nal])) {
            video.insert(video.end(),sei,sei+sei_size); inserted=true;
        }
        video.insert(video.end(),{0,0,0,1});
        video.insert(video.end(),data+nal,data+nal+n);
    }
    // Each cell is complete, independently usable, and associated with the
    // same video PTS. Sequence numbers wrap modulo 256.
    std::vector<uint8_t> cell(5+klv_size);
    cell[0]=0; cell[1]=mux.sequence++; cell[2]=0xdf;
    cell[3]=klv_size>>8; cell[4]=klv_size;
    memcpy(cell.data()+5,klv,klv_size);
    if(pes(mux,klv_pid,3,0xfc,cell,pts,true,write,opaque) ||
       pes(mux,video_pid,2,0xe0,video,pts,key,write,opaque)) return -1;
    mux.started=true; mux.last_pts=pts;
    mux.headers_pending=false;
    return 1;
}
