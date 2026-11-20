/* Exclusive-owner RAM diagnostic. MI_SYS_Init/Exit only; no sensor/pipeline calls.
 * Must run under the qualified watchdog after all vendor MI owners have exited. */
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#ifndef ZT30_SYS_LIBRARY_DIRECTORY
#define ZT30_SYS_LIBRARY_DIRECTORY "/config/lib"
#endif
static volatile sig_atomic_t stopping;
static void stop(int) { stopping=1; }
static uint64_t monotonic_ns() { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000U+uint64_t(t.tv_nsec); }
struct Library {
    void *handle;
    explicit Library(const char *name):handle(dlopen(name,RTLD_NOW|RTLD_GLOBAL)) {}
    ~Library() { if(handle)dlclose(handle); }
};
template<class T> struct Guarded {
    uint64_t before[4];T value{};uint64_t after[4];
    Guarded() { for(unsigned i=0;i<4;i++) { before[i]=0xa55a55aaa55a55aaULL;after[i]=0x5aa5aa555aa5aa55ULL; } }
    bool valid() const { for(unsigned i=0;i<4;i++)if(before[i]!=0xa55a55aaa55a55aaULL || after[i]!=0x5aa5aa555aa5aa55ULL)return false;return true; }
};
struct Owned {
    int (*exit)(unsigned short);
    bool active=false;
    explicit Owned(int (*exit_fn)(unsigned short)):exit(exit_fn) {}
    ~Owned() { if(active) { puts("STEP MI_SYS_Exit cleanup");int rc=exit(0);fprintf(stderr,"cleanup MI_SYS_Exit=%d\n",rc); } }
    int finish() { puts("STEP MI_SYS_Exit");int rc=exit(0);active=false;return rc; }
};
int main(int argc,char **)
{
    if(argc!=1)return 2;
    setvbuf(stdout,nullptr,_IOLBF,0);
    puts("STEP dlopen cam wrapper");
    struct sigaction action{};action.sa_handler=stop;action.sa_flags=SA_RESTART;sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,nullptr);sigaction(SIGINT,&action,nullptr);sigaction(SIGHUP,&action,nullptr);
    Library cam(ZT30_SYS_LIBRARY_DIRECTORY "/libcam_os_wrapper.so");
    if(!cam.handle) { fprintf(stderr,"cam wrapper: %s\n",dlerror());return 3; }
    puts("STEP dlopen MI SYS");
    Library sys(ZT30_SYS_LIBRARY_DIRECTORY "/libmi_sys.so");
    if(!sys.handle) { fprintf(stderr,"MI SYS: %s\n",dlerror());return 3; }
    auto init=reinterpret_cast<int(*)(unsigned short)>(dlsym(sys.handle,"MI_SYS_Init"));
    auto exit_fn=reinterpret_cast<int(*)(unsigned short)>(dlsym(sys.handle,"MI_SYS_Exit"));
    auto version=reinterpret_cast<int(*)(unsigned short,char*)>(dlsym(sys.handle,"MI_SYS_GetVersion"));
    auto pts=reinterpret_cast<int(*)(unsigned short,uint64_t*)>(dlsym(sys.handle,"MI_SYS_GetCurPts"));
    if(!init || !exit_fn || !version || !pts) { fputs("required fixed SYS symbol missing\n",stderr);return 4; }
    if(stopping)return 5;
    Owned owner(exit_fn);
    const uint64_t begin=monotonic_ns();
    puts("STEP MI_SYS_Init");errno=0;
    int rc=init(0);int init_errno=errno;printf("INIT rc=%d elapsed_ns=%llu errno=%d\n",rc,(unsigned long long)(monotonic_ns()-begin),init_errno);
    if(rc)return 6;
    owner.active=true;
    if(stopping)return 5;
    Guarded<char[128]> info;
    puts("STEP MI_SYS_GetVersion");
    rc=version(0,info.value);
    if(rc || !info.valid() || !memchr(info.value,0,128)) { fprintf(stderr,"version failed rc=%d guard=%d\n",rc,info.valid());return 7; }
    printf("VERSION %s\n",info.value);
    uint64_t first_pts=0,last_pts=0,first_clock=0,last_clock=0;
    for(unsigned i=0;i<10;i++) {
        if(stopping)return 5;
        Guarded<uint64_t> sample;
        printf("STEP MI_SYS_GetCurPts %u\n",i);
        uint64_t before=monotonic_ns();rc=pts(0,&sample.value);uint64_t after=monotonic_ns();
        if(rc || !sample.valid()) { fprintf(stderr,"PTS failed rc=%d guard=%d\n",rc,sample.valid());return 7; }
        uint64_t clock=before+(after-before)/2;
        printf("SAMPLE %u pts=%llu monotonic_ns=%llu call_ns=%llu\n",i,(unsigned long long)sample.value,(unsigned long long)clock,(unsigned long long)(after-before));
        if(i && sample.value<=last_pts) { fputs("non-monotonic PTS\n",stderr);return 8; }
        if(i==0) { first_pts=sample.value;first_clock=clock; }
        last_pts=sample.value;last_clock=clock;
        if(i!=9) { timespec remaining{0,100000000};while(nanosleep(&remaining,&remaining)<0 && !stopping) {} }
    }
    uint64_t pts_elapsed=last_pts-first_pts,clock_elapsed_us=(last_clock-first_clock)/1000;
    bool microseconds=clock_elapsed_us>0 && pts_elapsed>=clock_elapsed_us*8/10 && pts_elapsed<=clock_elapsed_us*12/10;
    rc=owner.finish();printf("EXIT rc=%d\n",rc);
    if(rc)return 9;
    if(stopping)return 5;
    if(!microseconds) { fputs("PTS rate does not match microseconds\n",stderr);return 8; }
    printf("RESULT PASS pts_elapsed_us=%llu monotonic_elapsed_us=%llu\n",(unsigned long long)pts_elapsed,(unsigned long long)clock_elapsed_us);
    return ferror(stdout)?10:0;
}
