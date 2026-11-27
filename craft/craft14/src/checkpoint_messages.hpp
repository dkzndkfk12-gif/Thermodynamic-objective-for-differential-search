#pragma once
#include "contraction.hpp"
#include "vendor/zstd.h"
#include <filesystem>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace craft {
inline void zstd_check(size_t result){if(ZSTD_isError(result))throw std::runtime_error(std::string("checkpoint compression: ")+ZSTD_getErrorName(result));}
inline void durable_file(const std::filesystem::path& path){int fd=::open(path.c_str(),O_RDONLY);require(fd>=0,"cannot open checkpoint for sync");int code=::fsync(fd);::close(fd);require(code==0,"checkpoint fsync failed");}
inline std::string safe_file(const J& name){auto s=name.get<std::string>();require(std::filesystem::path(s).filename()==s&&!s.empty()&&s!="."&&s!="..","checkpoint reference must be a local filename");return s;}
class ZstdFrameWriter {
    std::ofstream& file;
    ZSTD_CStream* stream=ZSTD_createCStream();
    std::vector<char> input=std::vector<char>(ZSTD_CStreamInSize()),output=std::vector<char>(ZSTD_CStreamOutSize());
    size_t used=0;
    std::function<void()> keep_writing;
    void drain(){if(keep_writing)keep_writing();ZSTD_inBuffer in{input.data(),used,0};while(in.pos<in.size){ZSTD_outBuffer out{output.data(),output.size(),0};zstd_check(ZSTD_compressStream(stream,&out,&in));file.write(output.data(),std::streamsize(out.pos));}used=0;}
public:
    explicit ZstdFrameWriter(std::ofstream& f,std::function<void()> allow={}):file(f),keep_writing(std::move(allow)){require(stream!=nullptr,"cannot create checkpoint compressor");}
    ~ZstdFrameWriter(){ZSTD_freeCStream(stream);}
    void begin(){used=0;zstd_check(ZSTD_initCStream(stream,1));zstd_check(ZSTD_CCtx_setParameter(stream,ZSTD_c_checksumFlag,1));}
    void write(const void* data,size_t n){auto p=static_cast<const char*>(data);while(n){auto k=std::min(n,input.size()-used);std::memcpy(input.data()+used,p,k);used+=k;p+=k;n-=k;if(used==input.size())drain();}}
    void number(uint64_t n){unsigned char bytes[10];size_t k=0;do{bytes[k++]=uint8_t((n&127)|(n>=128?128:0));n>>=7;}while(n);write(bytes,k);}
    void end(){drain();size_t remaining;do{ZSTD_outBuffer out{output.data(),output.size(),0};remaining=ZSTD_endStream(stream,&out);zstd_check(remaining);file.write(output.data(),std::streamsize(out.pos));}while(remaining);}
};
class ZstdFrameReader {
    std::ifstream& file;
    ZSTD_DStream* stream=ZSTD_createDStream();
    std::vector<char> input=std::vector<char>(ZSTD_DStreamInSize()),output=std::vector<char>(ZSTD_DStreamOutSize());
    ZSTD_inBuffer in{nullptr,0,0};size_t position=0,available=0;uint64_t compressed_remaining=0;bool finished=false;
    void fill(){
        position=available=0;
        while(!available&&!finished){
            if(in.pos==in.size){require(compressed_remaining>0,"truncated compressed message");size_t n=size_t(std::min<uint64_t>(compressed_remaining,input.size()));file.read(input.data(),std::streamsize(n));require(file.gcount()==std::streamsize(n),"truncated message file");compressed_remaining-=n;in={input.data(),n,0};}
            ZSTD_outBuffer out{output.data(),output.size(),0};size_t result=ZSTD_decompressStream(stream,&out,&in);zstd_check(result);available=out.pos;finished=result==0;
        }
    }
public:
    ZstdFrameReader(std::ifstream& f,uint64_t offset,uint64_t bytes):file(f),compressed_remaining(bytes){require(stream!=nullptr,"cannot create checkpoint decompressor");file.clear();file.seekg(std::streamoff(offset));require(bool(file),"bad checkpoint offset");zstd_check(ZSTD_initDStream(stream));}
    ~ZstdFrameReader(){ZSTD_freeDStream(stream);}
    void read(void* data,size_t n){auto p=static_cast<char*>(data);while(n){if(position==available){fill();require(available>0,"message payload ended early");}size_t k=std::min(n,available-position);std::memcpy(p,output.data()+position,k);position+=k;p+=k;n-=k;}}
    uint64_t number(){uint64_t n=0;for(unsigned k=0;k<10;++k){uint8_t b;read(&b,1);require(k<9||(b&254)==0,"message integer overflow");n|=uint64_t(b&127)<<(7*k);if(!(b&128))return n;}throw std::runtime_error("bad message integer");}
    void end(){require(position==available,"unexpected data after message");if(!finished){fill();require(available==0&&finished,"extra message payload");}require(compressed_remaining==0&&in.pos==in.size,"extra compressed frame data");}
};
class MessageRegistry {
    std::mutex mutex;
    std::unordered_map<const SumTable*,uint64_t> pointers;
    std::unordered_map<std::string,uint64_t> meanings;
public:
    std::atomic<uint64_t> unique{0},references{0},written{0},bytes{0};
    std::pair<uint64_t,bool> claim(const SumTable& table){
        ++references;
        {std::lock_guard<std::mutex> lock(mutex);auto p=pointers.find(&table);if(p!=pointers.end())return {p->second,false};}
                                                                                 
                                                                                
        auto key=table.key+"/checkpoint-prefix/"+std::to_string(table.data.size());
        std::lock_guard<std::mutex> lock(mutex);auto p=pointers.find(&table);if(p!=pointers.end())return {p->second,false};
        auto old=meanings.find(key);if(old!=meanings.end()){pointers.emplace(&table,old->second);return {old->second,false};}
        uint64_t id=unique.fetch_add(1);meanings.emplace(std::move(key),id);pointers.emplace(&table,id);return {id,true};
    }
};
class MessageShardWriter:public TableSink {
    MessageRegistry& registry;
    std::filesystem::path data_path,index_path,data_tmp,index_tmp;
    std::ofstream file,index;
    ZstdFrameWriter compressed;
public:
    MessageShardWriter(MessageRegistry& r,std::filesystem::path data,std::filesystem::path idx,std::function<void()> allow={})
        :registry(r),data_path(std::move(data)),index_path(std::move(idx)),data_tmp(data_path.string()+".tmp"),index_tmp(index_path.string()+".tmp"),file(data_tmp,std::ios::binary),index(index_tmp),compressed(file,std::move(allow)){file.exceptions(std::ios::failbit|std::ios::badbit);index.exceptions(std::ios::failbit|std::ios::badbit);}
    J reference(const SumTable& table)override{
        auto [id,write]=registry.claim(table);
        if(write){
            uint64_t offset=uint64_t(file.tellp());compressed.begin();const char magic[]="CRAFTMSG3";compressed.write(magic,9);
            auto metadata=J{{"scope",table.scope},{"domains",table.domains},{"key",table.key},{"degree",table.degree},{"coefficient_bits",table.coefficient_bits},{"rows",table.data.size()}}.dump();compressed.number(metadata.size());compressed.write(metadata.data(),metadata.size());
            std::vector<unsigned char> buffer;
            for(const auto& n:table.data){require(n>=0,"negative checkpoint coefficient");size_t bytes=n==0?0:(mpz_sizeinbase(n.get_mpz_t(),2)+7)/8;compressed.number(bytes);if(bytes){buffer.resize(bytes);size_t written=0;mpz_export(buffer.data(),&written,1,1,1,0,n.get_mpz_t());require(written==bytes,"integer export length mismatch");compressed.write(buffer.data(),bytes);}}
            compressed.end();uint64_t bytes=uint64_t(file.tellp())-offset;
            index<<J{{"id",id},{"offset",offset},{"compressed_bytes",bytes}}.dump()<<'\n';registry.bytes.fetch_add(bytes);++registry.written;
        }return {{"message_ref",id}};
    }
    void finish(){file.flush();index.flush();file.close();index.close();durable_file(data_tmp);durable_file(index_tmp);std::filesystem::rename(data_tmp,data_path);std::filesystem::rename(index_tmp,index_path);}
};
class MessageStoreReader:public TableSource {
    struct Location{size_t shard;uint64_t offset,bytes;};
    std::vector<std::unique_ptr<std::ifstream>> files;
    std::unordered_map<uint64_t,Location> locations;
    std::unordered_map<uint64_t,std::weak_ptr<SumTable>> loaded;
public:
    MessageStoreReader(const std::filesystem::path& directory,const J& manifest){
        for(const auto& shard:manifest.at("message_shards")){
            auto data=directory/safe_file(shard.at("data_file")),index=directory/safe_file(shard.at("index_file"));uint64_t bytes=std::filesystem::file_size(data);auto f=std::make_unique<std::ifstream>(data,std::ios::binary);require(bool(*f),"missing checkpoint message data");size_t number=files.size();files.push_back(std::move(f));std::ifstream in(index);require(bool(in),"missing checkpoint message index");std::string line;
            while(std::getline(in,line)){auto j=J::parse(line);uint64_t id=j.at("id").get<uint64_t>(),offset=j.at("offset").get<uint64_t>(),size=j.at("compressed_bytes").get<uint64_t>();require(size>0&&offset<=bytes&&size<=bytes-offset,"message offset outside data file");require(locations.emplace(id,Location{number,offset,size}).second,"duplicate checkpoint message id");}require(!in.bad(),"message index read failed");
        }require(locations.size()==manifest.at("unique_messages").get<size_t>(),"checkpoint message count mismatch");
    }
    std::shared_ptr<SumTable> load(uint64_t id)override{
        auto found=loaded.find(id);if(found!=loaded.end())if(auto p=found->second.lock())return p;
        auto location=locations.find(id);require(location!=locations.end(),"missing referenced message");auto at=location->second;ZstdFrameReader in(*files[at.shard],at.offset,at.bytes);char magic[9];in.read(magic,9);require(std::memcmp(magic,"CRAFTMSG3",9)==0,"bad message format");uint64_t bytes=in.number();require(bytes<=std::string().max_size(),"invalid message header size");std::string header(size_t(bytes),'\0');in.read(header.data(),header.size());auto j=J::parse(header);
        auto t=std::make_shared<SumTable>();t->scope=j.at("scope").get<std::vector<int>>();t->domains=j.at("domains").get<std::vector<uint16_t>>();t->key=j.at("key").get<std::string>();t->degree=j.at("degree").get<unsigned>();t->coefficient_bits=j.at("coefficient_bits").get<unsigned>();require(t->coefficient_bits>0&&t->scope.size()==t->domains.size(),"invalid message geometry");for(auto d:t->domains)require(d!=0,"zero message domain");size_t rows=j.at("rows").get<size_t>();require(rows<=t->cells(),"too many message prefix rows");t->data.reserve(rows);std::vector<unsigned char> buffer;uint64_t maximum=((uint64_t(t->degree)+1)*t->coefficient_bits+7)/8;
        for(size_t k=0;k<rows;++k){uint64_t n=in.number();require(n<=maximum,"message coefficient exceeds histogram bound");buffer.resize(size_t(n));in.read(buffer.data(),buffer.size());mpz_class value=0;if(n)mpz_import(value.get_mpz_t(),size_t(n),1,1,1,0,buffer.data());t->data.push_back(std::move(value));}
        in.end();loaded[id]=t;return t;
    }
};
struct TableSinkScope{TableSink* previous;explicit TableSinkScope(TableSink* sink):previous(checkpoint_table_sink){checkpoint_table_sink=sink;}~TableSinkScope(){checkpoint_table_sink=previous;}};
struct TableSourceScope{TableSource* previous;explicit TableSourceScope(TableSource* source):previous(checkpoint_table_source){checkpoint_table_source=source;}~TableSourceScope(){checkpoint_table_source=previous;}};
}                   
