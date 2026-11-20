#pragma once
#include <cstdint>
#include <limits>
namespace ca_zt30 {
/* MI SYS PTS is an unsigned microsecond clock which can inherit a vendor seed
 * greater than INT64_MAX. Subtract integer origins before any conversion.
 * Observations bracket GetCurPts; they do not turn receipt time into capture
 * time. This single-use clock fails closed on resets; restart the pipeline.
 * Calls must not overlap. Initialization before pthread_create and subsequent
 * use by the sole worker until pthread_join satisfy this requirement.
 * Mapping qualifies the SDK presentation clock, not sensor exposure timing.
 */
class PtsClock {
public:
    bool observe(uint64_t pts, uint64_t before_ns, uint64_t after_ns) {
        if (failed || after_ns < before_ns || after_ns-before_ns > 2000000U)
            return fail();
        const uint64_t middle_us=(before_ns+(after_ns-before_ns)/2)/1000;
        if (!initialized) {
            origin_pts=last_pts=pts;
            origin_us=last_us=middle_us;
            initialized=true;
            return true;
        }
        if (pts <= last_pts || middle_us <= last_us) return fail();
        uint64_t mapped=0;
        if (!translate(pts,origin_pts,origin_us,mapped)) return fail();
        // 5ms fixed budget plus 1000ppm drift since the initial sample.
        const uint64_t tolerance=5000+(middle_us-origin_us)/1000;
        const uint64_t error=mapped>middle_us?mapped-middle_us:middle_us-mapped;
        if (error>tolerance) return fail();
        // Also reject a sudden step after long uptime, when the cumulative
        // drift budget alone would conceal it.
        if (!translate(pts,last_pts,last_us,mapped)) return fail();
        const uint64_t recent_error=mapped>middle_us?mapped-middle_us:middle_us-mapped;
        if (recent_error>5000+(middle_us-last_us)/1000) return fail();
        last_pts=pts;last_us=middle_us;
        return true;
    }
    bool frame(uint64_t pts, uint64_t receipt_us, uint64_t &mapped_us) {
        mapped_us=0;
        uint64_t mapped=0;
        if (failed || !initialized || (have_frame && pts<=last_frame_pts) ||
            !translate(pts,last_pts,last_us,mapped)) return fail();
        if ((mapped>receipt_us && mapped-receipt_us>2000) ||
            (mapped<=receipt_us && receipt_us-mapped>500000)) return fail();
        if (have_frame && mapped<=last_frame_us) return fail();
        last_frame_pts=pts;last_frame_us=mapped;have_frame=true;
        mapped_us=mapped;
        return true;
    }
    bool healthy() const { return initialized && !failed; }
private:
    // Frame mapping uses the latest qualified observation so allowed clock
    // drift cannot accumulate into a false future/stale-frame failure. Keep
    // the initial observation separately to validate the total clock drift.
    static bool translate(uint64_t pts,uint64_t anchor_pts,uint64_t anchor_us,uint64_t &result) {
        if (pts>=anchor_pts) {
            const uint64_t delta=pts-anchor_pts;
            if (delta>std::numeric_limits<uint64_t>::max()-anchor_us) return false;
            result=anchor_us+delta;
        } else {
            const uint64_t delta=anchor_pts-pts;
            if (delta>anchor_us) return false;
            result=anchor_us-delta;
        }
        return true;
    }
    bool fail() { failed=true;return false; }
    bool initialized=false,failed=false,have_frame=false;
    uint64_t origin_pts=0,origin_us=0,last_pts=0,last_us=0;
    uint64_t last_frame_pts=0,last_frame_us=0;
};
struct PtsObservation {
    uint64_t pts=0,bracket_ns=0;
    int status=0;
    unsigned attempts=0;
};
// Scheduling delay makes an observation imprecise, not evidence of a clock
// reset. Retry a bounded number of samples before passing a precise one to the
// clock. Never retry an SDK error, reversed time, reset or drift failure.
template<typename Read,typename Now>
bool observe_pts_clock(PtsClock &clock,Read read,Now now,PtsObservation &sample) {
    for(unsigned i=0;i<3;i++) {
        sample.pts=0;const uint64_t before=now();
        sample.status=read(&sample.pts);const uint64_t after=now();
        sample.attempts=i+1;sample.bracket_ns=after>=before?after-before:UINT64_MAX;
        if(!sample.status && after>=before && sample.bracket_ns>2000000ULL)continue;
        return !sample.status && clock.observe(sample.pts,before,after);
    }
    return false;
}
} // namespace ca_zt30
