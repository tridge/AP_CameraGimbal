#include "../camera_app/src/backends/zt30/pts_clock.h"
#include <cassert>
#include <initializer_list>
using ca_zt30::PtsClock;
int main() {
    const uint64_t seed=UINT64_C(0xe1237890f7e21ce3), ns=UINT64_C(2000000000000);
    uint64_t out=99;
    PtsClock c;
    assert(c.observe(seed,ns,ns+100000));
    assert(c.frame(seed-33333,ns/1000+50,out) && out==ns/1000+50-33333);
    assert(c.observe(seed+33333,ns+33333000,ns+33433000));
    assert(c.frame(seed,ns/1000+33383,out) && out==ns/1000+50);
    assert(c.frame(seed+1,ns/1000+33383,out) && out==ns/1000+51);
    assert(!c.frame(seed+1,ns/1000+33383,out) && out==0 && !c.healthy());
    assert(!c.observe(seed+66666,ns+66666000,ns+66766000));
    PtsClock reset;assert(reset.observe(seed,ns,ns));
    assert(!reset.observe(0,ns+1000000,ns+1000000));
    PtsClock jumped;assert(jumped.observe(seed,ns,ns));
    assert(!jumped.observe(seed+1000000,ns+1000000,ns+1000000));
    PtsClock stale;assert(stale.observe(seed,ns,ns));
    assert(!stale.frame(seed-500001,ns/1000,out));
    PtsClock future;assert(future.observe(seed,ns,ns));
    assert(!future.frame(seed+2001,ns/1000,out));
    PtsClock underflow;assert(underflow.observe(9999999,1000,1000));
    assert(!underflow.frame(0,1,out));
    PtsClock overflow;assert(overflow.observe(0,ns,ns));
    assert(!overflow.frame(UINT64_MAX,ns/1000,out));
    PtsClock wrap;assert(wrap.observe(UINT64_MAX-10,ns,ns));
    assert(!wrap.observe(10,ns+20000,ns+20000));
    PtsClock boundary;assert(boundary.observe(seed,ns,ns+2000000));
    assert(boundary.frame(seed-500000,ns/1000+1000,out)); // exactly500ms
    PtsClock future_edge;assert(future_edge.observe(seed,ns,ns));
    assert(future_edge.frame(seed+2000,ns/1000,out));
    PtsClock tolerance;assert(tolerance.observe(seed,ns,ns));
    assert(tolerance.observe(seed+5001,ns+1000,ns+1000)); // exactly5ms
    PtsClock equal;assert(equal.observe(seed,ns,ns));
    assert(!equal.observe(seed,ns+1000,ns+1000));
    PtsClock slow;assert(!slow.observe(seed,ns,ns+2000001));
    PtsClock reverse;assert(!reverse.observe(seed,ns+1,ns));
    PtsClock uncalibrated;assert(!uncalibrated.frame(seed,ns/1000,out));
    PtsClock drift;assert(drift.observe(seed,ns,ns));
    assert(drift.observe(seed+1001000,ns+1000000000,ns+1000000000));
    assert(!drift.observe(seed+2010000,ns+2000000000,ns+2000000000));
    // Eight hours at 30fps, with independent clocks at either drift limit.
    // The original fixed offset falsely rejected even +10ppm after ~200s.
    for (int ppm : {-1000,-100,-10,0,10,100,1000}) {
        PtsClock running;
        assert(running.observe(seed,ns,ns));
        uint64_t previous=0;
        for(uint64_t n=1;n<=8U*60*60*30;n++) {
            const uint64_t elapsed=n*1000000/30;
            const int64_t correction=int64_t(elapsed)*ppm/1000000;
            const uint64_t pts=seed+elapsed+correction;
            const uint64_t now=ns+elapsed*1000;
            assert(running.observe(pts,now,now+100000));
            assert(running.frame(pts-1000,now/1000+5000,out));
            assert(out>previous);previous=out;
        }
        // Even after eight hours, an abrupt 10ms clock step must fail.
        const uint64_t elapsed=8ULL*60*60*1000000+1000000;
        const uint64_t pts=seed+elapsed+int64_t(elapsed)*ppm/1000000+10000;
        assert(!running.observe(pts,ns+elapsed*1000,ns+elapsed*1000));
    }
    // A scheduling overrun must not poison the clock before a precise retry.
    ca_zt30::PtsObservation observation;
    unsigned reads=0,ticks=0;
    const uint64_t times[]={ns,ns+2000001,ns+3000000,ns+3050000};
    PtsClock retried;
    assert(ca_zt30::observe_pts_clock(retried,[&](uint64_t *pts) { *pts=seed+reads++*3000;return 0; },
        [&]() { assert(ticks<4);return times[ticks++]; },observation));
    assert(reads==2 && observation.attempts==2 && retried.healthy());
    assert(retried.frame(seed+3001,(ns+3050000)/1000,out));
    // Exhaustion is bounded; an actual reset/error is never retried.
    reads=ticks=0;PtsClock busy;
    assert(!ca_zt30::observe_pts_clock(busy,[&](uint64_t *pts) { *pts=seed;reads++;return 0; },
        [&]() { return ns+(ticks++)*3000000; },observation));
    assert(reads==3 && observation.attempts==3 && !busy.healthy());
    reads=ticks=0;
    assert(!ca_zt30::observe_pts_clock(retried,[&](uint64_t *pts) { *pts=0;reads++;return 0; },
        [&]() { return ns+10000000+(ticks++)*1000; },observation));
    assert(reads==1 && !retried.healthy());
    reads=ticks=0;PtsClock sdk_failed;
    assert(!ca_zt30::observe_pts_clock(sdk_failed,[&](uint64_t*) { reads++;return -7; },
        [&]() { return ns+(ticks++)*1000; },observation));
    assert(reads==1 && observation.status==-7 && !sdk_failed.healthy());
}
