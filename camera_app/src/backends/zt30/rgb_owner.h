#pragma once
#include "media_graph.h"
#include "mi/api.h"
namespace ca_zt30 {
/* Single RGB path ownership primitive. Caller must establish exclusive device
 * ownership and load the pinned SDK first. It does not own application threads,
 * image pointers, encoders, USB, UART or the supervisor. SDK userspace3A
 * threads are acquired and released through the ISP channel lifecycle.
 * No heap allocation or SDK constructors here; every successful resource is
 * tracked, and cleanup continues after an error. No copy/double ownership.
 */
class RgbOwner {
public:
    explicit RgbOwner(Api &api):a(api) {}
    ~RgbOwner() { close(); }
    RgbOwner(const RgbOwner&)=delete;
    RgbOwner &operator=(const RgbOwner&)=delete;
    int open(bool wide=false,bool borrow_devices=false,unsigned scaler_mask=3);
    int load_iq(); // Only after external evidence proves frames are flowing.
    int close();
    const char *failed_call=nullptr;
    int raw_status=0;
    bool ready() const { return bound_scl; }
private:
    bool check(int result,const char *call);
    void cleanup(int result,const char *call,int &first_error);
    Api &a;
    const RgbPath *path=nullptr;
    bool unhealthy=false;
    bool sys=false,sensor=false,vif_group=false,vif_device=false,vif_port=false;
    bool isp_device=false,isp_channel=false,isp_started=false,isp_port=false;
    bool scl_device=false,scl_channel=false,scl_started=false,scl_port=false;
    bool bound_isp=false,bound_scl=false;
};
} // namespace ca_zt30
