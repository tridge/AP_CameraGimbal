#include "../zt30-process.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <filesystem>

#ifdef __linux__
static void slot_payload(const std::string &path)
{
    std::filesystem::create_directories(path + "/webroot");
    chmod((path.substr(0, path.rfind('/'))).c_str(),0700);
    chmod(path.c_str(),0700); chmod((path+"/webroot").c_str(),0700);
    for (const char *name : {"camera-app", "zt30-web"}) {
        std::filesystem::copy_file("/proc/self/exe",path+"/"+name);
        chmod((path+"/"+name).c_str(),0700);
    }
    for (const char *name : {"encoder-sdk.so", "webroot/live.js", "webroot/status-actions.html", "webroot/parameters-form.html"}) {
        std::ofstream out(path+"/"+name);out<<"fixture";out.close();chmod((path+"/"+name).c_str(),0600);
    }
}
static pid_t child(const std::string &program, const std::string &argument, const std::string &config)
{
    int pipes[2];assert(pipe(pipes)==0);
    pid_t pid=fork();assert(pid>=0);
    if (!pid) {
        close(pipes[0]);assert(fcntl(pipes[1],F_SETFD,FD_CLOEXEC)==0);
        if (config.empty()) unsetenv("CAMERA_APP_CONFIG");
        else setenv("CAMERA_APP_CONFIG",config.c_str(),1);
        execl(program.c_str(),argument.c_str(),"wait",(char*)nullptr);_exit(127);
    }
    close(pipes[1]);char c;assert(read(pipes[0],&c,1)==0);close(pipes[0]);return pid;
}
static void slot_processes(const std::string &root)
{
    std::string a=root+"/slots/"+std::string(64,'a'), b=root+"/slots/"+std::string(64,'b');
    slot_payload(a);slot_payload(b);
    assert(symlink(("slots/"+std::string(64,'a')).c_str(),(root+"/active").c_str())==0);
    assert(symlink("active/camera-app",(root+"/camera-app").c_str())==0);
    std::string config=root+"/camera.ini", old=a+"/camera-app", next=b+"/camera-app";
    pid_t pinned=child(old,old,config), stable=child(root+"/camera-app",root+"/camera-app",config);
    std::string error;
    assert(apcam_zt30::activate(root,std::string(64,'b').c_str(),error));
    assert(zt30_slot_app_pid(pinned,old.c_str(),root.c_str()));
    assert(zt30_slot_app_pid(stable,old.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(stable,next.c_str(),root.c_str()));
    pid_t newer=child(next,next,config), foreign=child(old,old,root+"/other.ini"), spoof=child(old,next,config);
    pid_t missing=child(old,old,""), outside=child(old,"/unmanaged/camera-app",config);
    assert(zt30_slot_app_pid(newer,next.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(foreign,old.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(spoof,old.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(pinned,old.c_str(),"/unrelated/root"));
    assert(!zt30_slot_app_pid(missing,old.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(outside,old.c_str(),root.c_str()));
    // Metadata drift must not make a retained, already running app unstoppably
    // invisible. Incomplete slots remain ineligible for a new launch.
    chmod((a+"/webroot/live.js").c_str(),0666);
    assert(zt30_slot_app_pid(pinned,old.c_str(),root.c_str()));
    assert(unlink(old.c_str())==0);
    assert(zt30_slot_app_pid(pinned,old.c_str(),root.c_str()));
    assert(!zt30_slot_app_pid(spoof,old.c_str(),root.c_str()));
    std::ofstream replacement(old); replacement<<"replacement inode";replacement.close();chmod(old.c_str(),0600);
    assert(zt30_slot_app_pid(pinned,old.c_str(),root.c_str()));
    assert(zt30_slot_app_pid(stable,old.c_str(),root.c_str()));
    for(pid_t pid:{pinned,stable,newer,foreign,spoof,missing,outside}) {kill(pid,SIGTERM);int status;assert(waitpid(pid,&status,0)==pid);}
}
int main(int argc, char **) {
    if (argc > 1) { while (true) pause(); }
    char root[]="/tmp/zt30-process-XXXXXX";
    assert(mkdtemp(root));
    std::string prefix=std::string(root)+"/stage.";
    std::string stage=prefix+"aB0123";
    assert(mkdir(stage.c_str(),0700)==0);
    std::string original=stage+"/camera-app", installed=std::string(root)+"/camera-app";
    std::ifstream src("/proc/self/exe",std::ios::binary);
    std::ofstream dst(original,std::ios::binary); dst<<src.rdbuf();dst.close();
    assert(chmod(original.c_str(),0700)==0);
    assert(rename(original.c_str(),installed.c_str())==0);
    int pipefd[2];assert(pipe(pipefd)==0);
    pid_t pid=fork();assert(pid>=0);
    if (!pid) {
        close(pipefd[0]);assert(fcntl(pipefd[1],F_SETFD,FD_CLOEXEC)==0);
        execl(installed.c_str(),installed.c_str(),"wait",(char*)nullptr);_exit(127);
    }
    close(pipefd[1]);char c;assert(read(pipefd[0],&c,1)==0);close(pipefd[0]);
    // Replace the running inode, just as the filesystem updater does.
    assert(unlink(installed.c_str())==0);
    char proc[64],exe[4096];snprintf(proc,sizeof(proc),"/proc/%ld/exe",(long)pid);
    ssize_t n=readlink(proc,exe,sizeof(exe)-1);assert(n>0);exe[n]=0;
    std::string observed=exe;
    assert(observed.size()>10 && observed.substr(observed.size()-10)==" (deleted)");
    observed.resize(observed.size()-10);
    // Kernels may report either the canonical or the original staging path.
    assert(observed==installed || observed==original);
    assert(zt30_staged_app_pid(pid,original.c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,original.c_str(),"/unrelated/camera-app",prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,(prefix+"bad/camera-app").c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,(original+".other").c_str(),installed.c_str(),prefix.c_str()));
    for (const char *tail : {"abc12/camera-app", "abc1234/camera-app", "ab.123/camera-app", "ab/123/camera-app", "abc123/camera-apx", "abc123camera-app/"})
        assert(!zt30_staged_app_pid(pid,(prefix+tail).c_str(),installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,"camera-app",installed.c_str(),prefix.c_str()));
    assert(!zt30_staged_app_pid(pid,original.c_str(),(installed+"-other").c_str(),prefix.c_str()));
    kill(pid,SIGTERM);int status;waitpid(pid,&status,0);rmdir(stage.c_str());
    slot_processes(root);
    std::filesystem::remove_all(root);
    puts("ZT30 staged and retained-slot executable identity PASS");
}

#else
int main() { puts("SKIP ZT30 staged executable identity: Linux procfs required"); }
#endif
