#include "../web/APC_VPN.h"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>

static std::string root;
static bool settings(ca_vpn_config &vpn) {
    std::ifstream file(root+"/settings");
    unsigned enabled=0; std::string id;
    if (!(file>>enabled>>id)) return false;
    vpn={};vpn.enabled=enabled;
    if (id=="-") id.clear();
    if (!apcam_vpn_network_id_valid(id.c_str())) return false;
    snprintf(vpn.network_id,sizeof(vpn.network_id),"%s",id.c_str());
    return !vpn.enabled || vpn.network_id[0];
}
struct FakeVPN : APC_VPN {
    FakeVPN():APC_VPN(root.c_str(),(root+"/vpn").c_str(),settings) {}
    bool tun_ready(std::string &) override {return true;}
};
static std::string read(const std::string &name) {
    std::ifstream file(root+name);return {std::istreambuf_iterator<char>(file),{}};
}
static void configure(const char *text) {
    std::ofstream file(root+"/settings.new");file<<text;file.close();
    assert(rename((root+"/settings.new").c_str(),(root+"/settings").c_str())==0);
}
static bool wait_for(std::function<bool()> condition, unsigned seconds=12) {
    for(unsigned i=0;i<seconds*20;i++) {if(condition())return true;usleep(50000);}return false;
}
static bool alive(int pid) {return pid>0 && kill(pid,0)==0;}
static void daemon_stop(int) {unlink((root+"/interface").c_str());_exit(0);}
int main(int argc,char **argv) {
    // This same executable is installed as the fake ZeroTier binary.
    if (strstr(argv[0],"zerotier")) {
        if (argc>1 && !strncmp(argv[1],"-D",2)) {
            std::string home=argv[1]+2;root=home.substr(0,home.size()-10);
            if (!strcmp(argv[3],"join")) {
                std::ofstream(home+"/networks.d/"+argv[4]+".conf")<<"membership";
                std::cout<<"200 join OK\n";
            } else if (!strcmp(argv[3],"info")) std::cout<<R"({"address":"0123456789","online":true})";
            else std::cout<<R"([{"id":"abfd31bd474e3f28","status":"OK","assignedAddresses":["10.147.19.134/24"]}])";
            return 0;
        }
        root=std::string(argv[1]);root.resize(root.size()-10);
        signal(SIGTERM,daemon_stop);
        std::ofstream(root+"/interface")<<getpid();
        std::ofstream(root+"/vpn/state/identity.secret",std::ios::app)<<"private-test-identity\n";
        while(true) pause();
    }
    assert(argc==2);root=argv[1];
    ca_vpn_config parsed{};
    assert(APC_VPN::parse("[network]\nvpn_enabled=false\n",parsed) && !parsed.enabled);
    assert(!APC_VPN::parse("[network]\nvpn_enabled=true\n",parsed));
    assert(!APC_VPN::parse("[network]\nvpn_provider=tailscale\n",parsed));
    assert(APC_VPN::parse("[network]\nvpn_enabled=true\nvpn_network_id=0123456789ABCDEF\n",parsed));
    assert(!strcmp(parsed.network_id,"0123456789abcdef"));
    assert(APC_VPN::parse("[network]\nvpn_enabled=true\nvpn_network_id=0123456789ABCDEF\nvpn_enabled=false\n",parsed) && !parsed.enabled);
    assert(!APC_VPN::parse(("[network]\nvpn_provider="+std::string(200,'x')).c_str(),parsed));
    assert(mkdir((root+"/vpn").c_str(),0700)==0);
    char executable[4096];ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);assert(n>0);executable[n]=0;
    assert(symlink(executable,(root+"/vpn/zerotier-one").c_str())==0);
    configure("0 -");
    pid_t parent=getpid(), worker=fork();assert(worker>=0);
    if(!worker) {FakeVPN service;service.supervise(parent);_exit(0);}
    assert(wait_for([]{return read("/vpn-status.json").find("Disabled")!=std::string::npos;}));
    assert(read("/interface").empty()); // Default off never starts a client.
    configure("1 abfd31bd474e3f28");
    assert(wait_for([]{return read("/vpn-status.json").find("Connected")!=std::string::npos;}));
    assert(!read("/interface").empty());
    assert(read("/vpn/state/networks.d/abfd31bd474e3f28.local.conf").find("allowDefault=0")!=std::string::npos);
    auto identity=read("/vpn/state/identity.secret");assert(!identity.empty());
    configure("0 abfd31bd474e3f28");
    assert(wait_for([]{return read("/vpn-status.json").find("Disabled")!=std::string::npos;}));
    assert(read("/interface").empty());assert(read("/vpn/state/identity.secret")==identity);
    configure("1 0123456789abcdef");
    assert(wait_for([]{return !read("/vpn/state/networks.d/0123456789abcdef.conf").empty();}));
    assert(read("/vpn/state/networks.d/abfd31bd474e3f28.conf").empty());
    int client=atoi(read("/interface").c_str());assert(alive(client));kill(client,SIGKILL);
    assert(wait_for([client]{return atoi(read("/interface").c_str())!=client;},20));
    configure("1 bad-network");
    assert(wait_for([]{return read("/vpn-status.json").find("Invalid VPN")!=std::string::npos;}));
    assert(read("/interface").empty());
    kill(worker,SIGTERM);int status;assert(waitpid(worker,&status,0)==worker);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    configure("1 abfd31bd474e3f28");
    pid_t owner=fork();assert(owner>=0);
    if (!owner) {
        pid_t web=getpid(), child_worker=fork();assert(child_worker>=0);
        if (!child_worker) {FakeVPN service;service.supervise(web);_exit(0);}
        while(true) pause();
    }
    assert(wait_for([]{return read("/vpn-status.json").find("Connected")!=std::string::npos;}));
    kill(owner,SIGKILL);assert(waitpid(owner,&status,0)==owner);
    assert(wait_for([]{return read("/interface").empty();}));
    puts("PASS VPN default off, join, disable, identity retention, network replacement, crash recovery and invalid settings");
}
