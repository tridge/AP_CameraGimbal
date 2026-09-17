/* Exercise the real helper overlay path without AX hardware. */
#define main capture_main
#define dlsym test_dlsym
#define ca_overlay_bitmaps test_bitmaps
#include "../src/backends/z1mini/ax_capture.c"
#undef ca_overlay_bitmaps
#undef dlsym
#undef main
#include <assert.h>

extern int ca_overlay_bitmaps(struct ca_overlay_bitmap [CA_OVERLAY_REGIONS], unsigned, unsigned,
                             const struct ca_overlay_geometry *);
static unsigned allocations, mappings, inject_sequence;
static bool fail_allocation;
static int request_fd;

static void request(unsigned sequence, bool enabled)
{
    struct ca_z1_overlay_request value={.sequence=sequence,.desired=enabled,.reserved={0}};
    assert(write(request_fd,&value,sizeof(value))==sizeof(value));
    read_overlay_request();
}

int test_bitmaps(struct ca_overlay_bitmap bits[CA_OVERLAY_REGIONS], unsigned width, unsigned height,
                 const struct ca_overlay_geometry *geometry)
{
    allocations++;
    if (fail_allocation) { errno=ENOMEM; return -1; }
    return ca_overlay_bitmaps(bits,width,height,geometry);
}

static void *map_pixels(AX_U64 address, AX_U32 size)
{
    (void)address;
    mappings++;
    if (inject_sequence) {
        unsigned sequence=inject_sequence;
        inject_sequence=0;
        request(sequence,false);
    }
    return calloc(1,size);
}

static AX_S32 unmap_pixels(void *memory, AX_U32 size)
{
    (void)size;
    free(memory);
    return 0;
}

void *test_dlsym(void *handle, const char *name)
{
    (void)handle;
    if (!strcmp(name,"AX_SYS_Mmap")) return map_pixels;
    if (!strcmp(name,"AX_SYS_Munmap")) return unmap_pixels;
    return NULL;
}

static void ack(FILE *out, unsigned sequence, bool enabled, unsigned error)
{
    struct ca_z1_native_header header;
    assert(fseek(out,-(long)sizeof(header),SEEK_END)==0);
    assert(fread(&header,1,sizeof(header),out)==sizeof(header));
    assert(header.magic==CA_Z1_NATIVE_OVERLAY_MAGIC && header.pts==sequence);
    assert(header.stream==enabled && header.key==error);
    assert(fseek(out,0,SEEK_END)==0);
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    if (sockets[0]!=3) { assert(dup2(sockets[0],3)==3); close(sockets[0]); }
    request_fd=sockets[1];
    FILE *out=tmpfile();
    assert(out);
    AX_VIDEO_FRAME_S frame={0};
    frame.u32Width=1920; frame.u32Height=1080;
    frame.u32PicStride[0]=frame.u32PicStride[1]=1920;
    frame.enCompressMode=AX_COMPRESS_MODE_NONE;
    struct overlay_cache cache[2]={0};
    for (unsigned c=0;c<2;c++) overlay_frame(c,&frame,out,&cache[c]);
    assert(allocations==0 && mappings==0 && ftell(out)==0);
    request(1,true);
    fail_allocation=true;
    for (unsigned c=0;c<2;c++) overlay_frame(c,&frame,out,&cache[c]);
    ack(out,1,true,ENOMEM);
    assert(allocations==2 && mappings==0 && !stopped);
    overlay_frame(0,&frame,out,&cache[0]);
    assert(allocations==2); /* Retry only on a new request, not every frame. */
    request(2,true);
    fail_allocation=false;
    for (unsigned c=0;c<2;c++) overlay_frame(c,&frame,out,&cache[c]);
    ack(out,2,true,0);
    assert(allocations==4 && mappings==4 && !stopped);

    request(3,true);
    overlay_frame(0,&frame,out,&cache[0]);
    long before=ftell(out);
    inject_sequence=4; /* A new OFF request arrives while channel 1 draws ON. */
    overlay_frame(1,&frame,out,&cache[1]);
    assert(ftell(out)==before); /* Old work must not acknowledge sequence 4. */
    for (unsigned c=0;c<2;c++) overlay_frame(c,&frame,out,&cache[c]);
    ack(out,4,false,0);
    for (unsigned c=0;c<2;c++) ca_overlay_free(cache[c].bits);
    fclose(out);
    close(request_fd); close(3);
    puts("PASS native overlay: disabled allocation, failed allocation/retry, concurrent request acknowledgement");
    return 0;
}
