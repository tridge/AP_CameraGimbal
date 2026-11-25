/* Direct, bounded Google Drive transfer worker. HTTPS and filesystem work stay
 * outside the camera control process. No shell, Python or rclone is required.
 * Wire protocol: developers.google.com/drive/api/guides/manage-uploads
 */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "native_http.h"
#include "apcam/sha256.h"
#include "apcam/upload_paths.h"
#include <mbedtls/md5.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <ctime>
#include <fstream>
#include <sstream>

namespace cloud {
static_assert(sizeof(off_t)>=8 && sizeof(curl_off_t)>=8,"Uploads require 64-bit file offsets");
struct FD {
    int value;
    explicit FD(int fd):value(fd) { if(fd<0) throw error("Cannot open upload file or directory"); }
    ~FD() { close(value); }
    FD(const FD &)=delete; FD &operator=(const FD &)=delete;
};
static std::string read_fd(int fd,size_t maximum) {
    std::string result; char buffer[4096];
    for(;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if(n<0 && errno==EINTR) continue;
        if(n<0) throw error("Cannot read upload settings or state");
        if(!n) return result;
        if(size_t(n)>maximum-result.size()) throw error("Upload settings or state exceed size limit");
        result.append(buffer,size_t(n));
    }
}
static std::string private_file(const std::string &path,size_t limit) {
    FD file(open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)); struct stat info{};
    if(fstat(file.value,&info) || !S_ISREG(info.st_mode) || (info.st_mode&077) || info.st_uid!=geteuid())
        throw error("Upload credentials/state must be a private regular file owned by this user");
    return read_fd(file.value,limit);
}
static std::string trim(const std::string &value) {
    auto a=value.find_first_not_of(" \t\r\n"), b=value.find_last_not_of(" \t\r\n");
    return a==std::string::npos ? "" : value.substr(a,b-a+1);
}
static Fields credentials(const std::string &path,const std::string &remote) {
    if(access(path.c_str(),F_OK) && errno==ENOENT) throw error("No cloud credentials saved; enter them on the Cloud Upload tab");
    std::istringstream input(private_file(path,65536));
    Fields result;std::string line;bool active=false,seen=false;
    while(std::getline(input,line)) {
        line=trim(line); if(line.empty() || line[0]=='#' || line[0]==';') continue;
        if(line[0]=='[') {
            active=line=="["+remote+"]";
            if(active && seen) throw error("Duplicate remote credentials section");
            seen|=active;continue;
        }
        if(!active) continue;
        auto at=line.find('=');if(at==std::string::npos) throw error("Invalid upload credentials line");
        std::string key=trim(line.substr(0,at)), value=trim(line.substr(at+1));
        if(!result.emplace(key,value).second) throw error("Duplicate upload credential");
    }
    if(result["type"]!="drive") throw error("The native worker currently supports Google Drive remotes only");
    if(!result["team_drive"].empty() || !result["service_account_file"].empty() || !result["service_account_credentials"].empty())
        throw error("Native uploads currently require a user My Drive account");
    return result;
}
static bool identifier(const std::string &id) {
    return !id.empty() && id.size()<=256 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos;
}
static bool unchanged(const struct stat &a,const struct stat &b) {
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
        a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
static std::string md5(int fd) {
    mbedtls_md5_context context;mbedtls_md5_init(&context);
    if(mbedtls_md5_starts(&context)) { mbedtls_md5_free(&context);throw error("Cannot initialize file checksum"); }
    char data[16384];off_t at=0;
    for(;;) {
        ssize_t n=pread(fd,data,sizeof(data),at);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { mbedtls_md5_free(&context);throw error("Cannot checksum upload file"); }
        if(!n) break;
        if(mbedtls_md5_update(&context,reinterpret_cast<const unsigned char *>(data),size_t(n))) {
            mbedtls_md5_free(&context);throw error("Cannot calculate file checksum");
        }
        at+=n;
    }
    unsigned char digest[16];int status=mbedtls_md5_finish(&context,digest);mbedtls_md5_free(&context);
    if(status) throw error("Cannot finish file checksum");
    char hex[33];for(unsigned i=0;i<16;i++) snprintf(hex+2*i,3,"%02x",digest[i]);return hex;
}
// First string value of a key in an error reply, printable ASCII only.
static std::string reply_text(const std::string &body,const char *key) {
    size_t at=body.find(std::string("\"")+key+"\"");
    if(at==std::string::npos) return {};
    at=body.find_first_not_of(" \t\r\n",at+strlen(key)+2);
    if(at==std::string::npos || body[at]!=':') return {};
    at=body.find_first_not_of(" \t\r\n",at+1);
    if(at==std::string::npos || body[at]!='"') return {};
    std::string result;
    for(at++;at<body.size() && body[at]!='"' && result.size()<96;at++)
        if(body[at]>=' ' && body[at]<='~' && body[at]!='\\') result+=body[at];
    return result;
}
static void require(const Response &response,std::initializer_list<long> allowed) {
    if(std::find(allowed.begin(),allowed.end(),response.status)!=allowed.end()) return;
    std::string message="Cloud service returned HTTP "+std::to_string(response.status);
    // OAuth replies carry error_description, Drive replies carry message.
    std::string detail=reply_text(response.body,"error_description");
    if(detail.empty()) detail=reply_text(response.body,"message");
    if(detail.empty()) detail=reply_text(response.body,"error");
    throw std::runtime_error(detail.empty() ? message : message+": "+detail);
}
static std::string query_literal(const std::string &value) {
    std::string result="'";
    for(char c:value) { if(c=='\'' || c=='\\') result+='\\';result+=c; }
    return result+'\'';
}

class Drive {
    HTTP http;
    Fields config;
    std::string base="https://www.googleapis.com",token_url="https://oauth2.googleapis.com/token";
    std::string access;
    time_t expires=0;
    void authorize() {
        if(!access.empty() && time(nullptr)<expires) return;
        auto token=fields(config["token"]);
        std::string refresh=text(token,"refresh_token");
        if(config["client_id"].empty() || config["client_secret"].empty() || refresh.empty())
            throw error("Google Drive needs client ID, client secret and an offline refresh token");
        const std::string form="grant_type=refresh_token&client_id="+escape(config["client_id"])+
            "&client_secret="+escape(config["client_secret"])+"&refresh_token="+escape(refresh);
        auto response=http.request("POST",token_url,{"Content-Type: application/x-www-form-urlencoded"},form);
        require(response,{200});auto values=fields(response.body);
        access=text(values,"access_token");
        if(access.empty() || access.size()>8192 || !safe_header(access)) throw error("Invalid OAuth access token");
        auto duration=values.find("expires_in");
        uint64_t seconds=duration==values.end() ? 300 : integer(duration->second);
        expires=time(nullptr)+time_t(std::max<uint64_t>(1,std::min<uint64_t>(seconds,3600)>30 ? std::min<uint64_t>(seconds,3600)-30 : 1));
    }
public:
    Drive(Fields settings,std::string ca,uint64_t bandwidth,std::string test_base={}):http(std::move(ca),bandwidth),config(std::move(settings)) {
#ifdef APCAM_UPLOAD_TESTING
        if(!test_base.empty()) {base=test_base;token_url=base+"/token";}
#else
        if(!test_base.empty()) throw error("Test endpoints are disabled in production");
#endif
    }
    bool session_url(const std::string &url) const {
        return url.size()<8192 && url.compare(0,base.size()+1,base+"/")==0 && safe_header(url);
    }
    Response call(const char *method,const std::string &url,std::vector<std::string> headers={},
                  const std::string &body={},int fd=-1,uint64_t offset=0,uint64_t length=0) {
        if(!session_url(url)) throw error("Cloud upload URL is outside the trusted service");
        authorize();headers.push_back("Authorization: Bearer "+access);
        auto out=http.request(method,url,headers,body,fd,offset,length);
        if(out.status==401) {
            access.clear();authorize();headers.back()="Authorization: Bearer "+access;
            out=http.request(method,url,headers,body,fd,offset,length);
        }
        return out;
    }
    Fields find(const std::string &parent,const std::string &name,bool folder) {
        if(!identifier(parent)) throw error("Invalid Google Drive parent identifier");
        std::string q=query_literal(parent)+" in parents and trashed = false and name = "+query_literal(name);
        if(folder) q+=" and mimeType = 'application/vnd.google-apps.folder'";
        auto response=call("GET",base+"/drive/v3/files?pageSize=2&fields=nextPageToken,files(id,size,md5Checksum,mimeType)&q="+escape(q));
        require(response,{200});auto value=fields(response.body);
        if(!text(value,"nextPageToken").empty()) throw error("Ambiguous Google Drive path");
        auto found=value.find("files");if(found==value.end()) throw error("Missing Google Drive file list");
        auto list=objects(found->second);
        if(list.size()>1) throw error("Ambiguous Google Drive path; duplicate file or folder names");
        if(list.empty()) return {};
        if(!identifier(text(list[0],"id"))) throw error("Invalid Google Drive file identifier");
        return list[0];
    }
    std::string folder(const std::string &parent,const std::string &name) {
        auto found=find(parent,name,true);if(!found.empty()) return text(found,"id");
        auto response=call("POST",base+"/drive/v3/files?fields=id",{"Content-Type: application/json"},
            "{\"name\":"+quote(name)+",\"mimeType\":\"application/vnd.google-apps.folder\",\"parents\":["+quote(parent)+"]}");
        require(response,{200,201});auto id=text(fields(response.body),"id");
        if(!identifier(id)) throw error("Invalid created Google Drive folder identifier");
        return id;
    }
    std::string root() const {
        auto found=config.find("root_folder_id");
        std::string id=found==config.end() || found->second.empty() ? "root" : found->second;
        if(!identifier(id)) throw error("Invalid configured Google Drive root folder");
        return id;
    }
    std::string create_session(const std::string &parent,const std::string &name,uint64_t size,const Fields &existing) {
        const std::string id=existing.empty()?"":text(existing,"id");
        std::string url=base+"/upload/drive/v3/files"+(id.empty()?"":"/"+id)+"?uploadType=resumable&fields=id,size,md5Checksum";
        std::string metadata="{\"name\":"+quote(name)+(id.empty()?",\"parents\":["+quote(parent)+"]":"")+"}";
        auto response=call(id.empty()?"POST":"PATCH",url,{"Content-Type: application/json","X-Upload-Content-Type: application/octet-stream","X-Upload-Content-Length: "+std::to_string(size)},metadata);
        require(response,{200,201});
        if(!session_url(response.location)) throw error("Missing or untrusted resumable upload session URL");
        return response.location;
    }
};

struct Progress {
    uint64_t bytes=0,total=0;unsigned files=0;
    void emit(uint64_t partial=0) const {
        fprintf(stderr,"{\"level\":\"notice\",\"stats\":{\"bytes\":%llu,\"totalBytes\":%llu,\"transfers\":%u,\"totalTransfers\":%u}}\n",
            static_cast<unsigned long long>(bytes+partial),static_cast<unsigned long long>(total),files,files+(partial?1U:0U));
        fflush(stderr);
    }
};
class Checkpoints {
    FD directory;
public:
    explicit Checkpoints(const std::string &path):directory(prepare(path)) {}
    static int prepare(const std::string &path) {
        if(mkdir(path.c_str(),0700) && errno!=EEXIST) throw error("Cannot create private upload state directory");
        int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0) return fd;
        struct stat info{};
        if(fstat(fd,&info) || info.st_uid!=geteuid() || (info.st_mode&077)) {
            close(fd);throw error("Upload state directory must be private and owned by this user");
        }
        // One worker per credential set, even if two camera instances share it.
        if(flock(fd,LOCK_EX|LOCK_NB)) {close(fd);throw error("Another cloud upload worker is active");}
        return fd;
    }
    std::string load(const std::string &key) {
        int fd=openat(directory.value,key.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0) {if(errno==ENOENT)return {};throw error("Cannot read resumable upload state");}
        FD file(fd);struct stat info{};
        if(fstat(fd,&info) || !S_ISREG(info.st_mode) || (info.st_mode&077) || info.st_uid!=geteuid())
            throw error("Invalid resumable upload state permissions");
        return read_fd(fd,8192);
    }
    void save(const std::string &key,const std::string &value) {
        DIR *raw=fdopendir(dup(directory.value));
        if(!raw) throw error("Cannot inspect upload state budget");
        std::unique_ptr<DIR,int(*)(DIR *)> entries(raw,closedir);
        rewinddir(raw);
        unsigned count=0;
        while(auto *entry=readdir(raw)) {
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            if(++count>=128) throw error("Too many incomplete upload sessions; remove obsolete upload state");
        }
        // Interrupted atomic writes are harmless and replaced on the next try.
        const std::string temporary=key+".tmp";
        FD file(openat(directory.value,temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600));
        size_t used=0;
        while(used<value.size()) {
            ssize_t n=write(file.value,value.data()+used,value.size()-used);
            if(n<0 && errno==EINTR) continue;
            if(n<=0) throw error("Cannot save resumable upload state");
            used+=size_t(n);
        }
        if(fsync(file.value) || renameat(directory.value,temporary.c_str(),directory.value,key.c_str()) || fsync(directory.value))
            throw error("Cannot commit resumable upload state");
    }
    void remove(const std::string &key) { if(unlinkat(directory.value,key.c_str(),0) && errno!=ENOENT) throw error("Cannot remove completed upload state"); }
};
static bool matches(const Fields &remote,uint64_t size,const std::string &hash) {
    if(remote.empty()) return false;
    return text(remote,"md5Checksum")==hash && integer(text(remote,"size"))==size;
}
static uint64_t uploaded(const Response &response,uint64_t size) {
    require(response,{308});
    if(response.range.empty()) return 0;
    if(response.range.compare(0,8,"bytes=0-")) throw error("Invalid resumable acknowledgement");
    uint64_t end=integer(response.range.substr(8));
    if(end>=size) throw error("Resumable acknowledgement exceeds local file");
    return end+1;
}

