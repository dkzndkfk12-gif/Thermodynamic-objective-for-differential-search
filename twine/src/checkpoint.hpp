#pragma once
#include "archive.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace twine {
namespace fs=std::filesystem;
inline constexpr char checkpoint_format[]="twine-family-gzip-v1";
inline constexpr char checkpoint_file[]="family_results.jsonl.gz";
inline constexpr char checkpoint_pending[]=".twine-family-results.pending";

inline void storage_error(const char* operation){
    throw std::system_error(errno,std::generic_category(),operation);
}

inline void sync_directory(const fs::path& path){
    int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)storage_error("open result directory");
    int rc=::fsync(fd), saved=errno;
    ::close(fd);
    if(rc<0)throw std::system_error(saved,std::generic_category(),"sync result directory");
}

                                                                                
inline void discard_pending_checkpoint(const fs::path& directory){
    const auto pending=directory/checkpoint_pending;
    const auto type=fs::symlink_status(pending);
    if(!fs::exists(type))return;
    require(fs::is_regular_file(type),"unexpected file type at pending TWINE checkpoint");
    fs::remove(pending);
}

class PendingCheckpoint {
    fs::path directory, scratch;
    int fd=-1;
    bool named=false;
public:
    explicit PendingCheckpoint(const fs::path& dir):directory(dir),scratch(dir/checkpoint_pending){
        fd=::open(directory.c_str(),O_TMPFILE|O_RDWR|O_CLOEXEC,0666);
        if(fd>=0)return;
        if(errno!=EOPNOTSUPP&&errno!=EINVAL&&errno!=EISDIR&&errno!=ENOSYS)
            storage_error("create anonymous TWINE checkpoint");
        fd=::open(scratch.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0666);
        if(fd<0)storage_error("create pending TWINE checkpoint");
        named=true;
    }
    PendingCheckpoint(const PendingCheckpoint&)=delete;
    PendingCheckpoint& operator=(const PendingCheckpoint&)=delete;
    ~PendingCheckpoint(){
        if(fd>=0)::close(fd);
        if(named)::unlink(scratch.c_str());
    }
    gzFile stream()const{
        int copy=::dup(fd);
        if(copy<0)storage_error("duplicate checkpoint descriptor");
        gzFile out=gzdopen(copy,"wb1");
        if(!out){::close(copy);throw std::runtime_error("open gzip checkpoint stream failed");}
        return out;
    }
    uintmax_t commit(){
        struct stat info{};
        if(::fsync(fd)<0||::fstat(fd,&info)<0)storage_error("sync TWINE checkpoint");
        if(!named){
            auto link="/proc/self/fd/"+std::to_string(fd);
            if(::linkat(AT_FDCWD,link.c_str(),AT_FDCWD,scratch.c_str(),AT_SYMLINK_FOLLOW)<0)
                storage_error("link TWINE checkpoint");
            named=true;
        }
        fs::rename(scratch,directory/checkpoint_file);
        named=false;
        sync_directory(directory);
        return uintmax_t(info.st_size);
    }
};

struct GzipCloser { void operator()(gzFile f)const{gzclose(f);} };
using GzipHandle=std::unique_ptr<gzFile_s,GzipCloser>;

template<class Write> uintmax_t write_checkpoint(const fs::path& directory,Write write){
    PendingCheckpoint pending(directory);
    GzipHandle out(pending.stream());
    require(gzbuffer(out.get(),65536)==0,"set gzip checkpoint buffer failed");
    write([&](const J& row){
        auto line=row.dump();
        line.push_back('\n');
        if(gzfwrite(line.data(),1,line.size(),out.get())!=line.size())
            throw std::runtime_error(std::string("write TWINE checkpoint: ")+gzerror(out.get(),nullptr));
    });
    require(gzclose(out.release())==Z_OK,"finish gzip checkpoint failed");
    return pending.commit();
}

template<class Read> void read_checkpoint(const fs::path& path,Read read){
    GzipHandle input(gzopen(path.c_str(),"rb"));
    require(input!=nullptr,"missing TWINE checkpoint");
    require(!gzdirect(input.get()),"expected a gzip TWINE checkpoint");
    std::array<char,65536> buffer;
    std::string line;
    int size;
    while((size=gzread(input.get(),buffer.data(),buffer.size()))>0){
        const char* begin=buffer.data();
        const char* end=begin+size;
        while(begin<end){
            auto newline=static_cast<const char*>(std::memchr(begin,'\n',size_t(end-begin)));
            line.append(begin,newline?newline:end);
            if(!newline)break;
            read(J::parse(line));
            line.clear();
            begin=newline+1;
        }
    }
    int code=Z_OK;
    const char* message=gzerror(input.get(),&code);
    if(size<0||(code!=Z_OK&&code!=Z_STREAM_END))
        throw std::runtime_error(std::string("invalid gzip TWINE checkpoint: ")+message);
    require(line.empty(),"unfinished TWINE checkpoint row");
    require(gzclose(input.release())==Z_OK,"finish gzip checkpoint read failed");
}

inline void check_header(const J& header,int rounds,uint64_t seed){
    require(header.at("clean_stop").get<bool>(),"checkpoint was not saved after a clean stop");
    require(header.at("seed")==seed,"resume seed differs from checkpoint");
    require(header.at("schema")=="twine-mass-v1"&&header.at("rounds")==rounds,"incompatible checkpoint");
}

inline J restore_checkpoint(Archive& archive,const fs::path& directory,uint64_t seed){
    J header;
    if(fs::exists(directory/checkpoint_file)){
        size_t count=0;
        read_checkpoint(directory/checkpoint_file,[&](const J& row){
            if(header.is_null()){
                check_header(row,archive.rounds,seed);
                require(row.at("checkpoint_format")==checkpoint_format,"unsupported compact TWINE checkpoint format");
                header=row;
            }else{
                archive.restore_record(row,true);
                ++count;
            }
        });
        require(!header.is_null()&&count==header.at("family_count").get<size_t>(),"TWINE checkpoint family count mismatch");
        auto status=archive.status();
        require(status.at("endpoints")==header.at("endpoint_count")&&status.at("complete_families")==header.at("complete_families"),
                "TWINE checkpoint endpoint or completion count mismatch");
        const auto& best=header.at("best");
        require(best.is_null()?status.at("best").is_null():status.at("best").at("mass")==best.at("mass"),
                "TWINE checkpoint best bound mismatch");
    }else{
        std::ifstream in(directory/"state.json");
        require(bool(in),"--resume requires a saved TWINE checkpoint");
        in>>header;
        check_header(header,archive.rounds,seed);
        require(!header.contains("checkpoint_format"),"missing compact TWINE checkpoint table");
        if(header.contains("families_file")){
            auto name=fs::path(header.at("families_file").get<std::string>());
            require(name==name.filename()&&!name.empty()&&name!="."&&name!="..","checkpoint filename must be local");
            std::ifstream records(directory/name);
            require(bool(records),"missing legacy TWINE checkpoint table");
            size_t count=0;
            for(std::string line;std::getline(records,line);++count)archive.restore_record(J::parse(line),false);
            require(!records.bad()&&count==header.at("family_count").get<size_t>(),"truncated legacy TWINE checkpoint");
        }else archive.restore(header);
    }
    if(header.contains("guidance_state"))archive.restore_guidance(header.at("guidance_state"));
    return header;
}
}                   
