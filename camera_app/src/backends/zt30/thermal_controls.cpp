#include "thermal_controls.h"
#include <cerrno>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
namespace ca_zt30 {
int ThermalControls::transfer(uint8_t type,uint8_t request,uint16_t index,void *data,unsigned bytes) {
    if(fd<0 || !a.ioctl) { errno=ENODATA;return -1; }
    usbdevfs_ctrltransfer command{};command.bRequestType=type;command.bRequest=request;
    command.wValue=0x78;command.wIndex=index;command.wLength=bytes;command.timeout=250;command.data=data;
    const int rc=a.ioctl(a.opaque,fd,USBDEVFS_CONTROL,&command);
    if(rc==int(bytes))return 0;
    if(rc>=0)errno=EPROTO;
    return -1;
}
int ThermalControls::read_palette(uint8_t *value) {
    uint8_t command[8]={9,0x84,0,0,0,0,0,1},result=0;
    if(transfer(0x41,0x45,0x1d00,command,8) || transfer(0xc1,0x44,0x1d08,&result,1))return -1;
    if(result<1 || result>12 || result==2) { errno=EPROTO;return -1; }
    *value=result-1;cached.store(*value);return 0;
}
int ThermalControls::write_palette(uint8_t value) {
    uint8_t command[8]={9,0xc4,0,0,0,0,0,1},module=value+1;
    return transfer(0x41,0x45,0x9d00,command,8) || transfer(0x41,0x45,0x1d08,&module,1) ? -1:0;
}
int ThermalControls::palette(uint8_t *value) {
    if(!value) { errno=EINVAL;return -1; }
    std::lock_guard<std::mutex> g(lock);
    return read_palette(value);
}
int ThermalControls::set_palette(uint8_t value) {
    if(value>11 || value==1) { errno=EINVAL;return -1; }
    std::lock_guard<std::mutex> g(lock);
    uint8_t before=0,observed=0;
    if(read_palette(&before))return -1;
    if(before==value)return 0;
    int result=write_palette(value);
    if(!result) {
        for(unsigned attempt=0;attempt<4;attempt++) {
            if(a.pause_ms)a.pause_ms(a.opaque,5);
            result=read_palette(&observed);
            if(!result && observed==value)return 0;
            if(result)break;
        }
        if(!result)errno=EIO;
    }
    const int saved=errno;
    (void)write_palette(before); // Restore even a partially staged module write.
    errno=saved;return -1;
}
}
