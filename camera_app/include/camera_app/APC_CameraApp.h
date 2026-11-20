#pragma once
#include "apcam/target.h"
#include "camera_app/manual_control.h"
#include "camera_app/APC_NetworkCapture.h"

struct ca_siyi_server;
struct ca_xfrobot_server;
struct ca_media;
struct ca_unigcs;

// Owns the services and their shutdown order; driver/media workers stop before
// the diagnostic logger closes. Partial startup follows the same cleanup path.
class APC_CameraApp {
public:
    APC_CameraApp() { _manual.fd = -1; }
    ~APC_CameraApp();
    APC_CameraApp(const APC_CameraApp &) = delete;
    APC_CameraApp &operator=(const APC_CameraApp &) = delete;
    int run(int argc, char **argv);

private:
    ca_siyi_server *_server = nullptr;
    ca_unigcs *_unigcs = nullptr;
#if APCAM_HAVE_XFROBOT
    ca_xfrobot_server *_xfrobot = nullptr;
#endif
    ca_mavlink_server *_mavlink_server = nullptr;
    ca_manual_control _manual {};
    ca_backend *_backend = nullptr;
    ca_media *_media = nullptr;
    APC_NetworkCapture _network_capture;
    bool _started = false;
};
