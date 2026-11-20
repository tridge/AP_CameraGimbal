#include "../src/backends/a8/isp.cpp"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static uint32_t limits[8], table[65], manual[3], mode;
static int table_result, limit_result;
static unsigned warnings;
static const uint32_t inherited[8] = {5653,33332,16,16,1024,1024,131072,1024};
void ca_log(const char *format, ...)
{
    if (strstr(format, "retaining minimum")) warnings++;
}
static int get_limit(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(attr, limits, sizeof(limits));
    return limit_result;
}
static int set_limit(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(limits, attr, sizeof(limits));
    return 0;
}
static int get_table(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(attr, table, sizeof(table));
    return table_result;
}
static int set_mode(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(&mode, attr, sizeof(mode));
    return 0;
}
static int get_manual(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(attr, manual, sizeof(manual));
    return 0;
}
static int set_manual(uint32_t dev, uint32_t chn, void *attr)
{
    assert(dev == 0 && chn == 0);
    memcpy(manual, attr, sizeof(manual));
    return 0;
}
static void reset(void)
{
    api = (struct isp_api) {
        .set_expo_mode=set_mode,
        .get_manual_expo=get_manual, .set_manual_expo=set_manual,
        .get_expo_limit=get_limit, .set_expo_limit=set_limit,
        .get_expo_table=get_table,
    };
    memcpy(limits, inherited, sizeof(limits));
    memset(table, 0, sizeof(table));
    table[0] = 8;
    for (unsigned i=0; i<8; i++) {
        table[1+4*i]=16;
        table[2+4*i]=i ? 33332 : 147;
        table[3+4*i]=table[4+4*i]=1024;
    }
    manual[0]=10000; manual[1]=2048; manual[2]=2048;
    mode=99; warnings=0; table_result=limit_result=0;
}
int main(void)
{
    struct ca_config config = {.iso=CA_ISO_AUTO, .shutter=CA_SHUTTER_AUTO};
    reset();
    assert(set_exposure(&config)==0);
    assert(limits[0]==147 && limits[1]==33332 && mode==AE_MODE_AUTO);
    assert(memcmp(limits+2,inherited+2,6*sizeof(uint32_t))==0);
    assert(!warnings);

    /* Starting in fixed shutter must not poison the later Auto defaults. */
    reset();
    config.shutter=CA_SHUTTER_1_2000;
    assert(set_exposure(&config)==0 && limits[0]==500 && limits[1]==500);
    config.shutter=CA_SHUTTER_AUTO;
    assert(set_exposure(&config)==0 && limits[0]==147 && limits[1]==33332);
    config.shutter=CA_SHUTTER_1_1000; config.iso=CA_ISO_800;
    assert(set_exposure(&config)==0 && mode==AE_MODE_MANUAL);
    assert(manual[0]==1000 && manual[1]==8192 && manual[2]==1024);
    config.shutter=CA_SHUTTER_AUTO; config.iso=CA_ISO_AUTO;
    assert(set_exposure(&config)==0 && mode==AE_MODE_AUTO && limits[0]==147 && limits[1]==33332);

    /* Do not raise an existing lower limit, or assume the first table row is shortest. */
    reset(); limits[0]=50;
    assert(set_exposure(&config)==0 && limits[0]==50);
    reset(); table[2]=20000; table[6]=120;
    assert(set_exposure(&config)==0 && limits[0]==120);

    /* Unusable rows must not discard a usable bright-scene shutter floor. */
    const uint32_t unusable[] = {0, 33333, 1000001, UINT32_MAX};
    for (unsigned i=0; i<sizeof(unusable)/sizeof(unusable[0]); i++) {
        reset(); table[2]=unusable[i]; table[6]=147;
        assert(set_exposure(&config)==0 && limits[0]==147 && limits[1]==33332);
        assert(memcmp(limits+2,inherited+2,6*sizeof(uint32_t))==0 && !warnings);
    }
    /* Use the configured maximum, without an arbitrary one-second cutoff. */
    reset(); limits[1]=2000000; table[6]=1500000;
    assert(set_exposure(&config)==0 && limits[0]==147 && limits[1]==2000000 && !warnings);
    reset(); table[0]=1; table[2]=33332;
    assert(set_exposure(&config)==0 && limits[0]==5653 && !warnings);

    for (unsigned bad=0; bad<6; bad++) {
        reset();
        if (bad==0) table[0]=0;
        if (bad==1) table[0]=17;
        if (bad==2) for (unsigned i=0; i<8; i++) table[2+4*i]=0;
        if (bad==3) for (unsigned i=0; i<8; i++) table[2+4*i]=33333;
        if (bad==4) table_result=-EIO;
        if (bad==5) api.get_expo_table=NULL;
        assert(set_exposure(&config)==0);
        assert(memcmp(limits,inherited,sizeof(limits))==0 && warnings==1);
    }
    reset(); limit_result=-EIO;
    assert(set_exposure(&config)==-EIO && !api.have_default_limit);
    puts("PASS A8 auto shutter floor, fixed/manual controls, untouched gain limits and invalid table fallback");
    return 0;
}
