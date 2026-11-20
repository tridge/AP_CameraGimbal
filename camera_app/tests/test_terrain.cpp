#include "camera_app/terrain.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
static ca_terrain_state query(double lat,double lon,float &height) {
    auto state=CA_TERRAIN_PENDING;
    for(unsigned i=0;i<500 && state==CA_TERRAIN_PENDING;i++) {
        state=ca_terrain_height(llround(lat*1e7),llround(lon*1e7),height);
        if(state==CA_TERRAIN_PENDING) usleep(10000);
    }
    return state;
}
int main(int argc,char **argv) {
    assert(argc==2 || argc==4);
    ca_terrain_start(argv[1]);
    float height=0;
    if(argc==4) {
        auto state=query(atof(argv[2]),atof(argv[3]),height);
        printf("%d %.6f\n",state,height);
        ca_terrain_stop();
        return state==CA_TERRAIN_READY?0:1;
    }
    double lat,lon;
    while(scanf("%lf %lf",&lat,&lon)==2) {
        auto state=query(lat,lon,height);
        printf("%d %.6f\n",state,height); fflush(stdout);
    }
    ca_terrain_stop();
}
