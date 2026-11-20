#pragma once
#include "mi/isp_abi.h"
#include "apcam/config.h"
#include "camera_app/sigmastar_exposure.h"
#include <mutex>
namespace ca_zt30 {
class ImageControls {
public:
    explicit ImageControls(ImageApi &api):a(api) {}
    int initialize(unsigned channel_mask,const ca_config &config);
    int apply(const ca_config &config);
    int exposure(unsigned lens,ca_exposure *sample);
private:
    struct State {
        IspLevel brightness{},contrast{};
        IspSaturation saturation{};
        IspWhiteBalance wb{};
        IspEv ev{};
        IspExposure manual{};
        IspExposureLimit limit{};
        uint32_t mode=0,metering=0;
    };
    ImageApi &a;
    std::mutex lock;
    unsigned channels=0;
    bool configured=false;
    ca_config current{};
    IspExposureLimit defaults[2]{};
    int read(unsigned,State&);
    int write(unsigned,const State&,State&);
    int apply_locked(const ca_config&);
};
}
