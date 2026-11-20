/* Run the real portable/Cygwin installer swap on an isolated host filesystem. */
#include <cstdio>
int test_rename(const char *, const char *);
#define main web_main
#define rename test_rename
#include "../mt11-web.cpp"
#undef rename
#undef main
#include <assert.h>

static unsigned rename_calls, fail_rename;

int test_rename(const char *a, const char *b)
{
    if (++rename_calls == fail_rename) { errno=EIO; return -1; }
    return rename(a,b);
}

static void tree(const char *directory, char marker)
{
    assert(mkdir(directory,0700)==0);
    char path[80];
    assert(snprintf(path,sizeof(path),"%s/marker",directory)<(int)sizeof(path));
    FILE *out=fopen(path,"w");
    assert(out && fputc(marker,out)!=EOF);
    assert(fclose(out)==0);
}

static char marker(const char *directory)
{
    char path[80];
    snprintf(path,sizeof(path),"%s/marker",directory);
    FILE *in=fopen(path,"r");
    assert(in);
    int value=fgetc(in);
    assert(fclose(in)==0);
    return (char)value;
}

int main(int argc, char **argv)
{
    static const char asset[] = "gcu/ap/webroot/head.html";
    static const char nul_asset[] = "gcu/ap/webroot/head.html\0suffix";
    assert(gcu_entry_name_ok(asset, sizeof(asset) - 1));
    assert(!gcu_entry_name_ok(nul_asset, sizeof(nul_asset) - 1));
    static const char traversal[] = "gcu/ap/webroot/../head.html";
    assert(!gcu_entry_name_ok(traversal, sizeof(traversal) - 1));

    assert(argc==2 && chdir(argv[1])==0);
    unsetenv("MT11_WEB_LIVE_PORT");
    unsetenv("CAMERA_APP_RTSP_PORT");
    assert(live_video_port()==8555);
    setenv("CAMERA_APP_RTSP_PORT","21000",1);
    assert(live_video_port()==21001);
    setenv("CAMERA_APP_RTSP_PORT","65535",1);
    assert(live_video_port()==8555);
    unsetenv("CAMERA_APP_RTSP_PORT");
    for (unsigned scenario=0; scenario<5; scenario++) {
        tree("stage",'N');
        if (scenario!=2) tree("live",'O');
        if (scenario==1 || scenario==2) tree("live.exchange",'O');
        rename_calls=0;
        fail_rename=scenario==3 ? 2 : scenario==4 ? 3 : 0;
        int result=exchange_paths("stage","live");
        assert(result==(fail_rename ? -1 : 0));
        assert(marker("live")== (fail_rename ? 'O' : 'N'));
        assert(marker("stage")== (fail_rename ? 'N' : 'O'));
        assert(access("live.exchange",F_OK)<0 && errno==ENOENT);
        assert(remove_path_tree("stage") && remove_path_tree("live"));
    }
    puts("PASS portable exchange: normal, stale backup, missing live tree and rename rollback");
    return 0;
}
