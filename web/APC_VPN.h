#pragma once
// A web-owned worker supervises only its own child. Neither camera-app health
// nor HTTP request workers can prevent a saved disable from taking effect.
#include "../include/apcam/APC_Config.h"
#include "../include/apcam/sha256.h"
#include "../include/apcam/json_reader.h"
#include <sys/utsname.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctime>
#ifdef __linux__
#include <sys/sysmacros.h>
#include <sys/prctl.h>
#endif
#include <string>
#include <algorithm>
#include <cstdio>

class APC_VPN {
public:
    using Reader = bool (*)(ca_vpn_config &);
    struct Resources {
        const unsigned char *client; size_t client_size;
        const unsigned char *module; size_t module_size;
        const char *kernel_device, *kernel_hash;
        uint64_t kernel_offset, kernel_size;
    };
private:
    std::string base, run, runtime;
    const Resources *resources;
    Reader reader;
    pid_t child = -1;
    inline static volatile sig_atomic_t stopping = 0;
    static void stop_signal(int) { stopping = 1; }
    static uint64_t milliseconds() {
        timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
        return uint64_t(t.tv_sec)*1000 + t.tv_nsec/1000000;
    }
    static bool directory(const std::string &path) {
        struct stat st{};
        if (mkdir(path.c_str(), 0700) < 0 && errno != EEXIST) return false;
        return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
            st.st_uid == geteuid() && chmod(path.c_str(), 0700) == 0;
    }
    static bool write_file(const std::string &path, const std::string &data) {
        std::string temp = path + ".new";
        int fd = open(temp.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW, 0600);
        if (fd < 0) return false;
        size_t done = 0;
        while (done < data.size()) {
            ssize_t n = write(fd, data.data()+done, data.size()-done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += size_t(n);
        }
        bool ok = done == data.size() && fsync(fd) == 0;
        if (close(fd) < 0) ok = false;
        if (ok) ok = rename(temp.c_str(), path.c_str()) == 0;
        if (!ok) unlink(temp.c_str());
        return ok;
    }
    static std::string read_file(const std::string &path, size_t limit = 4096) {
        int fd = open(path.c_str(), O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if (fd < 0) return {};
        std::string out; char data[512]; ssize_t n;
        while ((n = read(fd, data, sizeof(data))) > 0) {
            if (out.size()+size_t(n) > limit) {out.clear(); break;}
            out.append(data, size_t(n));
        }
        close(fd); return out;
    }
    static void child_lifetime(pid_t parent) {
#ifdef __linux__
        // A killed/OOM supervisor must not leave an unmanaged VPN behind.
        if (prctl(PR_SET_PDEATHSIG,SIGKILL)<0 || getppid()!=parent) _exit(125);
#else
        (void)parent;
#endif
    }
    static void stop_child(pid_t &pid) {
        if (pid <= 0) return;
        pid_t observed;
        do {observed=waitpid(pid,nullptr,WNOHANG);} while(observed<0 && errno==EINTR);
        if (observed == 0) {
            kill(pid, SIGTERM);
            uint64_t until = milliseconds()+3000;
            while (waitpid(pid, nullptr, WNOHANG) == 0 && milliseconds()<until) usleep(20000);
            // The PID remains our unreaped child until waitpid succeeds.
            if (waitpid(pid, nullptr, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            }
        }
        pid = -1;
    }
    // No shell expansion. CLI output is bounded, commands have a deadline and
    // cannot inherit the supervisor lock or a web listening socket.
    void cleanup_runtime() {
        if (!resources) return;
        unlink((runtime+"/zerotier-one").c_str());
        unlink((runtime+"/tun.ko").c_str());
        rmdir(runtime.c_str());
    }
    bool prepare_resources() {
        if (!resources) return true;
        if (!directory(runtime)) return false;
        for (bool client : {true,false}) {
            const unsigned char *data=client?resources->client:resources->module;
            size_t size=client?resources->client_size:resources->module_size;
            std::string path=runtime+(client?"/zerotier-one":"/tun.ko");
            std::string content(reinterpret_cast<const char *>(data),size);
            if (read_file(path,4*1024*1024)!=content && !write_file(path,content)) return false;
            if (chmod(path.c_str(),client?0700:0600)<0) return false;
        }
        return true;
    }
    bool command(const char *action, const char *argument, std::string &output) {
        int pipefd[2]; if (pipe(pipefd) < 0) return false;
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
        pid_t owner=getpid();
        pid_t pid = fork();
        if (!pid) {
            child_lifetime(owner);
            dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]); close(pipefd[1]);
            std::string home = "-D" + base + "/state";
            setenv("LD_LIBRARY_PATH","/lib:/usr/lib",1);
            execl((runtime+"/zerotier-one").c_str(), "zerotier-cli", home.c_str(), "-j", action, argument, (char *)nullptr);
            _exit(127);
        }
        close(pipefd[1]);
        if (pid < 0) {close(pipefd[0]); return false;}
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        uint64_t until = milliseconds()+1500;
        output.clear(); int status = 0; bool exited = false, overflow = false;
        while (milliseconds()<until && !stopping) {
            char data[1024]; ssize_t n;
            while ((n=read(pipefd[0], data, sizeof(data))) > 0) {
                if (output.size()+size_t(n)>8192) {overflow=true; break;}
                output.append(data, size_t(n));
            }
            if (overflow) break;
            if (waitpid(pid, &status, WNOHANG) == pid) {exited=true; break;}
            usleep(10000);
        }
        // Drain final bytes written just before exit.
        char data[1024]; ssize_t n;
        while (!overflow && (n=read(pipefd[0], data, sizeof(data)))>0) {
            if (output.size()+size_t(n)>8192) {overflow=true; break;}
            output.append(data, size_t(n));
        }
        close(pipefd[0]);
        if (!exited) stop_child(pid);
        return exited && !overflow && WIFEXITED(status) && WEXITSTATUS(status)==0;
    }
protected:
    virtual bool tun_ready(std::string &error) {
#ifdef __linux__
        struct stat st{};
        // Test for an actually registered driver, rather than a stale device node.
        FILE *misc=fopen("/proc/misc","r");
        char line[256];
        bool registered=false;
        while (misc && fgets(line,sizeof(line),misc)) {
            unsigned minor=0;char name[128]{};
            if (sscanf(line,"%u %127s",&minor,name)==2 && minor==200 && !strcmp(name,"tun")) {
                registered=true;break;
            }
        }
        if (misc) fclose(misc);
        if (!registered) {
            if (!resources) {error="VPN TUN support is not installed for this kernel"; return false;}
            int fd=open(resources->kernel_device,O_RDONLY|O_CLOEXEC);
            apcam::SHA256 hash;
            uint64_t remaining=resources->kernel_size;
            if (fd<0) {error="Cannot verify VPN kernel compatibility"; return false;}
            if (lseek(fd,off_t(resources->kernel_offset),SEEK_SET)!=off_t(resources->kernel_offset)) {
                close(fd);error="Cannot read VPN kernel image";return false;
            }
            char data[16384];
            while (remaining) {
                size_t wanted=remaining<sizeof(data)?size_t(remaining):sizeof(data);
                ssize_t n=read(fd,data,wanted);
                if (n<0 && errno==EINTR) continue;
                if (n<=0) break;
                hash.update(data,size_t(n)); remaining-=uint64_t(n);
            }
            close(fd);
            if (remaining || hash.finish()!=resources->kernel_hash) {
                error="VPN kernel differs from the qualified TUN module"; return false;
            }
            pid_t owner=getpid(),pid=fork();
            if (!pid) {
                child_lifetime(owner);
                execlp("insmod","insmod",(runtime+"/tun.ko").c_str(),(char *)nullptr); _exit(127);
            }
            if (pid<0) {error="Cannot load VPN TUN module"; return false;}
            int status=0;bool exited=false;
            const uint64_t until=milliseconds()+3000;
            while (!stopping && milliseconds()<until) {
                pid_t result=waitpid(pid,&status,WNOHANG);
                if (result==pid) {exited=true;break;}
                if (result<0 && errno!=EINTR) break;
                usleep(20000);
            }
            if (!exited) stop_child(pid);
            if (!exited || !WIFEXITED(status) || WEXITSTATUS(status)) {error="Cannot load VPN TUN module"; return false;}
        }
        if (mkdir("/dev/net",0755)<0 && errno!=EEXIST) return false;
        if (lstat("/dev/net/tun",&st)<0 && errno==ENOENT && mknod("/dev/net/tun",S_IFCHR|0600,makedev(10,200))<0) return false;
        if (lstat("/dev/net/tun",&st)<0 || !S_ISCHR(st.st_mode) || st.st_rdev!=makedev(10,200)) {
            error="VPN TUN device is unavailable"; return false;
        }
        return true;
#else
        error="VPN is unavailable on this host"; return false;
#endif
    }
private:
    static std::string quoted(const std::string &text) {
        std::string out="\"";
        for (unsigned char c : text) {
            if (c=='"' || c=='\\') out+='\\';
            if (c>=32 && c<127) out+=char(c); else if (c=='\n' || c=='\r') out+=' ';
        }
        return out+'"';
    }
    static std::string member(const std::string &object, const char *wanted) {
        gcu_json j{object.data(),object.data()+object.size()};
        if (!gcu_json_take(&j,'{')) return {};
        do {
            char key[128];
            if (!gcu_json_string(&j,key,sizeof(key)) || !gcu_json_take(&j,':')) return {};
            const char *begin=j.p;
            if (!gcu_json_value(&j,0)) return {};
            if (!strcmp(key,wanted)) return std::string(begin,size_t(j.p-begin));
        } while(gcu_json_take(&j,','));
        return {};
    }
    static std::string string_value(const std::string &value) {
        gcu_json j{value.data(),value.data()+value.size()}; char text[512];
        return gcu_json_string(&j,text,sizeof(text))?text:std::string();
    }
    void describe(const ca_vpn_config &desired, const std::string &info, const std::string &networks) {
        std::string node=string_value(member(info,"address"));
        if (node.size()!=10 || node.find_first_not_of("0123456789abcdef")!=std::string::npos) node.clear();
        std::string online=member(info,"online");
        auto truth=online.find_first_not_of(" \r\n\t");
        bool connected=truth!=std::string::npos && online.substr(truth)=="true";
        std::string network_status="REQUESTING_CONFIGURATION",addresses;
        gcu_json j{networks.data(),networks.data()+networks.size()};
        if (gcu_json_take(&j,'[')) do {
            const char *begin=j.p;
            if (!gcu_json_value(&j,0)) break;
            std::string network(begin,size_t(j.p-begin));
            if (string_value(member(network,"id"))!=desired.network_id) continue;
            network_status=string_value(member(network,"status"));
            std::string list=member(network,"assignedAddresses");
            gcu_json a{list.data(),list.data()+list.size()};
            if (gcu_json_take(&a,'[')) do {
                char address[128]; if (!gcu_json_string(&a,address,sizeof(address))) break;
                if (!addresses.empty()) addresses+=", ";
                addresses+=address;
            } while(gcu_json_take(&a,','));
        } while(gcu_json_take(&j,','));
        std::string message=!connected?"Waiting for Internet connection":
            network_status=="OK"?"Connected":
            network_status=="ACCESS_DENIED"?"Authorize this node in ZeroTier Central":
            network_status=="REQUESTING_CONFIGURATION"?"Joining network":network_status;
        status(true,desired.network_id,message,node.empty()?"":"Node ID: "+node,
            addresses.empty()?"":"Addresses: "+addresses);
    }
    void status(bool enabled, const char *id, const std::string &message, const std::string &node = "", const std::string &networks = "") {
        write_file(run+"/vpn-status.json", "{\"enabled\":"+std::string(enabled?"true":"false")+
            ",\"running\":"+(child>0?"true":"false")+",\"network_id\":"+quoted(id)+
            ",\"message\":"+quoted(message)+",\"node\":"+quoted(node)+",\"networks\":"+quoted(networks)+"}\n");
    }
public:
    static bool parse(const char *text, ca_vpn_config &vpn) {
        ca_config parsed{};
        std::string section;
        auto trim=[](std::string value) {
            size_t a=value.find_first_not_of(" \t\r\n"), b=value.find_last_not_of(" \t\r\n");
            return a==std::string::npos?std::string():value.substr(a,b-a+1);
        };
        while (*text) {
            const char *end=strchr(text,'\n'); if (!end) end=text+strlen(text);
            std::string line=trim(std::string(text,size_t(end-text)));
            text=*end?end+1:end;
            if (line.size()>=512) return false;
            if (line.empty() || line[0]=='#' || line[0]==';') continue;
            if (line[0]=='[') {
                if (line.back()!=']') return false;
                section=trim(line.substr(1,line.size()-2));
                if (section.empty() || section.size()>=64) return false;
                continue;
            }
            size_t equals=line.find('='); if (equals==std::string::npos) return false;
            if (section!="network") continue;
            std::string key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));
            if (key!="vpn_enabled" && key!="vpn_provider" && key!="vpn_network_id") continue;
            if (value.size()>=2 && value.front()=='"' && value.back()=='"') value=trim(value.substr(1,value.size()-2));
            const auto *field=APC_Config::find("network",key.c_str());
            if (!field || !APC_Config::assign(parsed,*field,value.c_str())) return false;
        }
        vpn=parsed.vpn;
        for (char &c:vpn.network_id) c=char(tolower((unsigned char)c));
        return !vpn.enabled || vpn.network_id[0];
    }
    virtual ~APC_VPN() = default;
    APC_VPN(const char *runtime_path, const char *directory_path, Reader read_settings, const Resources *payload=nullptr):
        base(directory_path),run(runtime_path),runtime(payload?run+"/vpn":base),resources(payload),reader(read_settings) {}
    void supervise(pid_t parent, int inherited_lock=-1) {
        stopping=0;
        signal(SIGTERM,stop_signal); signal(SIGINT,stop_signal); signal(SIGCHLD,SIG_DFL);
        struct rlimit core={0,0}; setrlimit(RLIMIT_CORE,&core); umask(0077);
        int lock=inherited_lock>=0?inherited_lock:open((run+"/vpn.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if (lock<0) return;
        // A preceding web instance gets time to cleanly stop its child first.
        while (flock(lock,LOCK_EX|LOCK_NB)<0) {
            if (errno!=EWOULDBLOCK || stopping || getppid()!=parent) {close(lock); return;}
            usleep(100000);
        }
        ca_vpn_config active{}, requested{}; unsigned counter=0; bool joined=false;
        unsigned retry_delay=10000;
        std::string start_error;
        uint64_t retry=0;
        while (!stopping && getppid()==parent) {
            ca_vpn_config desired{};
            bool valid=reader(desired);
            if (valid && (requested.enabled!=desired.enabled || requested.provider!=desired.provider ||
                          strcmp(requested.network_id,desired.network_id))) {
                requested=desired; retry=0; retry_delay=10000;
            }
            int exit_status=0;
            if (child>0 && waitpid(child,&exit_status,WNOHANG)==child) {
                child=-1; joined=false; retry=milliseconds()+5000;
                start_error="ZeroTier client exited; restarting";
                status(true,desired.network_id,start_error);
            }
            if (!valid || !desired.enabled || desired.provider!=CA_VPN_ZEROTIER ||
                strcmp(active.network_id,desired.network_id)) {
                stop_child(child); joined=false;
                if (!valid || !desired.enabled) cleanup_runtime();
            }
            if (!valid) status(false,"","Invalid VPN settings; disconnected");
            else if (!desired.enabled) status(false,desired.network_id,"Disabled");
            else if (child<0 && milliseconds()>=retry) {
                std::string error;
                if (!directory(base) || !prepare_resources()) error="Cannot prepare VPN runtime";
                else if (access((runtime+"/zerotier-one").c_str(),X_OK)<0) error="VPN client is not installed";
                else if (!directory(base+"/state") || !directory(base+"/state/networks.d")) error="Cannot prepare private VPN state";
                else if (!tun_ready(error) && error.empty()) error="Cannot prepare VPN TUN device";
                if (error.empty()) {
                    std::string previous=read_file(base+"/state/network-id",16);
                    if (!previous.empty() && previous!=desired.network_id && apcam_vpn_network_id_valid(previous.c_str())) {
                        unlink((base+"/state/networks.d/"+previous+".conf").c_str());
                        unlink((base+"/state/networks.d/"+previous+".local.conf").c_str());
                    }
                    bool prepared=write_file(base+"/state/networks.d/"+desired.network_id+".local.conf",
                        "allowManaged=1\nallowGlobal=0\nallowDefault=0\nallowDNS=0\n") &&
                        write_file(base+"/state/network-id",desired.network_id);
                    if (!prepared) error="Cannot save VPN membership settings";
                    else {
                        pid_t owner=getpid();
                        child=fork();
                        if (!child) {
                            child_lifetime(owner);
                            setenv("LD_LIBRARY_PATH","/lib:/usr/lib",1);
                            int fd=open((run+"/zerotier.log").c_str(),O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW,0600);
                            if (fd<0) _exit(126);
                            dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);
                            execl((runtime+"/zerotier-one").c_str(),"zerotier-one",(base+"/state").c_str(),(char *)nullptr);
                            _exit(127);
                        }
                        if (child<0) error="Cannot start VPN client";
                        active=desired; counter=0;
                        if (child>0) retry_delay=10000;
                    }
                }
                if (!error.empty()) {
                    start_error=error;
                    retry=milliseconds()+retry_delay;
                    retry_delay=std::min(retry_delay*2U,300000U);
                }
                status(true,desired.network_id,error.empty()?"Starting ZeroTier":error);
            } else if (child<0) status(true,desired.network_id,start_error);
            if (child>0 && counter++%3==0) {
                std::string output,info,networks;
                if (!joined) joined=command("join",desired.network_id,output);
                command("info",nullptr,info); command("listnetworks",nullptr,networks);
                describe(desired,info,networks);
                struct stat st{};
                if (stat((run+"/zerotier.log").c_str(),&st)==0 && st.st_size>1024*1024)
                    if (truncate((run+"/zerotier.log").c_str(),0)<0) perror("VPN log trim");
            }
            usleep(1000000);
        }
        stop_child(child); cleanup_runtime();
        status(false,active.network_id,"VPN service stopped"); close(lock);
    }
};
