#pragma once
#include "apcam/json_reader.h"
#include <curl/curl.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace cloud {
using Fields = std::map<std::string, std::string>;
inline std::runtime_error error(const char *message) { return std::runtime_error(message); }
inline std::string quote(const std::string &value) {
    std::string out="\"";
    for (unsigned char c:value) {
        if (c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if (c<32) { char hex[7]; snprintf(hex,sizeof(hex),"\\u%04x",c); out+=hex; }
        else out+=char(c);
    }
    return out+'"';
}
inline Fields fields(const std::string &json) {
    gcu_json reader{json.data(),json.data()+json.size()};
    Fields result;
    if (!gcu_json_take(&reader,'{')) throw error("Invalid JSON object from cloud service");
    if (!gcu_json_take(&reader,'}')) for (;;) {
        char key[128];
        if (!gcu_json_string(&reader,key,sizeof(key)) || !gcu_json_take(&reader,':'))
            throw error("Invalid JSON field from cloud service");
        gcu_json_space(&reader); const char *start=reader.p;
        if (!gcu_json_value(&reader,0) || result.count(key)) throw error("Invalid JSON value from cloud service");
        result.emplace(key,std::string(start,reader.p));
        if (gcu_json_take(&reader,'}')) break;
        if (!gcu_json_take(&reader,',')) throw error("Invalid JSON separator from cloud service");
    }
    gcu_json_space(&reader);
    if (reader.p!=reader.end) throw error("Trailing JSON from cloud service");
    return result;
}
inline std::string text(const Fields &object,const char *key) {
    auto value=object.find(key); if(value==object.end()) return {};
    std::vector<char> buffer(value->second.size()+1);
    gcu_json reader{value->second.data(),value->second.data()+value->second.size()};
    if (!gcu_json_string(&reader,buffer.data(),buffer.size()) || reader.p!=reader.end)
        throw error("Expected an ASCII JSON string from cloud service");
    return buffer.data();
}
inline uint64_t integer(const std::string &value) {
    if(value.empty() || value.size()>20 || value.find_first_not_of("0123456789")!=std::string::npos)
        throw error("Invalid integer from cloud service");
    char *end; errno=0; unsigned long long n=strtoull(value.c_str(),&end,10);
    if(errno || *end) throw error("Cloud integer exceeds range");
    return n;
}
inline std::vector<Fields> objects(const std::string &json) {
    gcu_json reader{json.data(),json.data()+json.size()};
    std::vector<Fields> out;
    if(!gcu_json_take(&reader,'[')) throw error("Invalid cloud file list");
    if(!gcu_json_take(&reader,']')) for(;;) {
        if(out.size()>=2) throw error("Ambiguous cloud file names");
        gcu_json_space(&reader); const char *start=reader.p;
        if(!gcu_json_value(&reader,0)) throw error("Invalid cloud file list item");
        out.push_back(fields(std::string(start,reader.p)));
        if(gcu_json_take(&reader,']')) break;
        if(!gcu_json_take(&reader,',')) throw error("Invalid cloud file list separator");
    }
    gcu_json_space(&reader);
    if(reader.p!=reader.end) throw error("Trailing cloud file list data");
    return out;
}
inline std::string escape(const std::string &value) {
    char *encoded=curl_easy_escape(nullptr,value.data(),int(value.size()));
    if(!encoded) throw error("Cannot encode cloud request");
    std::string result(encoded); curl_free(encoded); return result;
}
inline bool safe_header(const std::string &value) {
    return value.find_first_of("\r\n")==std::string::npos;
}
struct Response {
    long status=0;
    std::string body,location,range;
};
struct FileBody { int fd; uint64_t offset,left; };
class HTTP {
    using Handle=std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>;
    Handle handle;
    std::string ca;
    uint64_t bandwidth;
    static size_t receive(char *data,size_t size,size_t count,void *opaque) noexcept {
        try {
        auto &out=*static_cast<Response *>(opaque);
        if(size && count>SIZE_MAX/size) return 0;
        size*=count;
        if(size>131072-out.body.size()) return 0;
        out.body.append(data,size); return size;
        } catch(...) {return 0;}
    }
    static size_t header(char *data,size_t size,size_t count,void *opaque) noexcept {
        try {
        if(size && count>SIZE_MAX/size) return 0;
        size*=count;
        if(size>8192) return 0;
        auto &out=*static_cast<Response *>(opaque);
        std::string line(data,size);
        auto colon=line.find(':');
        if(colon==std::string::npos) return size;
        std::string name=line.substr(0,colon);
        for(char &c:name) c=char(tolower(static_cast<unsigned char>(c)));
        std::string value=line.substr(colon+1);
        auto first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t\r\n");
        value=first==std::string::npos || last==std::string::npos ? "" : value.substr(first,last-first+1);
        if(name=="location") out.location=value;
        if(name=="range") out.range=value;
        return size;
        } catch(...) {return 0;}
    }
    static size_t read_file(char *data,size_t size,size_t count,void *opaque) {
        auto &source=*static_cast<FileBody *>(opaque);
        if(size && count>SIZE_MAX/size) return CURL_READFUNC_ABORT;
        size_t wanted=size_t(std::min<uint64_t>(size*count,source.left));
        if(!wanted) return 0;
        ssize_t n;
        do { n=pread(source.fd,data,wanted,off_t(source.offset)); } while(n<0 && errno==EINTR);
        if(n<=0) return CURL_READFUNC_ABORT;
        source.offset+=uint64_t(n);source.left-=uint64_t(n);return size_t(n);
    }
public:
    HTTP(std::string trust,uint64_t speed):handle(curl_easy_init(),curl_easy_cleanup),ca(std::move(trust)),bandwidth(speed) {
        if(!handle) throw error("Cannot initialize HTTPS");
    }
    Response request(const char *method,const std::string &url,const std::vector<std::string> &headers={},
                     const std::string &body={},int fd=-1,uint64_t offset=0,uint64_t length=0) {
        CURL *h=handle.get();
        // Reset request options but retain the bounded connection/TLS cache.
        // Videos need thousands of chunks; each must not repeat a handshake.
        curl_easy_reset(h);
        Response out; FileBody file{fd,offset,length};
        curl_slist *list=nullptr;
        for(const auto &item:headers) {
            if(!safe_header(item)) { curl_slist_free_all(list);throw error("Invalid cloud request header"); }
            auto *next=curl_slist_append(list,item.c_str());
            if(!next) { curl_slist_free_all(list);throw error("Cannot allocate HTTPS headers"); }
            list=next;
        }
        std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> owned(list,curl_slist_free_all);
        curl_easy_setopt(h,CURLOPT_URL,url.c_str());
        curl_easy_setopt(h,CURLOPT_MAXCONNECTS,2L);
        curl_easy_setopt(h,CURLOPT_PROTOCOLS_STR,"https");
        curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(h,CURLOPT_SSL_VERIFYPEER,1L);
        curl_easy_setopt(h,CURLOPT_SSL_VERIFYHOST,2L);
        curl_easy_setopt(h,CURLOPT_CAINFO,ca.c_str());
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,15L);
        curl_easy_setopt(h,CURLOPT_TIMEOUT,long(std::max<uint64_t>(120,bandwidth ? length/bandwidth+60 : 120)));
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,list);
        curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
        curl_easy_setopt(h,CURLOPT_WRITEDATA,&out);
        curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,header);
        curl_easy_setopt(h,CURLOPT_HEADERDATA,&out);
        curl_easy_setopt(h,CURLOPT_USERAGENT,"AP_CameraGimbal-upload/1");
        if(fd>=0) {
            curl_easy_setopt(h,CURLOPT_UPLOAD,1L);
            curl_easy_setopt(h,CURLOPT_READFUNCTION,read_file);
            curl_easy_setopt(h,CURLOPT_READDATA,&file);
            curl_easy_setopt(h,CURLOPT_INFILESIZE_LARGE,curl_off_t(length));
            curl_easy_setopt(h,CURLOPT_UPLOAD_BUFFERSIZE,16384L);
            if(bandwidth) curl_easy_setopt(h,CURLOPT_MAX_SEND_SPEED_LARGE,curl_off_t(bandwidth));
        } else if(strcmp(method,"GET")) {
            curl_easy_setopt(h,CURLOPT_POSTFIELDS,body.data());
            curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE_LARGE,curl_off_t(body.size()));
        }
        curl_easy_setopt(h,CURLOPT_CUSTOMREQUEST,method);
        CURLcode result=curl_easy_perform(h);
        if(result!=CURLE_OK) throw error(curl_easy_strerror(result));
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&out.status);
        return out;
    }
};
} // namespace cloud
