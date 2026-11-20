/* Separate, gated Phase B build of the qualified metadata forwarding shell.
 * The default ioctl-trace.so compiles without these hooks or pointer reads. */
#define ZT30_IOCTL_PAYLOAD_TRACE 1
#include "zt30_ioctl_trace.c"
#ifndef ZT30_PAYLOAD_LIMIT
#define ZT30_PAYLOAD_LIMIT LIMIT
#endif
static int payload_counts_fd=-1;
static struct zt30_trace_output payload_output={.fd=-1},payload_counts_output={.fd=-1};
static uint32_t payload_cursor=8,payload_count,payload_errors,payload_cap,payload_unavailable;
static ssize_t payload_write(int fd,const void *bytes,size_t length,uint32_t offset) {
    const struct zt30_trace_output *file=fd==payload_fd?&payload_output:&payload_counts_output;
    if(!zt30_trace_output_matches(file))return -1;
#if defined(__arm__) && __SIZEOF_POINTER__==4
    return syscall(SYS_pwrite64,fd,bytes,length,0,offset,0);
#else
    return syscall(SYS_pwrite64,fd,bytes,length,(off_t)offset);
#endif
}
static void payload_begin(const char *directory) {
    char path[160];
    if(zt30_self_reader_init(&payload_reader))return;
    if(snprintf(path,sizeof(path),"%s/ioctl-payload.bin",directory)>=(int)sizeof(path))goto failed;
    payload_fd=zt30_trace_output_open(&payload_output,path);
    if(payload_fd<0)goto failed;
    if(snprintf(path,sizeof(path),"%s/ioctl-payload-counts.bin",directory)>=(int)sizeof(path))goto failed;
    payload_counts_fd=zt30_trace_output_open(&payload_counts_output,path);
    if(payload_counts_fd<0 || payload_write(payload_fd,"ZT30PAY1",8,0)!=8)goto failed;
    return;
failed:
    if(payload_counts_fd>=0)syscall(SYS_close,payload_counts_fd);
    payload_counts_fd=-1;
    if(payload_fd>=0)syscall(SYS_close,payload_fd);
    payload_fd=-1;zt30_self_reader_close(&payload_reader);
}
static void payload_store(const struct zt30_payload_record *record) {
    uint32_t position;
    for(unsigned attempt=0;;attempt++) {
        if(attempt==32) {__sync_fetch_and_add(&payload_errors,1);payload_write(payload_fd,"FAILPAY1",8,0);return;}
        position=__atomic_load_n(&payload_cursor,__ATOMIC_RELAXED);
        if(position>ZT30_PAYLOAD_LIMIT-sizeof(*record)) {__sync_lock_test_and_set(&payload_cap,1);payload_write(payload_fd,"CAPLPAY1",8,0);return;}
        if(__sync_bool_compare_and_swap(&payload_cursor,position,position+sizeof(*record)))break;
    }
    ssize_t written;
    for(unsigned attempt=0;;attempt++) {
        written=payload_write(payload_fd,record,sizeof(*record),position);
        if(written>=0 || errno!=EINTR || attempt==7)break;
    }
    if(written!=(ssize_t)sizeof(*record)) {__sync_fetch_and_add(&payload_errors,1);payload_write(payload_fd,"FAILPAY1",8,0);return;}
    __sync_fetch_and_add(&payload_count,1);
    if(!(record->flags&ZT30_PAYLOAD_COMPLETE))__sync_fetch_and_add(&payload_unavailable,1);
}
static void payload_finish(void) {
    if(payload_fd<0 || getpid()!=owner)return;
    uint64_t ns=clock_ns();uint32_t footer[8]={0};memcpy(footer,"ZT30PAC1",8);
    footer[2]=__atomic_load_n(&payload_count,__ATOMIC_RELAXED);footer[3]=__atomic_load_n(&payload_errors,__ATOMIC_RELAXED);
    footer[4]=__atomic_load_n(&payload_cap,__ATOMIC_RELAXED);footer[5]=__atomic_load_n(&payload_unavailable,__ATOMIC_RELAXED);
    footer[6]=(uint32_t)ns;footer[7]=(uint32_t)(ns>>32);
    if(payload_write(payload_counts_fd,footer,sizeof(footer),0)!=sizeof(footer))payload_write(payload_fd,"FAILPAY1",8,0);
    /* These descriptors belong to a process-lifetime preload. Kernel exit
     * closes them; do not close a reader FD the application may have reused. */
}
