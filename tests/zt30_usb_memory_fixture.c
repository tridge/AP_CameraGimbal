#define _GNU_SOURCE
#include "zt30_usb_urb_snapshot.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
static struct zt30_self_reader reader;
static const struct zt30_usb_urb_header value={.type=2,.endpoint=0,.flags=2,.buffer=0xdeadbeef,
    .buffer_length=42,.usercontext=0x81234567};
static int zero(const void *p,size_t n) {const unsigned char *b=p;for(size_t i=0;i<n;i++)if(b[i])return 0;return 1;}
static void good(uintptr_t p) {
    struct zt30_usb_urb_header out;errno=EINTR;
    struct zt30_copy_result r=zt30_usb_urb_snapshot(&reader,p,&out);
    if(r.error)fprintf(stderr,"COPY UNAVAILABLE error=%d\n",r.error);
    assert(errno==EINTR && r.bytes==44 && !r.error && !memcmp(&out,&value,44));
}
static void unavailable(uintptr_t p) {
    struct zt30_usb_urb_header out;memset(&out,0xa5,44);errno=EINTR;
    struct zt30_copy_result r=zt30_usb_urb_snapshot(&reader,p,&out);
    assert(errno==EINTR && r.bytes==-1 && r.error && zero(&out,44));
}
struct metadata {const char *text;size_t length,offset;};
static ssize_t metadata_read(void *context,void *buffer,size_t size) {
    struct metadata *m=context;if(size>m->length-m->offset)size=m->length-m->offset;
    memcpy(buffer,m->text+m->offset,size);m->offset+=size;return (ssize_t)size;
}
static int parse(const char *text,uintptr_t source,size_t *consumed) {
    struct metadata m={text,strlen(text),0};struct zt30_usb_memory_guard g;errno=EINTR;
    int rc=zt30_usb_guard_parse(&g,metadata_read,&m,source,44,zt30_guard_now()+ZT30_USB_COPY_DEADLINE_NS);
    assert(errno==EINTR);if(consumed)*consumed=g.consumed;
    if(rc)assert(!g.owner && !g.low && !g.high);
    return rc;
}
static void parser_cases(void) {
    assert(!parse("80000000-80010000 rw-p 0 00:00 0 [heap]\nVmFlags: rd wr mr mw ac\n",0x80008000,NULL));
    assert(!parse("80000000-80010000 rw-p 0 00:00 0\nVmFlags: rd wr\n",0x80008000,NULL));
    const char *bad[]={"rwxp 0 00:00 0 [heap]","rw-s 0 00:00 0 [heap]","rw-p 1 00:00 0 [heap]",
      "rw-p 0 01:00 0 [heap]","rw-p 0 00:00 1 [heap]","rw-p 0 00:00 0 /dev/mi_sys"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {char b[256];snprintf(b,sizeof(b),"80000000-80010000 %s\nVmFlags: rd wr\n",bad[i]);assert(parse(b,0x80008000,NULL)==EPERM);}
    const char *flags[]={"rd wr io","rd wr pf","rd wr mm","rd","wr","rd wr malformed"};
    for(unsigned i=0;i<sizeof(flags)/sizeof(*flags);i++) {char b[256];snprintf(b,sizeof(b),"80000000-80010000 rw-p 0 00:00 0 [heap]\nVmFlags: %s\n",flags[i]);assert(parse(b,0x80008000,NULL)==EPERM);}
    assert(parse("80000000-80010000 rw-p 0 00:00 0 [heap]\n",0x80008000,NULL)==EPROTO);
    assert(parse("80000000-80010000 rw-p 0 00:00 0 [heap]\nVmFlags: rd wr\n",0x8000fff0,NULL)==EPERM);
    char longline[600];memset(longline,'a',sizeof(longline));longline[599]=0;assert(parse(longline,0x80008000,NULL)==E2BIG);
    assert(parse("",0xdeadbeef,NULL)==EINVAL);
    puts("PARSER PASS");
}
static void scale_case(unsigned vmas,unsigned entry_bytes,int late) {
    size_t capacity=(size_t)vmas*2048;char *text=malloc(capacity);assert(text);size_t n=0;
    for(unsigned i=0;i<vmas;i++) {
        unsigned low=i?0x10000000+i*0x4000:0x1000000;
        unsigned high=i?low+0x2000:0x10000000; /* first mapping: sparse240MiB */
        size_t start=n;n+=(size_t)snprintf(text+n,capacity-n,"%08x-%08x rw-p 0 00:00 0\nSize: 262144 kB\n",low,high);
        while(n-start+20<entry_bytes) {memcpy(text+n,"Rss: 0 kB\n",10);n+=10;}
        n+=(size_t)snprintf(text+n,capacity-n,"VmFlags: rd wr mr mw me ac\n");
    }
    text[n]=0;uintptr_t source=late?0x10000000+(vmas-1)*0x4000+16:0x1000010;
    size_t consumed=0;uint64_t start=zt30_guard_now();int rc=parse(text,source,&consumed);uint64_t elapsed=zt30_guard_now()-start;
    int expected=late && n>ZT30_SMAPS_LIMIT?E2BIG:0;
    printf("SCALE vmas=%u entry_bytes=%u late=%d bytes=%zu consumed=%zu ns=%llu rc=%d\n",vmas,entry_bytes,late,n,consumed,(unsigned long long)elapsed,rc);
    if(!late || n<=ZT30_SMAPS_LIMIT)assert(rc==0);
    else assert(rc==expected || rc==ETIMEDOUT);
    if(vmas==150 && entry_bytes==600 && late) {assert(!rc && elapsed<25000000ULL);puts("VENDOR_SCALE PASS under25ms");}
    free(text);
}
static void *small_stack(void *unused) {
    (void)unused;struct zt30_usb_urb_header local=value;good((uintptr_t)&local);puts("SMALL_STACK PASS bytes=32768");return NULL;
}
static volatile unsigned race_stop;
static void *race_address;
static void *mutate(void *unused) {
    (void)unused;
    while(!__atomic_load_n(&race_stop,__ATOMIC_RELAXED)) {
        assert(!mprotect(race_address,4096,PROT_NONE));
        assert(!mprotect(race_address,4096,PROT_READ|PROT_WRITE));
        assert(!munmap(race_address,4096));
        void *p=mmap(race_address,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);assert(p==race_address);
    }
    return NULL;
}
static void race_case(void) {
    /* Reserve both neighbors so remaps cannot replace any other mapping. */
    void *space=mmap(NULL,12288,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(space!=MAP_FAILED);
    race_address=(char*)space+4096;assert(!mprotect(race_address,4096,PROT_READ|PROT_WRITE));
    pthread_t thread;assert(!pthread_create(&thread,NULL,mutate,NULL));unsigned valid=0,missing=0;
    for(unsigned i=0;i<128;i++) {
        struct zt30_usb_urb_header out;errno=EINTR;
        struct zt30_copy_result r=zt30_usb_urb_snapshot(&reader,(uintptr_t)race_address,&out);
        assert(errno==EINTR);
        if(r.bytes==44 && !r.error)valid++;
        else {assert(r.bytes==-1 && r.error && zero(&out,44));missing++;}
    }
    __atomic_store_n(&race_stop,1,__ATOMIC_RELAXED);assert(!pthread_join(thread,NULL));assert(!munmap(space,12288));
    printf("RACE PASS valid=%u unavailable=%u atomic=0\n",valid,missing);
}
int main(void) {
    assert(!setvbuf(stdout,NULL,_IOLBF,0));
    assert(sizeof(struct zt30_usb_urb_header)==44);assert(!zt30_usb_reader_init(&reader));assert(reader.fd>=512);
    struct zt30_usb_urb_header local=value,*heap=malloc(44);assert(heap);*heap=value;
    good((uintptr_t)&local);good((uintptr_t)heap);unavailable(0);unavailable(0xdeadbeef);unavailable((uintptr_t)heap+1);
    zt30_usb_snapshot_busy=1;unavailable((uintptr_t)heap);zt30_usb_snapshot_busy=0;
    void *high=mmap((void*)(uintptr_t)0x90000000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(high!=MAP_FAILED);
    assert((uintptr_t)high>=0x80000000U);memcpy(high,&value,44);good((uintptr_t)high);printf("HIGH_POINTER PASS address=%llx\n",(unsigned long long)(uintptr_t)high);
    assert(!mprotect(high,4096,PROT_NONE));unavailable((uintptr_t)high);
    assert(!mprotect(high,4096,PROT_READ|PROT_WRITE|PROT_EXEC));unavailable((uintptr_t)high);assert(!munmap(high,4096));
    void *shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED);unavailable((uintptr_t)shared);assert(!munmap(shared,4096));
    FILE *file=tmpfile();assert(file);assert(!ftruncate(fileno(file),4096));void *backed=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fileno(file),0);assert(backed!=MAP_FAILED);unavailable((uintptr_t)backed);assert(!munmap(backed,4096));fclose(file);
    void *split=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(split!=MAP_FAILED);assert(!mprotect((char*)split+4096,4096,PROT_NONE));unavailable((uintptr_t)split+4096-16);assert(!munmap(split,8192));
    parser_cases();
    for(unsigned n=150;n<=600;n*=2)for(unsigned bytes=600;bytes<=1100;bytes+=500)for(int late=0;late<=1;late++)scale_case(n,bytes,late);
    struct zt30_usb_memory_guard g;assert(!zt30_usb_guard_init(&g,(uintptr_t)heap,44,zt30_guard_now()+ZT30_USB_COPY_DEADLINE_NS));g.tid++;assert(zt30_usb_guard_check(&g,(uintptr_t)heap,44)==EPERM);g.tid--;g.deadline=0;assert(zt30_usb_guard_check(&g,(uintptr_t)heap,44)==ETIMEDOUT);
    pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,32768));pthread_t thread;assert(!pthread_create(&thread,&attr,small_stack,NULL));assert(!pthread_join(thread,NULL));pthread_attr_destroy(&attr);
    pid_t child=fork();assert(child>=0);if(!child) {unavailable((uintptr_t)heap);_exit(0);}int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));puts("FORK PASS");
    race_case();free(heap);
    int protected_fd=reader.fd,other=open("/dev/null",O_RDONLY);assert(other>=0);assert(dup2(other,protected_fd)==protected_fd);close(other);unavailable((uintptr_t)&local);zt30_usb_reader_close(&reader);assert(fcntl(protected_fd,F_GETFD)>=0);close(protected_fd);puts("FD_REUSE PASS");
    struct zt30_usb_proc_file smaps;assert(!zt30_usb_smaps_open(&smaps));
    protected_fd=smaps.fd;other=open("/dev/null",O_RDONLY);assert(other>=0);assert(dup2(other,protected_fd)==protected_fd);close(other);
    char scratch[16];assert(zt30_usb_smaps_read(&smaps,scratch,sizeof(scratch))==-1 && errno==EPERM);
    zt30_usb_smaps_close(&smaps);assert(fcntl(protected_fd,F_GETFD)>=0);close(protected_fd);puts("SMAPS_REUSE PASS");
    puts("RESULT PASS private flat44 only no USB/SDK");return 0;
}
