#pragma once
#include "mi/native_sdk.h"
#include "camera_app/overlay.h"
#include "camera_app/config.h"
#include <array>
namespace ca_zt30 {
// Main-thread-only region owner. RGB scaler channels and SYS outlive close().
class Overlay {
public:
    explicit Overlay(NativeSdk &sdk):s(sdk) {}
    int configure(unsigned source,bool external_main,const ca_config &);
    int apply(const ca_config &);
    int select_source(unsigned);
    int refresh(uint64_t now_ms);
    int close();
private:
    struct Region { bool created=false,attached=false;unsigned width=0,height=0; };
    int clear(unsigned index);
    int apply_with_fov(const ca_config &,float rgb_fov);
    NativeSdk &s;
    bool initialized=false,configured=false,external=false,external_sub=false;
    unsigned source=0;
    float applied_hfov=0;
    uint64_t refresh_after=0;
    ca_config settings{};
    std::array<Region,4*CA_OVERLAY_REGIONS> regions{};
};
}
