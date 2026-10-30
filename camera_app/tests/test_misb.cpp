#include "camera_app/misb.h"
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Items = std::map<unsigned,std::vector<uint8_t>>;
static Items decode(const uint8_t *p, size_t n)
{
    assert(n >= 17 && !memcmp(p,"\x06\x0e\x2b\x34\x02\x0b\x01\x01\x0e\x01\x03\x01\x01\0\0\0",16));
    size_t at=16, length=p[at++];
    if (length&128) {
        unsigned bytes=length&127; length=0;
        while(bytes--) { assert(at<n); length=(length<<8)|p[at++]; }
    }
    assert(at+length==n);
    unsigned sum=0;
    for(size_t i=0;i<n-2;i++) sum+=unsigned(p[i])<<(i%2 ? 0 : 8);
    assert((sum&65535)==unsigned(p[n-2])*256+p[n-1]);
    assert(p[at]==2 && p[n-4]==1 && p[n-3]==2);
    Items result;
    while(at<n) {
        unsigned tag=p[at++], size=p[at++];
        assert(at+size<=n && !result.count(tag));
        result[tag]=std::vector<uint8_t>(p+at,p+at+size); at+=size;
    }
    assert(result.at(65)==std::vector<uint8_t>{8});
    return result;
}
static uint64_t integer(const Items &items,unsigned tag)
{
    uint64_t value=0;
    for(uint8_t byte:items.at(tag)) value=(value<<8)|byte;
    return value;
}
static double unsigned_angle(const Items &items,unsigned tag,double range)
{
    return integer(items,tag)*range/double((UINT64_C(1)<<(items.at(tag).size()*8))-1);
}
static double signed_angle(const Items &items,unsigned tag,double range)
{
    unsigned bits=items.at(tag).size()*8;
    uint64_t value=integer(items,tag);
    int64_t signed_value=value& (UINT64_C(1)<<(bits-1)) ? int64_t(value)-int64_t(UINT64_C(1)<<bits) : int64_t(value);
    return signed_value*range/double((UINT64_C(1)<<(bits-1))-1);
}

