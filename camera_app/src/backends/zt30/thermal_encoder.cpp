#include "thermal_encoder.h"
#include <cstring>
#include <mutex>
#include "camera_app/log.h"
static std::mutex input_lease_lock;
namespace ca_zt30 {
thread_local ThermalEncoder *ThermalEncoder::active=nullptr;
ThermalEncoder::ThermalEncoder(Api &sys,EncoderApi &encoder,PacketMemory &packets,InputBufferApi buffers,uint32_t device,uint32_t channel):
    output(sys,encoder,packets),sdk(buffers),input_port{{2,device,channel,0}},input({get,put},{map,copy},input_port) {}
bool ThermalEncoder::maps(std::vector<InputMapping> &out) {
    FILE *file=std::fopen("/proc/self/maps","r");if(!file)return false;
    bool ok=read_input_mappings(file,out);std::fclose(file);return ok;
}
int ThermalEncoder::get(InputPort *port,InputBufferConfig *config,InputBufferInfo *info,uint32_t *handle,int32_t timeout) {
    auto &self=*active;self.writable=false;
    std::vector<InputMapping> before,leased;
    if(!self.sdk.get || !self.sdk.put || !maps(before))return -1;
    const int rc=self.sdk.get(port,config,info,handle,timeout);
    if(rc)return rc; // failed outputs must never be inspected/released
    self.leased=*info;
    const bool parsed=maps(leased);
    self.writable=parsed && new_input_mapping(before,leased,*info);
    if(!self.writable) {
        // Failure-only bounded evidence. Never dereference SDK plane addresses.
        uint32_t bytes=0;std::memcpy(&bytes,reinterpret_cast<const uint8_t*>(info)+108,4);
        ca_log("ZT30 input mapping rejected: device=%u channel=%u parsed=%u before=%zu after=%zu Y=%08x UV=%08x bytes=%u",
            port->words[1],port->words[2],unsigned(parsed),before.size(),leased.size(),
            info->plane_address[0],info->plane_address[1],bytes);
        const uint64_t first=info->plane_address[0],last=first+bytes;
        for(const auto &m:before)if(first<m.last && m.first<last)
            ca_log("ZT30 input before overlap: %08llx-%08llx shared_zero=%u",
                (unsigned long long)m.first,(unsigned long long)m.last,unsigned(m.shared_zero));
        for(const auto &m:leased)if(first<m.last && m.first<last)
            ca_log("ZT30 input after overlap: %08llx-%08llx shared_zero=%u",
                (unsigned long long)m.first,(unsigned long long)m.last,unsigned(m.shared_zero));
        const auto *raw=reinterpret_cast<const uint8_t*>(info);char hex[65];
        for(size_t offset=0;offset<sizeof(*info);offset+=32) {
            size_t n=0;for(;n<32 && offset+n<sizeof(*info);++n)
                std::snprintf(hex+n*2,3,"%02x",raw[offset+n]);
            ca_log("ZT30 input info %03zu: %s",offset,hex);
        }
    }
    return rc; // validation failure is discarded exactly once by InputBufferOwner
}
int ThermalEncoder::put(uint32_t handle,InputBufferInfo *info,uint8_t flag) {
    auto &self=*active;self.writable=false;
    return self.sdk.put(handle,info,flag);
}
bool ThermalEncoder::map(uint32_t address,size_t bytes,uint8_t **pointer) {
    auto &self=*active;uint32_t allocation=0;
    std::memcpy(&allocation,reinterpret_cast<const uint8_t*>(&self.leased)+108,4);
    if(sizeof(uintptr_t)!=4 || !self.writable || address!=self.leased.plane_address[0] || bytes!=allocation)return false;
    if(self.probe_only) { self.probe_valid=true;return false; }
    *pointer=reinterpret_cast<uint8_t*>(uintptr_t(address));return true;
}
bool ThermalEncoder::copy(uint8_t *destination,size_t stride,const uint8_t *source,size_t bytes,size_t rows) {
    for(size_t row=0;row<rows;++row)std::memcpy(destination+row*stride,source+row*bytes,bytes);
    return true;
}
bool ThermalEncoder::probe_input_discarded() {
    if(active || !output.ready())return false;
    std::lock_guard<std::mutex> guard(input_lease_lock);
    std::vector<uint8_t> ignored(ThermalNv12Bytes);
    // Deliberate MapFailed poisons an InputBufferOwner. Keep this disposable
    // probe separate from the owner which will submit real photographs.
    InputBufferOwner probe({get,put},{map,copy},input_port);
    active=this;probe_only=true;probe_valid=false;
    const auto result=probe.submit(ignored.data(),ignored.size(),0,100);
    probe_only=false;active=nullptr;
    return result==InputBufferResult::MapFailed && probe_valid && input.healthy();
}
InputBufferResult ThermalEncoder::submit(const uint8_t *frame,size_t bytes,uint64_t pts) {
    if(active || !output.ready())return InputBufferResult::Unhealthy;
    // Keep the before/Get/after mapping proof atomic across both encoders.
    std::lock_guard<std::mutex> guard(input_lease_lock);
    active=this;
    const auto result=input.submit(frame,bytes,pts,100);
    active=nullptr;return result;
}
}
