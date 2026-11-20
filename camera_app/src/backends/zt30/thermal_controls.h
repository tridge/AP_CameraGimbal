#pragma once
#include "usb_iso.h"
#include <mutex>
#include <atomic>
namespace ca_zt30 {
// Qualified module palette protocol, independent of ISO/bulk video transport.
// attach/detach serialize against all controls before the USB owner closes fd.
class ThermalControls {
public:
    explicit ThermalControls(UsbIsoApi api):a(api) {}
    void attach(int descriptor) { std::lock_guard<std::mutex> g(lock);fd=descriptor; }
    void detach() { std::lock_guard<std::mutex> g(lock);fd=-1; }
    uint8_t cached_palette() const { return cached.load(); }
    int palette(uint8_t *value);
    int set_palette(uint8_t value);
private:
    int transfer(uint8_t type,uint8_t request,uint16_t index,void *,unsigned);
    int read_palette(uint8_t *);
    int write_palette(uint8_t);
    UsbIsoApi a;
    int fd=-1;
    std::atomic<uint8_t> cached{255};
    std::mutex lock;
};
}
