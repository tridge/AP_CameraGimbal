#pragma once
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
// Credentials follow the persistent camera.ini, including portable SITL roots.
// The transfer executable and CA bundle travel beside the actual camera binary.
static inline bool apcam_upload_sibling(char *out,size_t size,const char *path,const char *name)
{
    const char *slash=strrchr(path,'/');
    int n=slash ? snprintf(out,size,"%.*s/%s",int(slash-path),path,name) : snprintf(out,size,"%s",name);
    return n>=0 && size_t(n)<size;
}

static inline bool apcam_upload_executable(char *out,size_t size)
{
#ifdef __APPLE__
    char path[4096];uint32_t length=sizeof(path);
    if(_NSGetExecutablePath(path,&length)!=0) return false;
    char *resolved=realpath(path,nullptr);
    if(!resolved) return false;
    int n=snprintf(out,size,"%s",resolved);free(resolved);
    return n>=0 && size_t(n)<size;
#else
    if(size<2) return false;
    ssize_t n=readlink("/proc/self/exe",out,size-1);
    if(n<=0 || size_t(n)>=size-1) return false;
    out[n]=0;return true;
#endif
}
