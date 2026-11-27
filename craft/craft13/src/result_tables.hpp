#pragma once
#include "checkpoint_messages.hpp"
#include "wall_budget.hpp"
#include <cerrno>
#include <system_error>
#include <sys/stat.h>
#include <zlib.h>

namespace craft {
inline constexpr char compact_gzip_format[]="certified-family-results-gzip-v1";

class PendingResultTable {
    std::filesystem::path destination, scratch;
    int fd=-1;
    static void fail(const char* operation){
        throw std::system_error(errno,std::generic_category(),operation);
    }
public:
    explicit PendingResultTable(const std::filesystem::path& path):destination(path){
        auto dir=path.parent_path();
        fd=::open(dir.c_str(),O_TMPFILE|O_RDWR|O_CLOEXEC,0666);
        if(fd>=0)return;
        if(errno!=EOPNOTSUPP&&errno!=EINVAL&&errno!=EISDIR&&errno!=ENOSYS)fail("create anonymous result table");
        scratch=dir/pending_result_name;
        fd=::open(scratch.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0666);
        if(fd<0)fail("create pending result table");
        named_result_pending=1;
    }
    PendingResultTable(const PendingResultTable&)=delete;
    PendingResultTable& operator=(const PendingResultTable&)=delete;
    ~PendingResultTable(){
        if(fd>=0)::close(fd);
        if(!scratch.empty()){::unlink(scratch.c_str());named_result_pending=0;}
    }
    gzFile compressor()const{
        int copy=::dup(fd);
        if(copy<0)fail("duplicate result descriptor");
        gzFile out=gzdopen(copy,"wb1");
        if(!out){::close(copy);throw std::runtime_error("cannot open gzip result writer");}
        return out;
    }
    uintmax_t publish(){
        struct stat info{};
        if(::fsync(fd)<0||::fstat(fd,&info)<0)fail("sync compressed result table");
        if(scratch.empty()){
            auto source="/proc/self/fd/"+std::to_string(fd);
            if(::linkat(AT_FDCWD,source.c_str(),AT_FDCWD,destination.c_str(),AT_SYMLINK_FOLLOW)<0)
                fail("publish anonymous result table");
        }else{
            if(::link(scratch.c_str(),destination.c_str())<0)fail("publish compressed result table");
            if(::unlink(scratch.c_str())<0)fail("unlink pending result table");
            scratch.clear();
            named_result_pending=0;
        }
        durable_file(destination.parent_path());
        return uintmax_t(info.st_size);
    }
};

inline void discard_pending_result(const std::filesystem::path& dir){
    auto path=dir/pending_result_name;
    auto status=std::filesystem::symlink_status(path);
    if(!std::filesystem::exists(status))return;
    require(std::filesystem::is_regular_file(status),"unexpected file type at pending result path");
    std::filesystem::remove(path);
}
struct GzipCloser {void operator()(gzFile f)const{gzclose(f);}};
using GzipHandle=std::unique_ptr<gzFile_s,GzipCloser>;

template<class Write> uintmax_t write_gzip_table(const std::filesystem::path& path,Write write){
    PendingResultTable pending(path);
    GzipHandle out(pending.compressor());
    write([&](const J& row){
        auto line=row.dump();
        line.push_back('\n');
        if(gzfwrite(line.data(),1,line.size(),out.get())!=line.size())
            throw std::runtime_error(std::string("cannot write gzip result table: ")+gzerror(out.get(),nullptr));
    });
    require(gzclose(out.release())==Z_OK,"cannot finish gzip result table");
    return pending.publish();
}

template<class Row> void read_gzip_table(const std::filesystem::path& path,Row row){
    GzipHandle in(gzopen(path.c_str(),"rb"));
    require(in!=nullptr,"missing compressed result table");
    require(!gzdirect(in.get()),"expected a gzip result table");
    std::array<char,65536> buffer;
    std::string line;
    int n;
    while((n=gzread(in.get(),buffer.data(),buffer.size()))>0){
        const char* begin=buffer.data();
        const char* end=begin+n;
        while(begin<end){
            auto newline=static_cast<const char*>(std::memchr(begin,'\n',size_t(end-begin)));
            line.append(begin,newline?newline:end);
            if(!newline)break;
            row(line);
            line.clear();
            begin=newline+1;
        }
    }
    int code=Z_OK;
    const char* detail=gzerror(in.get(),&code);
    if(n<0||(code!=Z_OK&&code!=Z_STREAM_END))
        throw std::runtime_error(std::string("invalid gzip result table: ")+detail);
    require(line.empty(),"unfinished result table row");
    require(gzclose(in.release())==Z_OK,"cannot finish gzip result read");
}
}
