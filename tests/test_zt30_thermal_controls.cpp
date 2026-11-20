#include "camera_app/src/backends/zt30/thermal_controls.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <initializer_list>
using namespace ca_zt30;
static unsigned calls=0,failure=0;
static uint8_t module_palette=1;
static bool ignore_writes=false,reading=false,writing=false;
static int control(void*,int fd,unsigned long op,void *data) {
    assert(fd==7 && op==USBDEVFS_CONTROL);
    auto &c=*static_cast<usbdevfs_ctrltransfer*>(data);
    assert(c.wValue==0x78 && c.timeout==250);
    auto *p=static_cast<uint8_t*>(c.data);
    ++calls;if(calls==failure) { errno=EIO;return -1; }
    if(c.bRequestType==0x41 && c.bRequest==0x45 && c.wIndex==0x1d00) {
        const uint8_t expected[8]={9,0x84,0,0,0,0,0,1};
        assert(c.wLength==8 && !memcmp(p,expected,8));reading=true;
    } else if(c.bRequestType==0xc1 && c.bRequest==0x44 && c.wIndex==0x1d08) {
        assert(reading && c.wLength==1);reading=false;*p=module_palette;
    } else if(c.bRequestType==0x41 && c.bRequest==0x45 && c.wIndex==0x9d00) {
        const uint8_t expected[8]={9,0xc4,0,0,0,0,0,1};
        assert(c.wLength==8 && !memcmp(p,expected,8));writing=true;
    } else {
        assert(writing && c.bRequestType==0x41 && c.bRequest==0x45 && c.wIndex==0x1d08 && c.wLength==1);
        writing=false;if(!ignore_writes)module_palette=*p;
    }
    return c.wLength;
}
int main() {
    ThermalControls module({nullptr,control,nullptr,nullptr,[](void*,unsigned ms){assert(ms==5);}});
    uint8_t value=255;
    assert(module.palette(&value)<0 && errno==ENODATA && !calls);
    module.attach(7);
    assert(!module.palette(&value) && value==0);
    unsigned n=calls;assert(!module.set_palette(0) && calls==n+2);
    for(uint8_t palette:{uint8_t(2),uint8_t(3),uint8_t(4),uint8_t(10),uint8_t(11),uint8_t(0)}) {
        assert(!module.set_palette(palette));assert(!module.palette(&value) && value==palette);
    }
    n=calls;assert(module.set_palette(1)<0 && errno==EINVAL && calls==n);
    assert(module.set_palette(12)<0 && calls==n);
    for(unsigned step=1;step<=6;step++) {
        calls=0;failure=step;module_palette=1;reading=writing=false;
        assert(module.set_palette(3)<0);
        // Failed read does not touch module state; failed write/readback restores it.
        assert(module_palette==1);
    }
    failure=0;ignore_writes=true;
    assert(module.set_palette(3)<0 && errno==EIO && module_palette==1);
    n=calls;
    const auto cached=module.cached_palette();
    assert(cached!=255 && calls==n);
    module.detach();n=calls;
    assert(module.cached_palette()==cached && calls==n);
    assert(module.palette(&value)<0 && errno==ENODATA && calls==n);
    puts("PASS ZT30 module palette: wire protocol, readback, partial-write rollback, invalid/stale controls");
}
