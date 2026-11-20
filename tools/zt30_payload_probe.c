/* Own stack fixtures only: no SDK, driver ioctl or vendor attachment. */
#define _GNU_SOURCE
#include "zt30_ioctl_payload.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/wait.h>
static struct zt30_self_reader reader={.fd=-1};
static void descriptor_for(uint32_t desc[4],const void *pointer,unsigned length) {
    uintptr_t address=(uintptr_t)pointer;desc[0]=length;desc[1]=0;
    desc[2]=(uint32_t)address;
#if defined(__arm__)
    desc[3]=desc[2]&0x80000000U?UINT32_MAX:0;
#else
    desc[3]=(uint32_t)(address>>32);
#endif
}
static void *cross_thread(void *p) {struct zt30_stack_guard *g=p;return (void*)(uintptr_t)zt30_stack_guard_check(g,g->low+4,4);}
#define REQUIRE(x,code) do {if(!(x)) {printf("FAIL code=%u line=%u\n",(unsigned)(code),(unsigned)__LINE__);return (code);}} while(0)
int main(int argc,char **argv) {
    (void)argv;if(argc!=1)return 2;setvbuf(stdout,NULL,_IOLBF,0);
    errno=EINTR;REQUIRE(!zt30_self_reader_init(&reader) && errno==EINTR,3);
    uint32_t params[2]={1,6},desc[4];struct zt30_payload_call call;struct zt30_payload_record rec;
    descriptor_for(desc,params,8);uint64_t cold=zt30_guard_now();
    REQUIRE(zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,123,&call,&rec),4);
    cold=zt30_guard_now()-cold;
    REQUIRE(errno==EINTR && rec.flags==(ZT30_PAYLOAD_DESC|ZT30_PAYLOAD_COMPLETE) && rec.call_id==123 && rec.phase=='B' && rec.capture_length==4 && !memcmp(rec.data,params,4) && !rec.data[4],5);
    memset(desc,0,sizeof(desc));params[1]=9;
    REQUIRE(zt30_payload_after(&reader,&call,-7,&rec),6);
    REQUIRE(errno==EINTR && rec.raw_status==-7 && rec.phase=='A' && rec.flags==(ZT30_PAYLOAD_DESC|ZT30_PAYLOAD_COMPLETE|ZT30_PAYLOAD_POST) && !memcmp(rec.data,params,8) && !call.guard.owner,7);
    puts("CASE pre_post_original_pointer PASS");
    descriptor_for(desc,params,8);zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,124,&call,&rec);
    REQUIRE(zt30_payload_after(&reader,&call,0,&rec) && (rec.flags&ZT30_PAYLOAD_RAW_ZERO),8);puts("CASE post_raw_zero_flag PASS");
    REQUIRE(zt30_payload_before(&reader,1,0xc0086908U,230,62,1,125,&call,&rec) && rec.copy_error==EPERM && !rec.flags,9);
    REQUIRE(zt30_payload_after(&reader,&call,0,&rec),10);
    REQUIRE(rec.flags==(ZT30_PAYLOAD_POST|ZT30_PAYLOAD_RAW_ZERO) && rec.bytes==-1,11);puts("CASE bad_descriptor_unavailable PASS");
    descriptor_for(desc,params,4096);zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,126,&call,&rec);
    REQUIRE(rec.copy_error==EINVAL && !rec.flags && !call.descriptor_valid,12);
    zt30_payload_after(&reader,&call,0,&rec);puts("CASE exact_length PASS");
    void *heap=malloc(16);REQUIRE(heap,13);descriptor_for(desc,heap,8);zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,127,&call,&rec);
    REQUIRE(rec.copy_error==EPERM && !rec.flags && !call.descriptor_valid,14);free(heap);zt30_payload_after(&reader,&call,0,&rec);puts("CASE heap_payload_rejected PASS");
    uintptr_t p=0;REQUIRE(!zt30_arm_payload_pointer(0x80001000,UINT32_MAX,&p) && p==0x80001000U,15);
    REQUIRE(zt30_arm_payload_pointer(0x80001000,0,&p) && zt30_arm_payload_pointer(0x70001000,UINT32_MAX,&p) && zt30_arm_payload_pointer(0xc0000000,UINT32_MAX,&p) && zt30_arm_payload_pointer(0x80001001,UINT32_MAX,&p),16);puts("CASE arm_pointer_sign_and_bounds PASS");
    int saved_fd=reader.fd;reader.fd=-1;
    REQUIRE(!zt30_payload_before(&reader,0,0x80046900U,230,63,1,128,&call,&rec),17);
    REQUIRE(!zt30_payload_before(&reader,6,0,229,0,1,128,&call,&rec),18);
    REQUIRE(!zt30_payload_before(&reader,2,0xc0086908U,230,46,1,128,&call,&rec),19);
    REQUIRE(!zt30_payload_before(&reader,1,0xc0086908U,230,0,1,128,&call,&rec),28);
    REQUIRE(!zt30_payload_before(&reader,1,0xc0086908U,229,62,1,128,&call,&rec),28);
    reader.fd=saved_fd;puts("CASE scalar_unknown_and_wrong_device PASS");
    descriptor_for(desc,params,8);zt30_payload_before(&reader,0,0x40086908U,230,63,(uintptr_t)desc,129,&call,&rec);
    REQUIRE((rec.flags&ZT30_PAYLOAD_COMPLETE) && !zt30_payload_after(&reader,&call,-1,&rec) && !call.guard.owner,20);puts("CASE input_only_no_post PASS");
    struct zt30_stack_guard g;REQUIRE(!zt30_stack_guard_init(&g,(uintptr_t)&g),21);
    pthread_t thread;void *thread_result=NULL;REQUIRE(!pthread_create(&thread,NULL,cross_thread,&g) && !pthread_join(thread,&thread_result) && (uintptr_t)thread_result==EPERM,22);
    struct zt30_stack_guard hit;REQUIRE(!zt30_stack_guard_init(&hit,(uintptr_t)&g) && hit.expires_ns==g.expires_ns,23);
    g.expires_ns=1;REQUIRE(zt30_stack_guard_check(&g,(uintptr_t)params,8)==EPERM,24);puts("CASE tid_cache_hit_and_expiry PASS");
    uint32_t attributes[24];unsigned char *attribute_bytes=(unsigned char*)attributes;
    for(unsigned j=0;j<sizeof(attributes);j++)attribute_bytes[j]=(unsigned char)(37*j+7);
    for(unsigned i=0;i<sizeof(zt30_ioctl_layouts)/sizeof(*zt30_ioctl_layouts);i++) {
        const struct zt30_ioctl_layout *layout=&zt30_ioctl_layouts[i];
        descriptor_for(desc,attributes,layout->length);desc[1]=0xabcdef01U;
        const struct zt30_ioctl_device_id *dev=&zt30_ioctl_payload_ids[layout->device];
        REQUIRE(zt30_payload_before(&reader,layout->device,layout->command,dev->major,dev->minor,(uintptr_t)desc,150+i,&call,&rec) && (rec.flags&ZT30_PAYLOAD_COMPLETE),29);
        REQUIRE(rec.capture_length==layout->pre_length && !rec.descriptor[1],30);
        for(unsigned j=0;j<sizeof(rec.data);j++)REQUIRE(rec.data[j]==(j<layout->pre_length?attribute_bytes[j]:0),31);
        REQUIRE(zt30_payload_after(&reader,&call,0,&rec)==layout->post,32);
        if(layout->post)REQUIRE((rec.flags&ZT30_PAYLOAD_COMPLETE) && rec.capture_length==layout->length && !memcmp(rec.data,attributes,layout->length),34);
    }
    puts("CASE all_registry_masks PASS");
    descriptor_for(desc,params,8);zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,180,&call,&rec);call.guard.expires_ns=1;
    REQUIRE(zt30_payload_after(&reader,&call,0,&rec) && (rec.flags&ZT30_PAYLOAD_COMPLETE) && (rec.flags&ZT30_PAYLOAD_REFRESHED),33);
    puts("CASE slow_call_guard_refresh PASS");
    descriptor_for(desc,params,8);uint64_t start=zt30_guard_now();
    for(unsigned i=0;i<1000;i++) {
        REQUIRE(zt30_payload_before(&reader,1,0xc0086908U,230,62,(uintptr_t)desc,200+i,&call,&rec) && (rec.flags&ZT30_PAYLOAD_COMPLETE),25);
        REQUIRE(zt30_payload_after(&reader,&call,0,&rec) && (rec.flags&ZT30_PAYLOAD_COMPLETE),26);
    }
    uint64_t elapsed=zt30_guard_now()-start;
    printf("TIMING cold_ns=%llu cached_1000_ns=%llu\n",(unsigned long long)cold,(unsigned long long)elapsed);
    REQUIRE(errno==EINTR,27);zt30_self_reader_close(&reader);
    puts("RESULT PASS cases=11 copies=own-stack-only");return 0;
}
