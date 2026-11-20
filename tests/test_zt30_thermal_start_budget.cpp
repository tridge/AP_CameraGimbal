#include "../camera_app/src/backends/zt30/thermal_start_budget.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
using ca_zt30::ThermalStartBudget;
int main() {
    char dir[]="/tmp/apcg-thermal-budget-XXXXXX";assert(mkdtemp(dir));
    const std::string path=std::string(dir)+"/budget";
    for(unsigned i=0;i<3;++i) { ThermalStartBudget b;assert(b.begin(path.c_str())); }
    { ThermalStartBudget b;assert(!b.begin(path.c_str())); }
    assert(unlink(path.c_str())==0);
    for(unsigned i=0;i<6;++i) { ThermalStartBudget b;assert(b.begin(path.c_str()));assert(b.reset()); }
    { FILE *f=fopen(path.c_str(),"w");assert(f);fputs("bad",f);fclose(f); }
    { ThermalStartBudget b;assert(!b.begin(path.c_str())); }
    assert(unlink(path.c_str())==0);
    for(const char *text : {"4\n","1","","333"}) {
        FILE *f=fopen(path.c_str(),"w");assert(f);fputs(text,f);fclose(f);assert(chmod(path.c_str(),0600)==0);
        ThermalStartBudget b;
        if(*text)assert(!b.begin(path.c_str()));else assert(b.begin(path.c_str()));
    }
    assert(chmod(path.c_str(),0644)==0);
    { ThermalStartBudget b;assert(!b.begin(path.c_str())); }
    assert(chmod(path.c_str(),0600)==0);
    const std::string other=std::string(dir)+"/other";assert(link(path.c_str(),other.c_str())==0);
    { ThermalStartBudget b;assert(!b.begin(path.c_str())); }
    assert(unlink(other.c_str())==0);assert(unlink(path.c_str())==0);
    assert(symlink("missing",path.c_str())==0);
    { ThermalStartBudget b;assert(!b.begin(path.c_str())); }
    assert(unlink(path.c_str())==0);assert(rmdir(dir)==0);
    puts("ZT30 thermal startup budget tests passed");
}
