/* Metadata-only exclusive SYS/VENC diagnostic. No pixel dereference, USB,
 * binds, RGB initialization, output stream, PTS getter or persistent paths.
 * Watchdog and vendor restoration belong to the independent controller. */
#include "../camera_app/src/backends/zt30/input_metadata.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
using namespace ca_zt30;
static volatile sig_atomic_t stopping;
static void stop(int) { stopping=1; }
static uint64_t now(void*) {
    timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
}
static bool cancel(void*) { return stopping!=0; }
struct File {
    int fd;
    explicit File(int n):fd(n) {}
    ~File() { if(fd>=0)close(fd); }
    File(const File&)=delete;
    File &operator=(const File&)=delete;
};
static bool ram_directory(char (&directory)[160]) {
    char path[160]{};ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n<0 || n>=ssize_t(sizeof(path)-1))return false;
    const char prefix[]="/tmp/apcg-zt30.session.";
    if(std::strncmp(path,prefix,sizeof(prefix)-1))return false;
    char *tail=path+sizeof(prefix)-1;
    if(std::strlen(tail)!=6+std::strlen("/input-metadata-probe"))return false;
    for(unsigned i=0;i<6;++i)if(!((tail[i]>='0' && tail[i]<='9') || (tail[i]>='A' && tail[i]<='Z') || (tail[i]>='a' && tail[i]<='z')))return false;
    if(std::strcmp(tail+6,"/input-metadata-probe"))return false;
    tail[6]=0;struct statfs fs{};struct stat st{};
    if(lstat(path,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&0777)!=0700 ||
       statfs(path,&fs) || fs.f_type!=0x01021994 || uint64_t(fs.f_bavail)*fs.f_bsize<UINT64_C(16)*1024*1024)return false;
    std::strcpy(directory,path);return true;
}
static bool timely(uint64_t deadline) { const auto t=now(nullptr);return !stopping && t && t<deadline; }
static bool write_all(int fd,const void *data,size_t n,uint64_t deadline) {
    auto *p=static_cast<const uint8_t*>(data);
    while(n && timely(deadline)) {
        const ssize_t count=write(fd,p,n);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        p+=count;n-=size_t(count);
    }
    return n==0 && timely(deadline);
}
static bool capture(int dir,const char *name,const char *source,bool may_be_absent,uint64_t deadline) {
    if(!timely(deadline))return false;
    File output(openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(output.fd<0)return false;
    File input(open(source,O_RDONLY|O_CLOEXEC|O_NONBLOCK));
    if(input.fd<0)return may_be_absent && errno==ENOENT && write_all(output.fd,"ABSENT\n",7,deadline);
    char block[4096];size_t total=0;
    while(timely(deadline)) {
        const ssize_t n=read(input.fd,block,sizeof(block));
        if(n<0 && errno==EINTR)continue;
        if(n<0)return false;
        if(!n)return total>0 && timely(deadline);
        if(size_t(n)>262144-total || !write_all(output.fd,block,size_t(n),deadline))return false;
        total+=size_t(n);
    }
    return false;
}
struct Observer {
    int dir;
    static bool snapshot(void *context,InputSnapshot phase,const InputBufferInfo *info,uint32_t handle,uint64_t deadline) {
        auto &o=*static_cast<Observer*>(context);
        const char *prefix=phase==InputSnapshot::BeforeGet ? "before-get" : phase==InputSnapshot::Leased ? "leased" : phase==InputSnapshot::AfterPut ? "after-put" : "after-exit";
        std::printf("SNAPSHOT BEGIN %s ns=%llu deadline=%llu\n",prefix,(unsigned long long)now(nullptr),(unsigned long long)deadline);
        if(phase==InputSnapshot::Leased) {
            File f(openat(o.dir,"leased-info.bin",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
            if(!info || f.fd<0 || !write_all(f.fd,info,sizeof(*info),deadline))return false;
            std::printf("LEASE handle=%u info_bytes=%zu\n",handle,sizeof(*info));
        }
        struct Source { const char *name,*path;bool optional; };
        const Source sources[]={
            {"maps","/proc/self/maps",false},
            {"venc","/proc/mi_modules/mi_venc/mi_venc0",phase==InputSnapshot::AfterExit},
            {"sys","/proc/mi_modules/mi_sys/mi_sys0",false},
            {"queues","/proc/mi_modules/mi_bufqueue_status",false},
            {"mma","/proc/mi_modules/mi_sys_mma/mem_stat",false}
        };
        for(const auto &s:sources) {
            char name[80];const int n=std::snprintf(name,sizeof(name),"%s-%s.txt",prefix,s.name);
            if(n<=0 || size_t(n)>=sizeof(name) || !capture(o.dir,name,s.path,s.optional,deadline))return false;
        }
        std::printf("SNAPSHOT END %s ns=%llu\n",prefix,(unsigned long long)now(nullptr));
        return !std::ferror(stdout) && timely(deadline);
    }
    static void event(void*,const char *name,int rc) { std::printf("CALL %s rc=%d ns=%llu\n",name,rc,(unsigned long long)now(nullptr)); }
};
struct Sdk {
    void *handle=nullptr;
    ~Sdk() { if(handle)dlclose(handle); }
    InputMetadataApi api{};
    template<class T> bool symbol(T &out,const char *name) {
        out=reinterpret_cast<T>(dlsym(handle,name));
        if(!out)std::fprintf(stderr,"missing SDK symbol %s\n",name);
        return out!=nullptr;
    }
    bool open(const char *bundle) {
        handle=dlopen(bundle,RTLD_NOW|RTLD_GLOBAL);
        if(!handle) { std::fprintf(stderr,"minimal SYS/VENC dlopen: %s\n",dlerror());return false; }
#define LOAD(field,name) if(!symbol(api.field,name))return false
        LOAD(sys_init,"MI_SYS_Init");LOAD(sys_exit,"MI_SYS_Exit");
        LOAD(create_device,"MI_VENC_CreateDev");LOAD(destroy_device,"MI_VENC_DestroyDev");
        LOAD(create_channel,"MI_VENC_CreateChn");LOAD(destroy_channel,"MI_VENC_DestroyChn");
        LOAD(set_input_source,"MI_VENC_SetInputSourceConfig");
        LOAD(start,"MI_VENC_StartRecvPic");LOAD(stop,"MI_VENC_StopRecvPic");
        LOAD(buffer.get,"MI_SYS_ChnInputPortGetBuf");LOAD(buffer.put,"MI_SYS_ChnInputPortPutBuf");
#undef LOAD
        return true;
    }
};
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc==1) { std::puts("LAYOUT input metadata 640x512 format=11 info=264 timeout_ms=100 discard=1 no-SDK-access");return 0; }
    if(argc!=2 || std::strcmp(argv[1],"--inspect"))return 2;
    char directory[160]{};
    if(!ram_directory(directory)) { std::fputs("RAM qualification directory required\n",stderr);return 3; }
    File dir(open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(dir.fd<0)return 3;
    File marker(openat(dir.fd,"owned-input-metadata-preflight.ok",O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    const char expected[]="ZT30 input metadata preflight v1\n";char contents[sizeof(expected)]{};struct stat st{};
    if(marker.fd<0 || fstat(marker.fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_size!=sizeof(expected)-1 ||
       read(marker.fd,contents,sizeof(contents))!=ssize_t(sizeof(expected)-1) || std::strcmp(contents,expected))return 3;
    // Reserve once before dlopen: a rerun of this session cannot enter SDK.
    File once(openat(dir.fd,"input-metadata.started",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(once.fd<0)return 3;
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,nullptr) || sigaction(SIGINT,&action,nullptr) || sigaction(SIGHUP,&action,nullptr))return 4;
    char bundle[200];const int n=std::snprintf(bundle,sizeof(bundle),"%s/input-sdk.so",directory);
    if(n<=0 || size_t(n)>=sizeof(bundle) || stopping)return 4;
    std::printf("SDK BEGIN ns=%llu\n",(unsigned long long)now(nullptr));
    Sdk sdk;if(!sdk.open(bundle) || stopping) { std::fflush(nullptr);_exit(5); }
    Observer observer{dir.fd};InputMetadata test(sdk.api,{&observer,now,cancel,Observer::snapshot,Observer::event});
    const bool ok=test.run();
    std::printf("RESULT %s failure=%s status=%d cleanup=%d ns=%llu\n",ok?"PASS":"FAIL",test.failure?test.failure:"none",test.raw_status,test.cleanup_status,(unsigned long long)now(nullptr));
    // Ambiguous lease/cleanup may leave SDK callback threads alive. Never
    // unload their code or run exit finalizers on failure; process teardown
    // is bounded externally and its reclamation must be verified independently.
    if(!ok || std::ferror(stdout) || std::fflush(nullptr))_exit(6);
    return 0;
}