class Transfer {
    Drive drive;
    Checkpoints checkpoints;
    bool move;
    Progress progress;
    void file(int directory,const std::string &name,const std::string &relative,const std::string &parent) {
        int fd=openat(directory,name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0) {if(errno==ELOOP)return;throw error("Cannot open upload source file");}
        FD source(fd);struct stat before{},after{};
        if(fstat(fd,&before)) throw error("Cannot inspect upload source file");
        if(!S_ISREG(before.st_mode) || before.st_size<=0) return;
        if(time(nullptr)-before.st_mtime<2) {
            // A recording just closed may trigger this scan immediately. Wait
            // here, in the disposable worker, rather than postpone it a scan.
            timespec delay{2,0};while(nanosleep(&delay,&delay) && errno==EINTR) {}
            if(fstat(fd,&after) || !unchanged(before,after) || time(nullptr)-before.st_mtime<2) return;
        }
        const uint64_t size=uint64_t(before.st_size);
        std::string hash=md5(fd);
        if(fstat(fd,&after) || !unchanged(before,after)) throw error("Upload source changed during checksum");
        std::string key=apcam::sha256(parent+":"+relative+":"+std::to_string(size)+":"+hash);
        progress.total+=size;
        auto remote=drive.find(parent,name,false);
        if(!matches(remote,size,hash)) {
            std::string session=checkpoints.load(key);
            Response result;
            uint64_t offset=0;
            if(!session.empty()) {
                if(!drive.session_url(session)) throw error("Untrusted saved upload session");
                result=drive.call("PUT",session,{"Content-Length: 0","Content-Range: bytes */"+std::to_string(size)});
                if(result.status==200 || result.status==201) offset=size;
                else if(result.status==404 || result.status==410) session.clear();
                else offset=uploaded(result,size);
            }
            if(session.empty()) {
                session=drive.create_session(parent,name,size,remote);checkpoints.save(key,session);
            }
            while(offset<size) {
                if(fstat(fd,&after) || !unchanged(before,after)) throw error("Upload source changed; transfer cancelled");
                const uint64_t length=std::min<uint64_t>(256*1024,size-offset);
                result=drive.call("PUT",session,{"Content-Type: application/octet-stream","Expect:",
                    "Content-Range: bytes "+std::to_string(offset)+"-"+std::to_string(offset+length-1)+"/"+std::to_string(size)},"",fd,offset,length);
                uint64_t next=(result.status==200 || result.status==201)?size:uploaded(result,size);
                if(next<=offset || next>offset+length || ((result.status==200 || result.status==201) && offset+length!=size))
                    throw error("Invalid resumable upload progress");
                offset=next;progress.emit(offset);
            }
            // Independently query committed metadata, even after a recovered
            // final PUT whose response may have been lost during interruption.
            remote=drive.find(parent,name,false);
            if(!matches(remote,size,hash)) {
                checkpoints.remove(key);
                throw error("Cloud size/checksum verification failed; local file retained");
            }
        }
        if(fstat(fd,&after) || !unchanged(before,after)) throw error("Uploaded local file changed; local file retained");
        if(move) {
            if(flock(fd,LOCK_EX|LOCK_NB)) throw error("Local file is busy; retained after upload");
            struct stat path{};
            if(fstatat(directory,name.c_str(),&path,AT_SYMLINK_NOFOLLOW) || !unchanged(before,path))
                throw error("Local path changed; retained after upload");
            if(unlinkat(directory,name.c_str(),0)) throw error("Cannot remove verified local upload");
        }
        checkpoints.remove(key);progress.bytes+=size;progress.files++;progress.emit();
    }
    void walk(int fd,const std::string &parent,const std::string &relative,unsigned depth) {
        if(depth>32) throw error("Upload directory nesting exceeds limit");
        DIR *raw=fdopendir(dup(fd));if(!raw) throw error("Cannot list upload directory");
        std::unique_ptr<DIR,int(*)(DIR *)> directory(raw,closedir);
        for(;;) {
            errno=0;dirent *entry=readdir(raw);
            if(!entry) {if(errno)throw error("Cannot read upload directory");break;}
            std::string name=entry->d_name;
            if(name.empty() || name[0]=='.' || (name.size()>=4 && name.compare(name.size()-4,4,".tmp")==0) ||
                (name.size()>=5 && name.compare(name.size()-5,5,".part")==0)) continue;
            if(relative.size()+name.size()>4096) throw error("Upload path exceeds limit");
            struct stat info{};
            if(fstatat(fd,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)) throw error("Cannot inspect upload directory entry");
            if(S_ISDIR(info.st_mode)) {
                FD child(openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
                walk(child.value,drive.folder(parent,name),relative+"/"+name,depth+1);
            } else if(S_ISREG(info.st_mode)) file(fd,name,relative+"/"+name,parent);
        }
    }
public:
    Transfer(Fields config,const std::string &settings,const std::string &ca,uint64_t bandwidth,bool remove,const std::string &test):
        drive(std::move(config),ca,bandwidth,test),checkpoints(settings+".state"),move(remove) {}
    void run(const std::string &source,const std::string &destination) {
        std::string parent=drive.root();unsigned depth=0;size_t at=0;
        while(at<destination.size()) {
            size_t end=destination.find('/',at);if(end==std::string::npos)end=destination.size();
            const std::string part=destination.substr(at,end-at);at=end+1;
            if(part.empty()) continue;
            if(part=="." || part==".." || ++depth>32) throw error("Invalid remote folder path");
            parent=drive.folder(parent,part);
        }
        FD root(open(source.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        walk(root.value,parent,"",0);progress.emit();
    }
};
} // namespace cloud

int main(int argc,char **argv) {
    using namespace cloud;
    try {
        if(argc==2 && !strcmp(argv[1],"--version")) {puts("AP_CameraGimbal native Drive uploader 1");return 0;}
        if(argc<6 || (strcmp(argv[1],"copy") && strcmp(argv[1],"move"))) throw error("Expected copy|move SOURCE REMOTE:FOLDER --config FILE");
        std::string source=argv[2],destination=argv[3],settings,ca,test;
        uint64_t bandwidth=0;
        for(int i=4;i<argc;i++) {
            std::string option=argv[i];
            if(option=="--use-json-log") continue;
            if(i+1>=argc) throw error("Missing uploader option value");
            std::string value=argv[++i];
            if(option=="--config") settings=value;
            else if(option=="--bwlimit") {
                if(value.empty() || value.back()!='k') throw error("Expected KiB/s bandwidth");
                uint64_t kib=integer(value.substr(0,value.size()-1));
                if(kib>1000000) throw error("Bandwidth exceeds supported limit");
                bandwidth=kib*1024;
            }
            else if(option=="--test-base") test=value;
            else if(option!="--transfers" && option!="--checkers" && option!="--stats" && option!="--stats-log-level" && option!="--exclude")
                throw error("Unknown native upload option");
        }
        auto colon=destination.find(':');
        if(settings.empty() || colon==std::string::npos || !colon) throw error("Missing Drive settings or remote name");
        const char *trust=getenv("CAMERA_APP_UPLOAD_CA_FILE");
        if(trust && *trust) ca=trust;
        else {
            char executable[4096],trust_file[4096];
            if(!apcam_upload_executable(executable,sizeof(executable)) ||
                !apcam_upload_sibling(trust_file,sizeof(trust_file),executable,"upload-ca.pem"))
                throw error("Cannot locate upload CA trust store");
            ca=trust_file;
        }
        if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) throw error("Cannot initialize HTTPS library");
        {
            Transfer transfer(credentials(settings,destination.substr(0,colon)),settings,ca,bandwidth,!strcmp(argv[1],"move"),test);
            transfer.run(source,destination.substr(colon+1));
        }
        curl_global_cleanup();return 0;
    } catch(const std::exception &exception) {
        std::string message=quote(exception.what());
        fprintf(stderr,"{\"level\":\"error\",\"msg\":%s}\n",message.c_str());return 1;
    }
}
