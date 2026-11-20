#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ca_zt30 {
// tmpfs lifetime is one boot. Charge before touching USB/SDK; ambiguous
// process exits retain the charge. Sustained output or clean shutdown resets it.
class ThermalStartBudget {
public:
    ~ThermalStartBudget() { if(fd>=0)::close(fd); }
    bool begin(const char *path) {
        if(fd>=0)return false;
        fd=::open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        struct stat st{};
        if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
           (st.st_mode&0777)!=0600 || st.st_nlink!=1 || st.st_size>2)return false;
        unsigned attempts=0;
        if(st.st_size) {
            char text[2]{};
            if(st.st_size!=2 || pread(fd,text,2,0)!=2 || text[0]<'0' || text[0]>'3' || text[1]!='\n')return false;
            attempts=unsigned(text[0]-'0');
        }
        return attempts<3 && write(attempts+1);
    }
    bool reset() { return fd>=0 && write(0); }
private:
    bool write(unsigned count) {
        const char text[2]={char('0'+count),'\n'};
        return pwrite(fd,text,2,0)==2 && ftruncate(fd,2)==0 && fsync(fd)==0;
    }
    int fd=-1;
};
}
