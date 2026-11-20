#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
struct usbdevfs_urb;
namespace ca_zt30 {
bool thermal_usb_descriptors(const uint8_t*,size_t);
constexpr unsigned ThermalPackets=32,ThermalPacketBytes=3072,ThermalUrbs=8;
constexpr unsigned ThermalUrbBytes=ThermalPackets*ThermalPacketBytes;
constexpr unsigned ThermalFrameBytes=1310720;
struct UsbIsoApi {
    void *opaque;
    int (*ioctl)(void*,int,unsigned long,void*);
    int (*close)(void*,int);
    uint64_t (*now)(void*);
    void (*pause_ms)(void*,unsigned);
};
struct IsoPacket {
    uint64_t submission,received_ns;
    unsigned index;
    int status;
    const uint8_t *data;
    unsigned bytes;
};
class UsbIso {
public:
    // Takes sole fd ownership. No open/detach/reset/SDK operations are performed.
    UsbIso(int fd,const UsbIsoApi &api):fd_(fd),api_(api) {}
    ~UsbIso();
    UsbIso(const UsbIso&)=delete;UsbIso& operator=(const UsbIso&)=delete;
    using Sink=bool(*)(void*,const IsoPacket&);
    bool start();
    // 1=consumed/requeued, 0=EAGAIN, -1=failure. Sink borrows bytes only during call.
    int step(Sink sink,void *opaque);
    bool finish();
    const uint8_t *get_max() const { return maximum_; }
    const uint8_t *negotiated() const { return negotiated_; }
    struct Statistics {
        uint64_t completions=0,empty_completions=0,bytes=0,data_packets=0,max_reap_gap_ns=0;
        int alt_zero=0,release_control=0,release_stream=0;
    };
    // Read on the capture thread or after join; no payloads or per-packet logs.
    const Statistics &statistics() const { return stats_; }
    uint64_t packet_errors() const { return packet_errors_; }
    bool close_fallback() const { return fallback_; }
private:
    struct Entry {
        std::unique_ptr<uint8_t[]> storage,buffer;
        usbdevfs_urb *urb=nullptr;
        uint64_t sequence=0;
        bool submitted=false;
    };
    int fail() { active_=false;return -1; }
    bool transfer(uint8_t type,uint8_t request,uint16_t selector,uint8_t *data);
    bool submit(Entry &entry);
    Entry *find(usbdevfs_urb *pointer);
    unsigned pending() const;
    int fd_;
    UsbIsoApi api_;
    Entry entries_[ThermalUrbs];
    bool claims_[2]{},active_=false,alt_=false,started_=false,finished_=false,cleanup_ok_=true,fallback_=false;
    uint64_t next_=1,reaped_=1,packet_errors_=0,last_reap_ns_=0;
    Statistics stats_;
    uint8_t maximum_[26]{},negotiated_[26]{};
};
}
