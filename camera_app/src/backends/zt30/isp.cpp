#include "isp.h"
#include "camera_app/log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
namespace ca_zt30 {
static_assert(sizeof(ca_sstar_exposure_info)==572,"ZT30 exposure query copy572");
int ImageControls::read(unsigned channel,State &s)
{
    s={}; // The SDK fills some scalar values with 16-bit stores.
#define GET(field,member) if(!a.field || a.field(0,channel,&s.member)) { errno=EIO;return -1; }
    GET(get_brightness,brightness);GET(get_contrast,contrast);GET(get_saturation,saturation);
    GET(get_wb,wb);GET(get_ev,ev);GET(get_mode,mode);GET(get_manual,manual);
    GET(get_limit,limit);GET(get_metering,metering);
#undef GET
    return 0;
}
int ImageControls::write(unsigned channel,const State &before,State &after)
{
#define SET(field,member) do { if(std::memcmp(&before.member,&after.member,sizeof(after.member))) { \
    const int rc=a.field ? a.field(0,channel,&after.member) : -1; \
    if(rc) { ca_log("ZT30 ISP %s channel=%u failed: 0x%x",#field,channel,unsigned(rc));errno=EIO;return -1; } \
} } while(0)
    SET(set_brightness,brightness);SET(set_contrast,contrast);SET(set_saturation,saturation);
    SET(set_wb,wb);SET(set_ev,ev);SET(set_limit,limit);SET(set_manual,manual);
    SET(set_metering,metering);SET(set_mode,mode);
#undef SET
    return 0;
}
int ImageControls::initialize(unsigned mask,const ca_config &config)
{
    std::lock_guard<std::mutex> guard(lock);
    if(!mask || (mask&~3U) || channels) { errno=EINVAL;return -1; }
    for(unsigned channel=0;channel<2;channel++)if(mask&(1U<<channel)) {
        IspExposureLimit limit{};
        if(!a.get_limit || a.get_limit(0,channel,&limit) ||
           !limit.min_shutter || limit.min_shutter>limit.max_shutter ||
           limit.max_shutter>1000000 || limit.max_sensor_gain<1024 ||
           limit.max_isp_gain<1024) { errno=EIO;return -1; }
        defaults[channel]=limit;
    }
    channels=mask;
    if(apply_locked(config)) { channels=0;return -1; }
    return 0;
}
int ImageControls::apply(const ca_config &config)
{
    std::lock_guard<std::mutex> guard(lock);
    return apply_locked(config);
}
int ImageControls::apply_locked(const ca_config &config)
{
    if(!channels) { errno=ENODEV;return -1; }
    if(config.brightness<0 || config.brightness>100 || config.contrast<0 || config.contrast>100 ||
       config.saturation<0 || config.saturation>100 || config.exposure_compensation<-10 ||
       config.exposure_compensation>10 || unsigned(config.iso)>CA_ISO_3200 ||
       unsigned(config.shutter)>CA_SHUTTER_1_2000 || unsigned(config.metering)>CA_METERING_SPOT ||
       unsigned(config.white_balance)>CA_WB_INCANDESCENT) { errno=EINVAL;return -1; }
    // Two complete snapshots and two candidates live on the heap, not the
    // main control thread's stack. Snapshot every channel before the first Set.
    std::unique_ptr<State[]> transaction(new(std::nothrow) State[4]);
    if(!transaction) { errno=ENOMEM;return -1; }
    static const uint32_t shutter_us[]={0,33333,20000,10000,4000,2000,1333,1000,500};
    static const uint16_t white_balance[][4]={
        {0,0,0,0},{2149,1024,1024,1850},{2480,1024,1024,1550},
        {2341,1024,1024,1856},{1532,1024,1024,2220}};
    for(unsigned channel=0;channel<2;channel++)if(channels&(1U<<channel)) {
        auto &before=transaction[channel],&next=transaction[2+channel];
        if(read(channel,before))return -1;
        next=before;
        next.brightness.enabled=next.contrast.enabled=next.saturation.enabled=1;
        next.brightness.mode=next.contrast.mode=next.saturation.mode=1;
        next.brightness.manual=unsigned(config.brightness);
        next.contrast.manual=unsigned(config.contrast);
        // Native saturation is 32 at unity; the shared UI is neutral at 50.
        next.saturation.manual[0]=uint8_t((unsigned(config.saturation)*64U+50U)/100U);
        next.wb.mode=config.white_balance==CA_WB_AUTO?0:1;
        if(next.wb.mode)std::memcpy(next.wb.gains,white_balance[config.white_balance],sizeof(next.wb.gains));
        next.ev={config.exposure_compensation,10};
        next.metering=unsigned(config.metering);
        if(!configured || config.iso!=current.iso || config.shutter!=current.shutter) {
            next.limit=defaults[channel];
            const unsigned shutter=shutter_us[config.shutter];
            if(shutter) {
                next.manual.shutter_us=shutter;
                next.limit.min_shutter=std::min(next.limit.min_shutter,shutter);
                next.limit.max_shutter=std::max(next.limit.max_shutter,shutter);
            }
            if(config.iso!=CA_ISO_AUTO) {
                const uint32_t gain=1024U<<(unsigned(config.iso)-1);
                next.manual.sensor_gain=std::min(gain,next.limit.max_sensor_gain);
                next.manual.isp_gain=uint32_t(uint64_t(gain)*1024/next.manual.sensor_gain);
                next.limit.max_isp_gain=std::max(next.limit.max_isp_gain,next.manual.isp_gain);
            }
            next.mode=config.iso==CA_ISO_AUTO ? (shutter?3:0) : (shutter?4:2);
        }
    }
    unsigned attempted=0;
    for(unsigned channel=0;channel<2;channel++)if(channels&(1U<<channel)) {
        attempted|=1U<<channel;
        auto &next=transaction[2+channel];
        State observed;
        bool ok=write(channel,transaction[channel],next)==0 && read(channel,observed)==0;
        if(ok) {
            ok=observed.brightness.enabled==1 && observed.brightness.mode==1 &&
                observed.brightness.manual==next.brightness.manual &&
                observed.contrast.enabled==1 && observed.contrast.mode==1 &&
                observed.contrast.manual==next.contrast.manual &&
                observed.saturation.enabled==1 && observed.saturation.mode==1 &&
                observed.saturation.manual[0]==next.saturation.manual[0] &&
                observed.wb.mode==next.wb.mode &&
                (!next.wb.mode || !memcmp(observed.wb.gains,next.wb.gains,sizeof(next.wb.gains))) &&
                observed.ev.value==next.ev.value && observed.ev.denominator==next.ev.denominator &&
                observed.mode==next.mode && observed.metering==next.metering &&
                (config.iso==CA_ISO_AUTO || (observed.manual.sensor_gain==next.manual.sensor_gain &&
                                           observed.manual.isp_gain==next.manual.isp_gain)) &&
                (config.shutter==CA_SHUTTER_AUTO || observed.manual.shutter_us==next.manual.shutter_us);
            if(!ok)ca_log("ZT30 ISP readback differs for channel=%u",channel);
        }
        if(!ok) {
            // A failed setter may have changed its payload. Read back before
            // rolling every attempted sensor back, including the failed one.
            for(unsigned undo=0;undo<2;undo++)if(attempted&(1U<<undo)) {
                State actual;
                if(read(undo,actual) || write(undo,actual,transaction[undo]))
                    ca_log("ZT30 ISP rollback failed for channel=%u",undo);
            }
            errno=EIO;return -1;
        }
        ca_log("ZT30 ISP readback channel=%u mode=%u shutter_us=%u sensor_gain=%u isp_gain=%u wb=%u metering=%u",
            channel,observed.mode,observed.manual.shutter_us,observed.manual.sensor_gain,
            observed.manual.isp_gain,observed.wb.mode,observed.metering);
    }
    current=config;configured=true;
    ca_log("ZT30 ISP configured brightness=%d saturation=%d contrast=%d ev=%d iso=%d shutter=%d metering=%d white_balance=%d",
        config.brightness,config.saturation,config.contrast,config.exposure_compensation,
        config.iso,config.shutter,config.metering,config.white_balance);
    return 0;
}
int ImageControls::exposure(unsigned lens,ca_exposure *sample)
{
    std::lock_guard<std::mutex> guard(lock);
    const unsigned channel=lens==0?1:0;
    if(!sample || lens>1 || !(channels&(1U<<channel))) { errno=ENOTSUP;return -1; }
    ca_sstar_exposure_info info{};uint32_t mode=0;
    int rc=a.query?a.query(0,channel,&info):-1;
    if(rc) { errno=EIO;return rc; }
    ca_sstar_exposure_decode(sample,&info);
    rc=a.get_mode(0,channel,&mode);
    if(!rc)ca_sstar_exposure_mode(sample,mode);
    return rc;
}
}