struct BitWriter {
    std::vector<uint8_t> data;
    unsigned used=0;
    void put(uint32_t value,unsigned bits) {
        while(bits--) {
            if(!(used%8)) data.push_back(0);
            data.back()|=((value>>bits)&1)<<(7-used%8); used++;
        }
    }
    void ue(unsigned value) {
        unsigned bits=0; for(unsigned n=value+1;n;n>>=1) bits++;
        put(0,bits-1); put(value+1,bits);
    }
};
static std::vector<uint8_t> parameter(ca_video_codec codec,unsigned type,unsigned id,unsigned version)
{
    BitWriter bits;
    if(codec==CA_VIDEO_H264 && type==7) bits.put(0x42001e,24);
    if(codec==CA_VIDEO_H265 && type==32) bits.put(id,4);
    else {
        if(codec==CA_VIDEO_H265 && type==33) {
            bits.put(1,8); // VPS 0, no temporal sublayers, temporal nesting.
            for(unsigned i=0;i<3;i++) bits.put(0,32); // profile_tier_level
        }
        bits.ue(id);
    }
    // Opaque parameter contents exercise repeated reconfiguration of one ID.
    bits.put(version,32); bits.put(1,1);
    std::vector<uint8_t> nal;
    if(codec==CA_VIDEO_H265) nal={uint8_t(type<<1),1};
    else nal={uint8_t(0x60|type)};
    unsigned zeros=0;
    for(uint8_t b:bits.data) {
        if(zeros==2 && b<=3) { nal.push_back(3); zeros=0; }
        nal.push_back(b); zeros=b==0 ? zeros+1 : 0;
    }
    return nal;
}
static int check_transport(void *opaque,const void *data,size_t length)
{
    auto &count=*static_cast<unsigned *>(opaque);
    assert(length==188);
    const auto *p=static_cast<const uint8_t *>(data);
    assert(p[0]==0x47);
    if(((p[1]&31)*256+p[2])!=0x102 || !(p[1]&0x40)) return 0;
    unsigned off=4+((p[3]&0x20) ? 1+p[4] : 0);
    assert(off+14+5<188 && p[off+3]==0xfc);
    const uint8_t *cell=p+off+9+p[off+8];
    assert(cell[2]>>6==3); // ISO 13818-1 Table 2-98: a complete metadata AU.
    assert(!(cell[2]&0x20) && (cell[2]&0x1f)==0x1f);
    count++; return 0;
}
static void transport_and_cache_tests()
{
    for(ca_video_codec codec:{CA_VIDEO_H264,CA_VIDEO_H265}) {
        ca_misb_ts mux;
        unsigned count=0;
        ca_metadata metadata{}; timespec utc{1700000000,0};
        const auto types=codec==CA_VIDEO_H264 ? std::vector<unsigned>{7,8} : std::vector<unsigned>{32,33,34};
        for(unsigned type:types) {
            auto nal=parameter(codec,type,1,1);
            assert(ca_video_cache_parameter_set(mux.headers,codec,nal.data(),nal.size())==1);
        }
        for(unsigned version=0;version<1500;version++) {
            for(unsigned type:types) {
                auto nal=parameter(codec,type,0,version);
                std::vector<uint8_t> au={0,0,0,1}; au.insert(au.end(),nal.begin(),nal.end());
                assert(ca_misb_ts_write(&mux,codec,au.data(),au.size(),version*4500,false,
                                       &metadata,&utc,check_transport,&count)==0);
                assert(ca_video_cache_parameter_set(mux.headers,codec,nal.data(),nal.size())==0);
            }
            assert(mux.headers.size()<512); // Old versions never accumulate.
        }
        size_t scan=0,offset,n; unsigned entries=0;
        while(ca_annexb_next(mux.headers.data(),mux.headers.size(),&scan,&offset,&n)) entries++;
        assert(entries==types.size()*2); // Distinct IDs remain available.
        auto before=mux.headers;
        uint8_t truncated[]={0x67,0x42};
        assert(ca_video_cache_parameter_set(mux.headers,CA_VIDEO_H264,truncated,sizeof(truncated))==0);
        assert(mux.headers==before);
        std::vector<uint8_t> idr=codec==CA_VIDEO_H264 ? std::vector<uint8_t>{0,0,0,1,0x65,0x80} :
                                                                      std::vector<uint8_t>{0,0,0,1,0x26,1,0x80};
        assert(ca_misb_ts_write(&mux,codec,idr.data(),idr.size(),0,false,
                               &metadata,&utc,check_transport,&count)==1);
        assert(count==1 && mux.started);
    }
    std::vector<uint8_t> cache;
    bool full=false;
    for(unsigned id=0;id<32;id++) {
        auto nal=parameter(CA_VIDEO_H264,8,id,0);
        nal.insert(nal.end(),4000,0x55);
        auto before=cache;
        int result=ca_video_cache_parameter_set(cache,CA_VIDEO_H264,nal.data(),nal.size());
        if(result<0) {
            assert(errno==EOVERFLOW && cache==before);
            full=true; break;
        }
        assert(result==1 && cache.size()<=65536);
    }
    assert(full);
    auto before=cache;
    auto oversized=parameter(CA_VIDEO_H264,8,0,0); oversized.resize(4097,0x55);
    assert(ca_video_cache_parameter_set(cache,CA_VIDEO_H264,oversized.data(),oversized.size())==0);
    assert(cache==before);
    puts("PASS complete metadata AU flags and bounded H.264/H.265 parameter-set replacement");
}
static void dual_metadata_tests()
{
    for(ca_video_codec codec:{CA_VIDEO_H264,CA_VIDEO_H265}) {
        ca_misb_ts mux; ca_metadata metadata{}; timespec utc{1700000000,123456000};
        metadata.have_vehicle_attitude=true;
        metadata.vehicle_roll_rad=2.617993878f; // Inverted, beyond even tag 91.
        metadata.vehicle_pitch_rad=.785398163f;
        metadata.hfov_deg=60; metadata.zoom=2;
        std::vector<uint8_t> packets;
        auto collect=[](void *opaque,const void *p,size_t n) {
            auto &out=*static_cast<std::vector<uint8_t> *>(opaque);
            const auto *bytes=static_cast<const uint8_t *>(p);
            out.insert(out.end(),bytes,bytes+n); return 0;
        };
        auto au=codec==CA_VIDEO_H264 ? std::vector<uint8_t>{0,0,0,1,0x65,0x80} :
                                           std::vector<uint8_t>{0,0,0,1,0x26,1,0x80};
        assert(ca_misb_ts_write(&mux,codec,au.data(),au.size(),90000,true,
                               &metadata,&utc,collect,&packets)==1);
        std::vector<uint8_t> video;
        for(size_t at=0;at<packets.size();at+=188) {
            const uint8_t *p=packets.data()+at;
            if(((p[1]&31)*256+p[2])!=0x101) continue;
            size_t off=4+((p[3]&0x20) ? 1+p[4] : 0);
            if(p[1]&0x40) { assert(p[off+3]==0xe0); off+=9+p[off+8]; }
            video.insert(video.end(),p+off,p+188);
        }
        size_t at=0,nal,n; unsigned found=0;
        bool saw_sei=false;
        while(ca_annexb_next(video.data(),video.size(),&at,&nal,&n)) {
            unsigned type=codec==CA_VIDEO_H264 ? video[nal]&31 : (video[nal]>>1)&63;
            if(ca_video_nal_is_vcl(codec,video[nal])) assert(saw_sei);
            if(type!=unsigned(codec==CA_VIDEO_H264 ? 6 : 39)) continue;
            saw_sei=true;
            std::vector<uint8_t> rbsp; unsigned zeros=0;
            for(size_t i=nal+(codec==CA_VIDEO_H264 ? 1 : 2);i<nal+n;i++) {
                uint8_t byte=video[i];
                if(zeros==2 && byte==3) { zeros=0; continue; }
                rbsp.push_back(byte); zeros=byte==0 ? zeros+1 : 0;
            }
            assert(rbsp[0]==5);
            size_t pos=1,size=0;
            while(rbsp[pos]==255) { size+=255; pos++; }
            size+=rbsp[pos++]; assert(pos+size<rbsp.size() && size>16);
            assert(!memcmp(rbsp.data()+pos,ca_video_metadata_uuid,16));
            std::string json(rbsp.begin()+pos+16,rbsp.begin()+pos+size);
            assert(json.find("\"vehicle_attitude\":{\"roll_rad\":2.617994")!=std::string::npos);
            assert(json.find("\"utc_us\":1700000000123456")!=std::string::npos);
            assert(json.find("\"pts90k\":90000")!=std::string::npos);
            found++;
        }
        assert(found==1);
    }
    puts("PASS MISB H.264/H.265 carries unrestricted APCG attitude with matching frame time before VCL");
}
int main()
{
    transport_and_cache_tests();
    dual_metadata_tests();
    ca_metadata m{};
    timespec utc{1700000000,123456000};
    uint8_t data[CA_MISB_KLV_MAX];
    auto packet=[&]() { size_t n=ca_misb_klv(&m,&utc,1920,1080,data,sizeof(data)); assert(n); return decode(data,n); };
    auto items=packet();
    assert(integer(items,2)==UINT64_C(1700000000123456));
    for(unsigned tag:{5,6,7,13,14,15,16,17,18,19,20,90,91}) assert(!items.count(tag));
    m.have_position=true; m.lat_e7=-353632610; m.lon_e7=1491652300;
    m.alt_amsl_m=620.25; m.alt_relative_m=9999;
    m.have_vehicle_attitude=m.have_gimbal_attitude=true;
    m.hfov_deg=88;
    items=packet();
    assert(fabs(signed_angle(items,13,90)+35.363261)<1e-7);
    assert(fabs(signed_angle(items,14,180)-149.16523)<1e-7);
    assert(fabs(unsigned_angle(items,15,19900)-900-620.25)<.16);
    assert(fabs(unsigned_angle(items,16,180)-88)<.002);
    assert(fabs(unsigned_angle(items,17,180)-57.021613)<.002);
    // Level earth-stabilized camera on a banked platform: relative roll
    // compensates the bank, even though gimbal roll feedback reads zero.
    constexpr double rad=3.14159265358979323846/180;
    m.vehicle_roll_rad=30*rad;
    items=packet();
    assert(fabs(unsigned_angle(items,20,360)-330)<.001);
    assert(fabs(signed_angle(items,19,180))<.001);
    m.vehicle_roll_rad=0; m.vehicle_pitch_rad=10*rad;
    m.gimbal_pitch_rad=-45*rad;
    items=packet();
    assert(fabs(signed_angle(items,19,180)+55)<.001);
    // Earth yaw wrap must not turn the vehicle-relative azimuth into earth yaw.
    m.vehicle_pitch_rad=0; m.vehicle_yaw_rad=350*rad;
    m.gimbal_pitch_rad=0; m.gimbal_yaw_rad=20*rad;
    items=packet();
    assert(fabs(unsigned_angle(items,18,360)-20)<.001);
    m.vehicle_pitch_rad=30*rad;
    items=packet();
    assert(integer(items,6)==0x8000); // Never clamp pitch into a plausible value.
    assert(fabs(signed_angle(items,90,90)-30)<.00001);
    m.vehicle_roll_rad=-80*rad;
    items=packet();
    assert(integer(items,7)==0x8000);
    assert(fabs(signed_angle(items,91,90)+80)<.00001);
    for(double sign:{-1.0,1.0}) {
        m.vehicle_pitch_rad=m.vehicle_roll_rad=float(sign*90*rad);
        items=packet();
        assert(fabs(signed_angle(items,90,90)-sign*90)<1e-7);
        assert(fabs(signed_angle(items,91,90)-sign*90)<1e-7);
    }
    m.vehicle_roll_rad=150*rad;
    items=packet();
    assert(integer(items,91)==0x80000000); // Never mislabel inverted roll.
    m.vehicle_pitch_rad=30*rad;
    m.gimbal_roll_rad=NAN; m.lat_e7=1000000000; m.alt_amsl_m=INFINITY;
    items=packet();
    for(unsigned tag:{13,15,18,19,20}) assert(!items.count(tag));
    assert(ca_misb_klv(&m,&utc,0,0,data,10)==0);
    utc.tv_nsec=1000000000;
    assert(ca_misb_klv(&m,&utc,1920,1080,data,sizeof(data))==0);
    // A full, non-NUL-terminated model must remain bounded.
    utc.tv_nsec=0; m={};
    m.have_position=m.have_vehicle_attitude=m.have_gimbal_attitude=true;
    m.hfov_deg=88; memset(m.model,'X',sizeof(m.model));
    items=packet(); assert(items.at(11).size()==sizeof(m.model));
    assert(!ca_misb_klv(&m,&utc,1920,1080,data,32));
    puts("PASS ST 0601 timestamps, BER, checksum, geographic units, FOV and stabilized-camera orientation");
}
