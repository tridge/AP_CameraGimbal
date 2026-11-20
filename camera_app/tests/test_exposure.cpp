#include "camera_app/sigmastar_exposure.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct ca_exposure s=ca_exposure_empty(1,1234);
    assert(s.lens==1 && s.time_us==1234 && s.valid==0 && s.mode==255);
    assert(isnan(s.shutter_us) && isnan(s.target) && isnan(s.error));
    /* Literal SDK offsets catch accidental reordering of the gain, aperture,
     * time, short-exposure and histogram fields (manual-exposure differs). */
    uint32_t wire[143]= {};
    wire[0]=1; wire[1]=1; wire[2]=18; wire[3]=3072; wire[4]=2048; wire[5]=2500;
    wire[10]=70; wire[11]=85; wire[142]=100;
    struct ca_sstar_exposure_info info;
    memcpy(&info,wire,sizeof(info));
    ca_sstar_exposure_decode(&s,&info);
    assert(s.shutter_us==2500 && s.analog_gain==3 && s.isp_gain==2);
    assert(s.luma==70 && s.target==100 && s.error==30);
    assert(s.state==(CA_AE_STATE_STABLE|CA_AE_STATE_LIMIT));
    assert((s.valid & (CA_AE_STABLE|CA_AE_LIMIT))==(CA_AE_STABLE|CA_AE_LIMIT));
    assert(!(s.valid & CA_AE_DGAIN) && isnan(s.digital_gain));
    ca_sstar_exposure_mode(&s,0); assert(s.mode==CA_AE_AUTO);
    ca_sstar_exposure_mode(&s,2); assert(s.mode==CA_AE_GAIN_PRIORITY);
    ca_sstar_exposure_mode(&s,3); assert(s.mode==CA_AE_SHUTTER_PRIORITY);
    ca_sstar_exposure_mode(&s,4); assert(s.mode==CA_AE_MANUAL);
    s=ca_exposure_empty(0,0);
    ca_sstar_exposure_mode(&s,999); assert(!(s.valid & CA_AE_MODE) && s.mode==255);
    puts("PASS SigmaStar exposure ABI, feedback units, mode mapping and invalid fields");
    return 0;
}
