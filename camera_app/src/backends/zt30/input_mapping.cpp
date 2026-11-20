#include "input_mapping.h"
#include <cstring>
namespace ca_zt30 {
bool read_input_mappings(FILE *file,std::vector<InputMapping> &out) {
    out.clear();if(!file)return false;
    char line[1024];
    while(std::fgets(line,sizeof(line),file)) {
        if(out.size()>=512 || !std::strchr(line,'\n')) { out.clear();return false; }
        unsigned long long first=0,last=0,offset=0,inode=0;
        unsigned major=0,minor=0;char perms[5]{};int end=0;
        if(std::sscanf(line,"%llx-%llx %4s %llx %x:%x %llu %n",&first,&last,perms,&offset,&major,&minor,&inode,&end)!=7 ||
           !end || !first || first>=last || last>UINT64_C(0x100000000)) { out.clear();return false; }
        char *path=line+end;while(*path==' ')++path;
        path[std::strcspn(path,"\n")]=0;
        const bool zero=!std::strcmp(perms,"rw-s") && !offset && !std::strcmp(path,"/dev/zero (deleted)");
        out.push_back({first,last,zero});
    }
    if(std::ferror(file) || out.empty()) { out.clear();return false; }
    return true;
}
bool new_input_mapping(const std::vector<InputMapping> &before,const std::vector<InputMapping> &leased,const InputBufferInfo &info) {
    uint32_t bytes=0;uint64_t physical_y=0,physical_uv=0;
    const auto *raw=reinterpret_cast<const uint8_t*>(&info);
    std::memcpy(&bytes,raw+108,4);std::memcpy(&physical_y,raw+72,8);std::memcpy(&physical_uv,raw+80,8);
    const uint64_t first=info.plane_address[0],uv=info.plane_address[1],last=first+bytes;
    if(info.stride[0]!=640 || info.stride[1]!=640 || bytes!=491520 || uv-first!=327680 ||
       before.empty() || leased.empty() || !first || !bytes || last>UINT64_C(0x100000000) ||
       uv<=first || uv>=last || !physical_y || physical_uv<=physical_y ||
       physical_uv-physical_y!=uv-first || physical_y>UINT64_MAX-bytes)return false;
    for(const auto &m:before)if(first<m.last && m.first<last)return false;
    unsigned matches=0;
    for(const auto &m:leased)if(m.first==first && m.last==last && m.shared_zero)++matches;
    return matches==1;
}
}
