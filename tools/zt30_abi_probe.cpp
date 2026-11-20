/* ZT30 ABI observation tool. No explicit pipeline Init/Exit/bind/buffer calls.
 * dlopen/dlclose run SDK constructors/destructors; getters lazily open /dev/mi_sys.
 * GetVersion and GetCurPts signatures/bounds are proven against the pinned
 * 20220422200406 libmi_sys disassembly; other printed layouts are candidates. */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include "camera_app/src/backends/a8/mi/star/m6_sys.h"
#include "camera_app/src/backends/a8/mi/star/m6_snr.h"
#include "camera_app/src/backends/a8/mi/star/m6_vif.h"
#include "camera_app/src/backends/a8/mi/star/m6_isp.h"
#include "camera_app/src/backends/a8/mi/star/m6_scl.h"
#include "camera_app/src/backends/a8/mi/star/m6_venc.h"
#include "camera_app/src/backends/a8/mi/star/m6_rgn.h"

static_assert(sizeof(m6_sys_ver)==128, "MI_SYS_GetVersion writes 128 bytes");
static_assert(sizeof(m6_sys_bind)==16 && offsetof(m6_sys_bind,port)==12, "MI SYS port layout");
#if defined(__arm__)
static_assert(sizeof(void*)==4 && sizeof(uint64_t)==8, "ARMv7 ABI required");
// Copies and argument registers verified against the captured MI libraries.
static_assert(sizeof(m6_snr_pad)==52 && sizeof(m6_snr_plane)==72 && sizeof(m6_snr_res)==52, "sensor copied ABI sizes");
static_assert(sizeof(m6_vif_grp)==28 && sizeof(m6_vif_dev)==20 && sizeof(m6_vif_port)==20, "VIF copied ABI sizes");
static_assert(sizeof(m6_isp_chn)==80 && sizeof(m6_isp_para)==16 && sizeof(m6_isp_port)==12, "ISP copied ABI sizes");
static_assert(sizeof(m6_scl_port)==24 && sizeof(m6_venc_chn)==76, "SCL/VENC copied ABI sizes");
#endif

class Library {
public:
    explicit Library(const char *path) : handle(dlopen(path,RTLD_NOW|RTLD_GLOBAL)) {}
    ~Library() { if(handle) dlclose(handle); }
    Library(const Library &)=delete;
    Library &operator=(const Library &)=delete;
    void *handle;
};

template<typename T> struct Guarded {
    uint64_t before[4]={0xa55a55aaa55a55aaULL,0xa55a55aaa55a55aaULL,0xa55a55aaa55a55aaULL,0xa55a55aaa55a55aaULL};
    T value {};
    uint64_t after[4]={0x5aa5aa555aa5aa55ULL,0x5aa5aa555aa5aa55ULL,0x5aa5aa555aa5aa55ULL,0x5aa5aa555aa5aa55ULL};
    bool valid() const {
        for(unsigned i=0;i<4;i++) if(before[i]!=0xa55a55aaa55a55aaULL || after[i]!=0x5aa5aa555aa5aa55ULL) return false;
        return true;
    }
};

static int query(const char *directory)
{
    char path[4096];
    int n=snprintf(path,sizeof(path),"%s/libcam_os_wrapper.so",directory);
    if(n<0 || size_t(n)>=sizeof(path)) return 2;
    Library cam(path);
    if(!cam.handle) { fprintf(stderr,"load cam wrapper: %s\n",dlerror());return 3; }
    n=snprintf(path,sizeof(path),"%s/libmi_sys.so",directory);
    if(n<0 || size_t(n)>=sizeof(path)) return 2;
    Library sys(path);
    if(!sys.handle) { fprintf(stderr,"load MI SYS: %s\n",dlerror());return 3; }
    auto version=reinterpret_cast<int(*)(unsigned short,m6_sys_ver*)>(dlsym(sys.handle,"MI_SYS_GetVersion"));
    auto pts=reinterpret_cast<int(*)(unsigned short,uint64_t*)>(dlsym(sys.handle,"MI_SYS_GetCurPts"));
    if(!version || !pts) { fprintf(stderr,"required SYS getters missing\n");return 4; }
    Guarded<m6_sys_ver> v;
    const int vr=version(0,&v.value);
    if(!v.valid()) { fprintf(stderr,"version buffer guard corrupted\n");return 5; }
    if(vr) { fprintf(stderr,"GetVersion failed: %d\n",vr);return 6; }
    const void *end=memchr(v.value.version,0,sizeof(v.value.version));
    if(!end) { fprintf(stderr,"version string is not terminated\n");return 7; }
    printf("MI_SYS_GetVersion=%s\n",v.value.version);
    Guarded<uint64_t> t;
    const int tr=pts(0,&t.value);
    if(!t.valid()) { fprintf(stderr,"PTS buffer guard corrupted\n");return 5; }
    if(tr) { fprintf(stderr,"GetCurPts failed: %d\n",tr);return 6; }
    printf("MI_SYS_GetCurPts=%llu\n",static_cast<unsigned long long>(t.value));
    return 0;
}

int main(int argc,char **argv)
{
    if(argc!=1 && !(argc==3 && !strcmp(argv[1],"--query-sys"))) {
        fprintf(stderr,"Usage: %s [--query-sys /config/lib]\n",argv[0]);return 2;
    }
    printf("pointer_bytes=%zu endian=%s\n",sizeof(void*),__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__?"little":"big");
#define LAYOUT(type) printf("layout %s size=%zu align=%zu\n",#type,sizeof(type),alignof(type))
    LAYOUT(m6_sys_ver);LAYOUT(m6_sys_bind);LAYOUT(m6_snr_pad);LAYOUT(m6_snr_plane);LAYOUT(m6_snr_res);
    LAYOUT(m6_vif_grp);LAYOUT(m6_vif_dev);LAYOUT(m6_vif_port);LAYOUT(m6_isp_chn);LAYOUT(m6_isp_para);LAYOUT(m6_isp_port);
    LAYOUT(m6_scl_port);LAYOUT(m6_venc_chn);LAYOUT(m6_venc_pack);LAYOUT(m6_venc_strm);LAYOUT(m6_rgn_chn);LAYOUT(m6_rgn_bmp);
#undef LAYOUT
    // Printing candidate layouts does not qualify the uncalled module APIs.
    return argc==3 ? query(argv[2]) : 0;
}
