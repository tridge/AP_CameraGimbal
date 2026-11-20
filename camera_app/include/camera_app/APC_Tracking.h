#pragma once
#include "camera_app/image_tracker.h"
#include "camera_app/tracking_pose.h"
#include "camera_app/tracking_rate.h"
#include "apcam/APC_Resource.h"
class APC_Media_Backend;
struct ca_backend;

// Stable owner, joined before the media backend is replaced. Frame acquisition
// and correlation never run on the network/gimbal control thread.
class APC_Tracking {
public:
    ~APC_Tracking() { close(); }
    bool open(APC_Media_Backend *media);
    void close();
    bool available() const { return _running; }
    int start(ca_tracking_rect rect, ca_tracking_owner owner);
    void stop(ca_tracking_owner owner=CA_TRACK_OWNER_NONE);
    ca_tracking_status status();
    void update(ca_backend *gimbal, bool manual, float hfov);
private:
    void run();
    APC_Media_Backend *_media=nullptr;
    ca_backend *_gimbal=nullptr;
    ca_image_tracker *_engine=nullptr;
    APC_Mutex _lock;
    APC_Condition _wake;
    pthread_t _thread {};
    bool _running=false, _quit=false, _driving=false;
    unsigned _generation=0;
    uint64_t _command_ms=0;
    float _integral[2] {};
    ca_tracking_rate_pulses _rate_pulses[2];
    ca_tracking_status _status;
    ca_tracking_pose_history _poses;
};
