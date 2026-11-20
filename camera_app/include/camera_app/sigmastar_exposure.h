#ifndef CAMERA_APP_SIGMASTAR_EXPOSURE_H
#define CAMERA_APP_SIGMASTAR_EXPOSURE_H
#include "apcam/compiler.h"
#include "camera_app/exposure.h"
#include <stddef.h>

/* MI_ISP_AE_EXPO_INFO_TYPE_t from SigmaStar's ISP API (sections 3.62-3.66).
 * https://wx.comake.online/doc/ds82ff82j7jsd9-SSD220/customer/development/isp/api.html
 * Both SDK generations use this payload; I6C adds a device argument to calls.
 * SensorGain is the combined sensor gain, not separate analogue/digital gains.
 */
struct ca_sstar_exposure_value { uint32_t fnum, sensor_gain, isp_gain, us; };
struct ca_sstar_exposure_info {
    uint32_t stable, limited;
    struct ca_sstar_exposure_value long_value, short_value;
    uint32_t weighted_y, average_y, histogram[128], lv, bv, target;
};
APC_STATIC_ASSERT(offsetof(struct ca_sstar_exposure_info,target)==568,"SigmaStar AE ABI");
static inline void ca_sstar_exposure_decode(struct ca_exposure *s,
                                           const struct ca_sstar_exposure_info *q)
{
    s->shutter_us=q->long_value.us;
    s->analog_gain=q->long_value.sensor_gain/1024.0f;
    s->isp_gain=q->long_value.isp_gain/1024.0f;
    s->luma=q->weighted_y;
    s->target=q->target;
    s->error=s->target-s->luma;
    s->state=(q->stable ? CA_AE_STATE_STABLE : 0) | (q->limited ? CA_AE_STATE_LIMIT : 0);
    s->valid|=CA_AE_SHUTTER|CA_AE_AGAIN|CA_AE_IGAIN|CA_AE_LUMA|CA_AE_TARGET|
              CA_AE_ERROR|CA_AE_STABLE|CA_AE_LIMIT;
}
static inline void ca_sstar_exposure_mode(struct ca_exposure *s, uint32_t mode)
{
    if (mode==0) s->mode=CA_AE_AUTO;
    else if (mode==2) s->mode=CA_AE_GAIN_PRIORITY;
    else if (mode==3) s->mode=CA_AE_SHUTTER_PRIORITY;
    else if (mode==4) s->mode=CA_AE_MANUAL;
    else return;
    s->valid|=CA_AE_MODE;
}
#endif
