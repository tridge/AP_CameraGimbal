#ifndef GCU_MANIFEST_H
#define GCU_MANIFEST_H
/* Bounded JSON reader for the two required top-level package properties.
 * Unknown metadata is validated and skipped, never searched for guard values. */
#include "../include/apcam/json_reader.h"
static bool gcu_manifest_valid(const char *text, size_t length, bool *needs_isp)
{
    struct gcu_json j={text,text+length};
    bool target_seen=false, isp_seen=false;
    if (!gcu_json_take(&j,'{')) return false;
    do {
        char key[128];
        if (!gcu_json_string(&j,key,sizeof(key)) || !gcu_json_take(&j,':')) return false;
        if (!strcmp(key,"target")) {
            char target[64];
            if (target_seen || !gcu_json_string(&j,target,sizeof(target)) ||
                strcmp(target,"xfrobot-z1mini")) return false;
            target_seen=true;
        } else if (!strcmp(key,"vendor_isp_required")) {
            if (isp_seen) return false;
            if (gcu_json_literal(&j,"true")) *needs_isp=true;
            else if (gcu_json_literal(&j,"false")) *needs_isp=false;
            else return false;
            isp_seen=true;
        } else if (!gcu_json_value(&j,1)) return false;
        if (gcu_json_take(&j,'}')) {
            gcu_json_space(&j);
            return target_seen && isp_seen && j.p==j.end;
        }
    } while (gcu_json_take(&j,','));
    return false;
}
#endif
