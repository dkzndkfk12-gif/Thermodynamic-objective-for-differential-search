#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef __linux__
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace mass_messages {
inline std::uint64_t setting(const char* name,std::uint64_t fallback) {
    const char* s=std::getenv(name);if(!s)return fallback;
    try {std::size_t n=0;auto v=std::stoull(s,&n);return n==std::strlen(s)?v:fallback;}
    catch(...) {return fallback;}
}
inline std::uint64_t hash(const std::string& s) {
    std::uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;
}
inline constexpr std::uint64_t magic=0x54484d4d53473033ULL;
inline constexpr std::size_t slot_bytes=128*1024, header_bytes=4096;
struct Header {std::uint64_t tag,slots,sequence;};
struct Entry {std::uint64_t valid,hash,sequence,key_bytes,value_bytes;};
class Cache {
    int fd=-1;unsigned char* base=nullptr;std::size_t bytes=0;
  public:
    std::uint64_t hits=0,misses=0,stores=0;
    Cache() {
#ifdef __linux__
        auto path=std::getenv("THERMO_MESSAGE_CACHE_PATH");if(!path)return;
        fd=::open(path,O_RDWR|O_CLOEXEC);if(fd<0)return;
        struct stat st{};if(fstat(fd,&st)||st.st_size<off_t(header_bytes+4*slot_bytes)){::close(fd);fd=-1;return;}
        bytes=std::size_t(st.st_size);auto p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(p==MAP_FAILED){::close(fd);fd=-1;return;}base=static_cast<unsigned char*>(p);
        auto h=reinterpret_cast<Header*>(base);
        if(h->tag!=magic||h->slots!=(bytes-header_bytes)/slot_bytes){munmap(base,bytes);::close(fd);base=nullptr;fd=-1;}
#endif
    }
    Cache(const Cache&)=delete;Cache& operator=(const Cache&)=delete;
    ~Cache(){
#ifdef __linux__
        if(base)munmap(base,bytes);
        if(fd>=0)::close(fd);
#endif
    }
    bool get(const std::string& key,std::string& value) {
#ifdef __linux__
        if(base && !flock(fd,LOCK_SH|LOCK_NB)) {
            auto h=reinterpret_cast<Header*>(base);auto hv=hash(key);
            const auto group=(hv%(h->slots/4))*4;
            bool found=false;
            for(unsigned j=0;j<4;++j) {
                auto e=reinterpret_cast<Entry*>(base+header_bytes+(group+j)*slot_bytes);
                if(e->valid!=magic||e->hash!=hv||e->key_bytes!=key.size()||
                   e->key_bytes>slot_bytes-sizeof(Entry)||e->value_bytes>slot_bytes-sizeof(Entry)-e->key_bytes)continue;
                auto p=reinterpret_cast<const char*>(e+1);
                if(!std::memcmp(p,key.data(),key.size())){value.assign(p+e->key_bytes,e->value_bytes);found=true;break;}
            }
            flock(fd,LOCK_UN);if(found){++hits;return true;}
        }
#endif
        ++misses;return false;
    }
    void put(const std::string& key,const std::string& value) {
#ifdef __linux__
        if(!base||key.size()+value.size()>slot_bytes-sizeof(Entry)||flock(fd,LOCK_EX|LOCK_NB))return;
        auto h=reinterpret_cast<Header*>(base);auto hv=hash(key);const auto group=(hv%(h->slots/4))*4;
        Entry* chosen=nullptr;
        for(unsigned j=0;j<4;++j){auto e=reinterpret_cast<Entry*>(base+header_bytes+(group+j)*slot_bytes);
            if(e->valid!=magic){chosen=e;break;}
            if(!chosen||e->sequence<chosen->sequence)chosen=e;
            if(e->hash==hv&&e->key_bytes==key.size()&&e->key_bytes<=slot_bytes-sizeof(Entry)&&
               !std::memcmp(e+1,key.data(),key.size())){chosen=e;break;}}
        chosen->valid=0;__sync_synchronize();
        chosen->hash=hv;chosen->sequence=++h->sequence;chosen->key_bytes=key.size();chosen->value_bytes=value.size();
        auto p=reinterpret_cast<char*>(chosen+1);std::memcpy(p,key.data(),key.size());std::memcpy(p+key.size(),value.data(),value.size());
        __sync_synchronize();chosen->valid=magic;flock(fd,LOCK_UN);++stores;
#else
        (void)key;(void)value;
#endif
    }
};
inline Cache& cache(){static Cache c;return c;}

class Arena {
    int fd=-1;
  public:
    std::size_t bytes=0;
    explicit Arena(std::uint64_t mib) {
#ifdef __linux__
        unsetenv("THERMO_MESSAGE_CACHE_PATH");if(!mib)return;
        auto slots=(mib*1024*1024/slot_bytes)/4*4;if(slots<4)return;
        fd=int(syscall(SYS_memfd_create,"thermo-sum-messages",1U));if(fd<0)return;
        bytes=header_bytes+slots*slot_bytes;
        if(ftruncate(fd,off_t(bytes))){::close(fd);fd=-1;bytes=0;return;}
        Header h{magic,slots,0};if(pwrite(fd,&h,sizeof(h),0)!=ssize_t(sizeof(h))){::close(fd);fd=-1;bytes=0;return;}
        auto path="/proc/"+std::to_string(getpid())+"/fd/"+std::to_string(fd);
        setenv("THERMO_MESSAGE_CACHE_PATH",path.c_str(),1);
#else
        (void)mib;
#endif
    }
    Arena(const Arena&)=delete;Arena& operator=(const Arena&)=delete;
    ~Arena(){
#ifdef __linux__
        if(fd>=0)::close(fd);
#endif
    }
};
}
