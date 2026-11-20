#include "camera_app/terrain.h"
#include "apcam/terrain_format.h"
#include "apcam/APC_Resource.h"
#include <cstdio>
#include <tuple>
#include <algorithm>
#include <ctype.h>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace {
using clock_type=std::chrono::steady_clock;
struct Key {
    int lat,lon; unsigned x,y;
    bool operator<(const Key &b) const { return std::tie(lat,lon,x,y)<std::tie(b.lat,b.lon,b.x,b.y); }
};
struct Block { std::array<uint8_t,2048> data{}; uint64_t used=0; bool ready=false; };
struct File { unsigned spacing=0; bool pending=true; };
struct Terrain {
    APC_Mutex mutex;
    APC_Condition wake;
    std::thread worker;
    std::string root, active;
    std::map<std::pair<int,int>,File> files;
    std::map<Key,Block> blocks;
    std::deque<Key> jobs;
    bool stop=false;
    uint64_t ticks=0;
    void run();
    ~Terrain() { shutdown(); }
    void shutdown() {
        { std::lock_guard<APC_Mutex> g(mutex); stop=true; wake.signal(); }
        if(worker.joinable()) worker.join();
    }
} db;
void Terrain::run() {

    auto check=clock_type::now();
    while(true) {
        std::unique_lock<APC_Mutex> lock(mutex);
        if(stop) break;
        if(clock_type::now()>=check) {
            lock.unlock();
            char version[64]{};
            FILE *manifest=fopen((root+"/current").c_str(),"r");
            if(manifest) { if(!fgets(version,sizeof(version),manifest)) version[0]=0; fclose(manifest); }
            version[strcspn(version,"\r\n")]=0;
            bool valid=!strncmp(version,"dataset-",8) && strlen(version)>8;
            for(const char *p=version;*p;p++) if(!isalnum((unsigned char)*p) && *p!='-') valid=false;
            if(!valid) version[0]=0;
            lock.lock();
            if(active!=version) {
                files.clear(); blocks.clear(); jobs.clear(); active=version;
            }
            check=clock_type::now()+std::chrono::seconds(1);
        }
        if(jobs.empty()) {
            // pthread_cond_clockwait needs glibc 2.30; Z1-Mini has 2.25.
            // Keep manifest scheduling monotonic but use a realtime deadline
            // with the compatible pthread_cond_timedwait wrapper.
            auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(check-clock_type::now()).count();
            if(remaining>0) {
                timespec until{}; clock_gettime(CLOCK_REALTIME,&until);
                until.tv_sec+=remaining/1000000000;
                until.tv_nsec+=remaining%1000000000;
                if(until.tv_nsec>=1000000000) { ++until.tv_sec; until.tv_nsec-=1000000000; }
                wake.wait_until(mutex,until);
            }
            continue;
        }
        Key key=jobs.front(); jobs.pop_front();
        char name[32]; snprintf(name,sizeof(name),"/%c%02d%c%03d.DAT",key.lat<0?'S':'N',abs(key.lat),key.lon<0?'W':'E',abs(key.lon));
        const auto filekey=std::make_pair(key.lat,key.lon);
        unsigned spacing=files[filekey].spacing;
        lock.unlock();
        int fd=active.empty()?-1:open((root+"/"+active+name).c_str(),O_RDONLY|O_CLOEXEC);
        Block block;
        if(key.x==UINT32_MAX) {
            // Upload validation guarantees a populated first block. Partial
            // tiles still work: scan in the I/O thread, with bounded memory.
            if(fd>=0) {
                struct stat st{}; fstat(fd,&st);
                for(off_t off=0;off<st.st_size;off+=2048) {
                    if(pread(fd,block.data.data(),2048,off)!=2048) break;
                    if(ap_terrain::valid(block.data.data())) { spacing=ap_terrain::u16(block.data.data()+20); break; }
                }
            }
        } else if(fd>=0 && spacing) {
            uint64_t index=uint64_t(ap_terrain::stride(key.lat,spacing))*key.x+key.y;
            auto b=block.data.data();
            block.ready=pread(fd,b,2048,off_t(index*2048))==2048 && ap_terrain::valid(b) &&
                ap_terrain::u16(b+20)==spacing && ap_terrain::located(b,key.lat,key.lon,index);
        }
        if(fd>=0) close(fd);
        lock.lock();
        if(key.x==UINT32_MAX) files[filekey]={spacing,false};
        else {
            if(blocks.size()>=512) {
                auto oldest=std::min_element(blocks.begin(),blocks.end(),[](const auto &a,const auto &b){return a.second.used<b.second.used;});
                blocks.erase(oldest);
            }
            block.used=++ticks; blocks[key]=block;
        }
    }
}
}
void ca_terrain_start(const char *directory) {
    db.shutdown();
    std::lock_guard<APC_Mutex> g(db.mutex);
    db.root=directory; db.active.clear(); db.files.clear(); db.blocks.clear(); db.jobs.clear(); db.stop=false;
    db.worker=std::thread(&Terrain::run,&db);
}
void ca_terrain_stop() { db.shutdown(); }
ca_terrain_state ca_terrain_height(int32_t lat,int32_t lon,float &height) {
    if(lat<=-900000000 || lat>=900000000 || lon< -1800000000 || lon>=1800000000) return CA_TERRAIN_MISSING;
    std::lock_guard<APC_Mutex> g(db.mutex);
    if(db.stop || !db.worker.joinable()) return CA_TERRAIN_MISSING;
    int ld=ap_terrain::degree(lat), od=ap_terrain::degree(lon);
    auto fk=std::make_pair(ld,od);
    auto file=db.files.find(fk);
    if(file==db.files.end()) {
        if(db.jobs.size()>=64) return CA_TERRAIN_PENDING;
        if(db.files.size()>=64) db.files.clear();
        db.files[fk]={}; db.jobs.push_back({ld,od,UINT32_MAX,0}); db.wake.signal();
        return CA_TERRAIN_PENDING;
    }
    if(file->second.pending) return CA_TERRAIN_PENDING;
    unsigned spacing=file->second.spacing;
    if(!spacing) return CA_TERRAIN_MISSING;
    // AP Location::get_distance_NE uses the mean latitude, not tile latitude.
    float north=(lat-ld*10000000)*ap_terrain::metres;
    float east=(lon-od*10000000)*ap_terrain::metres*ap_terrain::scale(int32_t((int64_t(lat)+ld*10000000)/2));
    unsigned ix=north/spacing,iy=east/spacing;
    Key key{ld,od,ix/24,iy/28};
    auto block=db.blocks.find(key);
    if(block==db.blocks.end()) {
        bool queued=false;
        for(const auto &j:db.jobs) if(!(j<key) && !(key<j)) queued=true;
        // A second request while the worker reads may queue one duplicate;
        // the fixed queue bound still applies.
        if(!queued && db.jobs.size()<64) { db.jobs.push_back(key); db.wake.signal(); }
        return CA_TERRAIN_PENDING;
    }
    block->second.used=++db.ticks;
    if(!block->second.ready) return CA_TERRAIN_MISSING;
    const auto b=block->second.data.data();
    unsigned x=ix%24,y=iy%28;
    float h[2][2];
    for(unsigned i=0;i<2;i++) for(unsigned j=0;j<2;j++) {
        if(!(ap_terrain::u64(b)&(uint64_t(1)<<(((x+i)/4)*8+(y+j)/4)))) return CA_TERRAIN_MISSING;
        h[i][j]=int16_t(ap_terrain::u16(b+22+2*((x+i)*32+y+j)));
    }
    float fx=(north-ix*spacing)/spacing,fy=(east-iy*spacing)/spacing;
    height=((1-fx)*h[0][0]+fx*h[1][0])*(1-fy)+((1-fx)*h[0][1]+fx*h[1][1])*fy;
    return CA_TERRAIN_READY;
}
