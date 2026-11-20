#pragma once
#include "../include/apcam/terrain_format.h"
#include <algorithm>
#include <vector>
#include <string>

namespace terrain_upload {
constexpr uint64_t max_zip=512ULL*1024*1024, max_data=2ULL*1024*1024*1024;
struct Entry { char name[12]; uint32_t size; int lat,lon; };
inline void cleanup(const std::string &path) {
    DIR *d=opendir(path.c_str());
    if(!d) return;
    while(auto e=readdir(d)) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        unlink((path+"/"+e->d_name).c_str());
    }
    closedir(d); rmdir(path.c_str());
}
// Read one central-directory entry at a time. No archive-sized allocation.
inline const char *index(int fd,uint64_t size,std::vector<Entry> &entries,uint64_t &total) {
    using namespace ap_terrain;
    uint8_t tail[65557];
    size_t n=std::min<uint64_t>(sizeof(tail),size);
    if(n<22 || pread(fd,tail,n,size-n)!=ssize_t(n)) return "Cannot read ZIP";
    size_t end=n;
    for(size_t i=n-22;;i--) {
        if(u32(tail+i)==0x06054b50 && i+22+u16(tail+i+20)==n) { end=i; break; }
        if(!i) break;
    }
    if(end==n) return "Invalid ZIP directory";
    auto e=tail+end;
    uint32_t offset=u32(e+16),length=u32(e+12);
    unsigned count=u16(e+10);
    if(u16(e+4)||u16(e+6)||count!=u16(e+8)||!count||count>256 || uint64_t(offset)+length!=size-n+end) return "Unsupported ZIP layout";
    uint64_t pos=offset;
    total=0;
    for(unsigned i=0;i<count;i++) {
        uint8_t h[46],local[30]; Entry entry{};
        if(pos+46>uint64_t(offset)+length || pread(fd,h,46,pos)!=46 || u32(h)!=0x02014b50) return "Invalid ZIP entry";
        unsigned namesize=u16(h+28),extra=u16(h+30),comment=u16(h+32),method=u16(h+10),flags=u16(h+8);
        uint32_t compressed=u32(h+20),start=u32(h+42);
        entry.size=u32(h+24);
        unsigned mode=u32(h+38)>>16;
        if(namesize!=11 || extra>4096 || comment>4096 || (method!=0 && method!=8) ||
           (flags & ~0x808U) || u16(h+34) || ((mode&S_IFMT) && (mode&S_IFMT)!=S_IFREG) ||
           !entry.size || entry.size%2048 || entry.size>256U*1024*1024 ||
           pos+46+namesize+extra+comment>uint64_t(offset)+length ||
           pread(fd,entry.name,11,pos+46)!=11 || !name(entry.name,entry.lat,entry.lon)) return "ZIP must contain only ArduPilot terrain DAT tiles";
        for(const auto &previous:entries) if(!strcmp(previous.name,entry.name)) return "Duplicate terrain tile";
        if(uint64_t(start)+30>offset || pread(fd,local,30,start)!=30 || u32(local)!=0x04034b50 ||
           u16(local+6)!=flags || u16(local+8)!=method || u16(local+26)!=11 || u16(local+28)>4096 ||
           uint64_t(start)+30+11+u16(local+28)+compressed>offset) return "Invalid ZIP local header";
        char localname[11];
        if(pread(fd,localname,11,start+30)!=11 || memcmp(localname,entry.name,11)) return "ZIP names do not match";
        if(!(flags&8) && (u32(local+14)!=u32(h+16) || u32(local+18)!=compressed || u32(local+22)!=entry.size)) return "ZIP sizes do not match";
        total+=entry.size;
        if(total>max_data) return "Terrain archive exceeds 2 GiB extracted limit";
        entries.push_back(entry);
        pos+=46+namesize+extra+comment;
    }
    return pos==uint64_t(offset)+length?nullptr:"Invalid ZIP directory size";
}
inline const char *extract(const std::string &zip,const std::string &stage,const Entry &e,int client,unsigned &spacing) {
    int p[2];
    if(pipe(p)) return "Cannot create extraction pipe";
    // The server normally ignores SIGCHLD. Restore waitable children locally.
    struct sigaction old{}, action{}; action.sa_handler=SIG_DFL; sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD,&action,&old);
    pid_t child=fork();
    if(child==0) {
        close(p[0]); dup2(p[1],STDOUT_FILENO); close(p[1]); close(client);
        int null=open("/dev/null",O_RDWR);
        if(null>=0) { dup2(null,STDIN_FILENO); dup2(null,STDERR_FILENO); if(null>2) close(null); }
        struct rlimit cpu{120,120}; setrlimit(RLIMIT_CPU,&cpu);
        // Only stdout is consumed: even a malicious archive cannot create
        // paths, links or devices. The parent enforces the declared byte limit.
        execlp("unzip","unzip","-p",zip.c_str(),e.name,(char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    const char *error=nullptr;
    int out=open((stage+"/"+e.name).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
    uint64_t count=0; size_t used=0; uint8_t b[2048]; unsigned valid_blocks=0;
    if(child<0 || out<0) error="Cannot start terrain extraction";
    while(!error) {
        struct pollfd wait{p[0],POLLIN,0};
        int ready=poll(&wait,1,30000);
        if(ready<0 && errno==EINTR) continue;
        if(ready<=0) { error="Terrain extraction timed out"; break; }
        ssize_t got=read(p[0],b+used,sizeof(b)-used);
        if(got<0 && errno==EINTR) continue;
        if(got<0) { error="Cannot read extracted terrain"; break; }
        if(!got) break;
        used+=got; count+=got;
        if(count>e.size) { error="ZIP exceeds declared size"; break; }
        if(used!=sizeof(b)) continue;
        if(!ap_terrain::empty(b)) {
            unsigned s=ap_terrain::u16(b+20);
            if(!ap_terrain::valid(b) || !ap_terrain::located(b,e.lat,e.lon,count/2048-1) || (spacing && s!=spacing)) {
                error="Terrain block CRC, location or spacing is invalid"; break;
            }
            spacing=s; ++valid_blocks;
        }
        if(!write_all(out,b,sizeof(b))) { error="Cannot write terrain to SD card"; break; }
        used=0;
    }
    close(p[0]);
    if(error && child>0) kill(child,SIGKILL);
    int status=0;
    if(child>0) { while(waitpid(child,&status,0)<0 && errno==EINTR) {} }
    sigaction(SIGCHLD,&old,nullptr);
    if(!error && (!WIFEXITED(status) || WEXITSTATUS(status))) error="ZIP extraction failed (unzip required)";
    if(!error && (used || count!=e.size || !valid_blocks)) error="Incomplete terrain tile";
    if(out>=0) {
        if(fsync(out) && !error) error="Cannot sync terrain to SD card";
        if(close(out) && !error) error="Cannot close terrain file";
    }
    return error;
}
}

static void handle_terrain_upload(int fd,const APC_HTTPRequest *request) {
    using namespace terrain_upload;
    if(!valid_csrf_header(request)) { send_text_errorf(fd,403,"Forbidden",S_CSRF_RELOAD); return; }
    if(!request->content_length || request->content_length>max_zip) {
        send_text_error(fd,413,"Payload Too Large","Terrain ZIP limit is 512 MiB",nullptr); return;
    }
    std::string root=std::string(MEDIA_ROOT)+"/TERRAIN";
    if(mkdir(root.c_str(),0755) && errno!=EEXIST) {
        send_text_error(fd,500,"Error","Cannot create terrain directory on SD card",nullptr); return;
    }
    int lock=open((root+"/.upload.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(lock<0 || flock(lock,LOCK_EX|LOCK_NB)) {
        if(lock>=0) close(lock);
        send_text_error(fd,409,"Conflict","Another terrain upload is active",nullptr); return;
    }
    const char *error=nullptr;
    struct statvfs space{};
    if(statvfs(root.c_str(),&space) || uint64_t(space.f_bavail)*space.f_frsize<request->content_length+8*1024*1024) error="Insufficient SD space for ZIP";
    char temporary[PATH_MAX]; snprintf(temporary,sizeof(temporary),"%s/dataset-XXXXXX",root.c_str());
    bool created=!error && mkdtemp(temporary);
    if(!error && !created) error="Cannot create terrain staging directory";
    std::string stage=temporary, zip=stage+"/upload.zip";
    int output=-1; size_t received=0; uint64_t total=0; unsigned spacing=0;
    std::vector<Entry> entries;
    if(!error) { output=open(zip.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600); if(output<0) error="Cannot create terrain ZIP on SD card"; }
    if(!error && !receive_upload_body(fd,request,output,&received)) error="Terrain upload interrupted or SD card full";
    if(!error) error=index(output,received,entries,total);
    if(output>=0) close(output);
    if(!error && (statvfs(root.c_str(),&space) || uint64_t(space.f_bavail)*space.f_frsize<total+8*1024*1024)) error="Insufficient SD space for extracted terrain";
    for(const auto &entry:entries) {
        if(error) break;
        error=extract(zip,stage,entry,fd,spacing);
    }
    if(created) unlink(zip.c_str());
    std::string link=root+"/current.new", current=root+"/current";
    char previous[PATH_MAX]{};
    FILE *old=fopen(current.c_str(),"r");
    if(old) { if(!fgets(previous,sizeof(previous),old)) previous[0]=0; fclose(old); }
    previous[strcspn(previous,"\r\n")]=0;
    if(!error) {
        int directory=open(stage.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(directory<0 || fsync(directory)) error="Cannot sync terrain directory";
        if(directory>=0) close(directory);
    }
    if(!error) {
        unlink(link.c_str());
        // A regular manifest, not a symlink: SD cards commonly use FAT/exFAT.
        int manifest=open(link.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0644);
        const char *version=strrchr(temporary,'/')+1;
        if(manifest<0) error="Cannot create terrain manifest";
        else {
            if(!write_all(manifest,version,strlen(version)) || !write_all(manifest,"\n",1) || fsync(manifest)) error="Cannot sync terrain manifest";
            if(close(manifest) && !error) error="Cannot close terrain manifest";
        }
        if(!error && rename(link.c_str(),current.c_str())) error="Cannot activate terrain";
        unlink(link.c_str());
    }
    if(error) {
        if(created) cleanup(stage);
        send_text_error(fd,400,"Terrain upload failed",error,nullptr);
    } else {
        int directory=open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(directory>=0) { fsync(directory); close(directory); }
        // Queries reopen files in the worker; an already open old tile remains
        // usable until its read completes. Readers detect the new directory.
        if(!strncmp(previous,"dataset-",8) && !strchr(previous,'/')) cleanup(root+"/"+previous);
        char message[160]; snprintf(message,sizeof(message),"Uploaded %zu terrain tiles, %u m spacing. Active without restart.",entries.size(),spacing);
        send_response(fd,200,"OK","text/plain",message,strlen(message),nullptr);
    }
    close(lock);
}
