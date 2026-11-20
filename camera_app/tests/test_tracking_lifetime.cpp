// A failed worker start must not retain a backend across reconfiguration.
#include "camera_app/APC_Tracking.h"
#include "camera_app/backend.h"
#include "camera_app/metadata.h"
#include "test_media_backend.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static bool fail_thread;
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*f)(void *), void *p)
{ return fail_thread ? EAGAIN : __real_pthread_create(t,a,f,p); }
class Backend final : public TestMediaBackend {
    bool tracking_available() const override { return true; }
};
void ca_log(const char *, ...) {}
bool ca_binlog_active() { return false; }
uint64_t ca_binlog_time_us() { return 0; }
void ca_binlog_emit(uint8_t, const void *, size_t) {}
void ca_metadata_snapshot(ca_metadata *m) { *m={}; }
bool ca_backend_gimbal_attitude(const ca_backend *, ca_gimbal_attitude *) { return false; }
int ca_backend_set_gimbal_rates(ca_backend *, float, float) { return 0; }
int ca_backend_request_gimbal_attitude(ca_backend *) { return 0; }
int main()
{
    APC_Tracking tracker;
    for (bool fail : {true,false,true,false}) {
        auto *backend=new Backend;
        fail_thread=fail;
        assert(tracker.open(backend)==!fail);
        assert(tracker.available()==!fail);
        if (!fail) tracker.close();
        delete backend;
        // ASan catches the virtual call through a retained backend here.
        tracker.update(nullptr,false,88);
        tracker.close();
        assert(!tracker.available());
    }
    puts("PASS tracking worker failure, backend replacement and close lifetime");
}
