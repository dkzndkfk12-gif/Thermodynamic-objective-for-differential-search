#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <omp.h>
#ifdef __linux__
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "corridor_integer.hpp"

namespace pair_transfer {
using Word=gift_word::Word;
namespace fs=std::filesystem;
inline volatile std::sig_atomic_t interrupted=0;
inline void signal_handler(int){interrupted=1;}
struct Pause:std::runtime_error{using std::runtime_error::runtime_error;};
struct Limit:std::runtime_error{using std::runtime_error::runtime_error;};
struct Scratch:std::runtime_error{using std::runtime_error::runtime_error;};
struct Timer{
    using Clock=std::chrono::steady_clock;
    Clock::time_point start=Clock::now();double seconds;
    explicit Timer(double s):seconds(s){}
    double elapsed()const{return std::chrono::duration<double>(Clock::now()-start).count();}
    void check()const{if(interrupted||elapsed()>=seconds)throw Pause("TIME_OR_SIGNAL");}
};
inline int activity(Word x){int n=0;for(int c=0;c<NC;++c){n+=bool(x&15);x>>=4;}return n;}
inline int perm(int i){return P(i);}
inline Word inverse(Word x){Word y=0;for(int i=0;i<NB;++i)if((x>>perm(i))&1)y|=Word(1)<<i;return y;}
inline std::string hex(Word x){return gift_word::text(x,NC);}
inline Word word(const std::string& s){return gift_word::parse(s,NC);}
struct Options{
    Word a=0,b=0;int cap=0,threads=1;double seconds=60;
    std::uint64_t nodes=0,memory=0,token=0;fs::path work,out;
};

struct Kernel{
    struct Choice{Word bits,inverse_bits;unsigned numerator;};
    std::array<std::array<std::vector<Choice>,16>,NC> forward,backward;
    Kernel(){
        constexpr int s[16]={1,10,4,12,6,15,3,9,2,13,11,7,5,0,8,14};
        int d[16][16]={};for(int a=0;a<16;++a)for(int x=0;x<16;++x)++d[a][s[x]^s[x^a]];
        for(int a=1;a<16;++a)for(int b=1;b<16;++b)if(d[a][b]){
            if(d[a][b]!=2&&d[a][b]!=4&&d[a][b]!=6)throw std::runtime_error("DDT multiplicity is not supported");
            for(int c=0;c<NC;++c){
                Word z=0;for(int bit=0;bit<4;++bit)if((b>>bit)&1)z|=Word(1)<<perm(4*c+bit);
                forward[std::size_t(c)][std::size_t(a)].push_back({z,0,unsigned(d[a][b]/2)});
                Word x=Word(a)<<(4*c);
                backward[std::size_t(c)][std::size_t(b)].push_back({x,inverse(x),unsigned(d[a][b]/2)});
            }
        }
    }
    template<class F>void edges(Word x,bool reverse,int cap,bool prune_incoming,F&& emit,Timer& timer)const{
        if(reverse)x=inverse(x);

        if(reverse&&activity(x)>cap)return;
        std::array<int,NC> cells{},values{};int count=0;
        for(int c=0;c<NC;++c)if((x>>(4*c))&15){cells[count]=c;values[count++]=int((x>>(4*c))&15);}
        const auto& table=reverse?backward:forward;std::uint64_t visits=0;
        auto visit=[&](auto&& self,int at,Word y,Word inv,std::uint64_t num)->void{
            if((++visits&1023)==0)timer.check();
            if(at==count){emit(y,3*count,num);return;}
            for(const auto& q:table[std::size_t(cells[at])][std::size_t(values[at])]){
                Word z=y|q.bits,iz=inv|q.inverse_bits;
                if(!reverse&&activity(z)>cap)continue;

                if(reverse&&prune_incoming&&activity(iz)>cap)continue;
                self(self,at+1,z,iz,num*q.numerator);
            }
        };
        timer.check();visit(visit,0,0,0,1);
    }
};

struct Writer{
    fs::path target,temp;std::ofstream f;std::uint64_t checksum=1469598103934665603ULL;
    explicit Writer(const fs::path& p):target(p),temp(p.string()+".tmp"),f(temp,std::ios::binary|std::ios::trunc){
        f.exceptions(std::ios::badbit|std::ios::failbit);
    }
    void bytes(const unsigned char* data,std::size_t n){
        f.write(reinterpret_cast<const char*>(data),std::streamsize(n));
        for(std::size_t i=0;i<n;++i){checksum^=data[i];checksum*=1099511628211ULL;}
    }
    void u64(std::uint64_t v){unsigned char b[8];for(int i=0;i<8;++i)b[i]=static_cast<unsigned char>(v>>(8*i));bytes(b,8);}
    void state(Word v){u64(std::uint64_t(v));u64(std::uint64_t(v>>64));}
    template<class M>void mass(const M& m){
        unsigned used=M::LIMBS;while(used&&!m.w[used-1])--used;
        const unsigned char count=static_cast<unsigned char>(used);bytes(&count,1);
        std::array<unsigned char,M::LIMBS*4> b{};
        for(unsigned i=0;i<used;++i)for(unsigned j=0;j<4;++j)b[4*i+j]=static_cast<unsigned char>(m.w[i]>>(8*j));
        bytes(b.data(),4*used);
    }
    void finish(){
        unsigned char b[8];for(int i=0;i<8;++i)b[i]=static_cast<unsigned char>(checksum>>(8*i));
        f.write(reinterpret_cast<const char*>(b),8);f.flush();f.close();
#ifdef __linux__
        int fd=::open(temp.c_str(),O_RDONLY);if(fd<0)throw std::runtime_error("cannot sync checkpoint");
        const int ok=::fsync(fd);::close(fd);if(ok)throw std::runtime_error("cannot sync checkpoint");
#endif
        std::error_code ec;fs::rename(temp,target,ec);
#ifdef _WIN32
        if(ec){fs::remove(target);fs::rename(temp,target,ec);}
#endif
        if(ec)throw std::runtime_error("cannot commit checkpoint: "+ec.message());
#ifdef __linux__
        fd=::open(target.parent_path().c_str(),O_RDONLY|O_DIRECTORY);
        if(fd>=0){::fsync(fd);::close(fd);}
#endif
    }
};
struct Reader{
    std::ifstream f;std::uint64_t checksum=1469598103934665603ULL;
    explicit Reader(const fs::path& p):f(p,std::ios::binary){f.exceptions(std::ios::badbit|std::ios::failbit);}
    void bytes(unsigned char* data,std::size_t n){
        f.read(reinterpret_cast<char*>(data),std::streamsize(n));
        for(std::size_t i=0;i<n;++i){checksum^=data[i];checksum*=1099511628211ULL;}
    }
    std::uint64_t u64(){unsigned char b[8];bytes(b,8);std::uint64_t v=0;for(int i=0;i<8;++i)v|=std::uint64_t(b[i])<<(8*i);return v;}
    Word state(){Word lo=u64();return lo|(Word(u64())<<64);}
    template<class M>M mass(){
        unsigned char used=0;bytes(&used,1);if(used>M::LIMBS)throw std::runtime_error("invalid packed integer length");
        M m;std::array<unsigned char,M::LIMBS*4> b{};bytes(b.data(),4*used);
        for(unsigned i=0;i<used;++i)for(unsigned j=0;j<4;++j)m.w[i]|=std::uint32_t(b[4*i+j])<<(8*j);
        if(used&&!m.w[used-1])throw std::runtime_error("noncanonical packed integer");
        return m;
    }
    void finish(){
        unsigned char b[8];f.read(reinterpret_cast<char*>(b),8);std::uint64_t v=0;
        for(int i=0;i<8;++i)v|=std::uint64_t(b[i])<<(8*i);
        if(v!=checksum||f.peek()!=std::char_traits<char>::eof())throw std::runtime_error("checkpoint checksum/length mismatch");
    }
};

template<class M,int R,class K=Kernel>class Engine{
public:
    using Entry=std::pair<Word,M>;using Layer=std::vector<Entry>;using Map=std::unordered_map<Word,M,gift_word::Hash>;
    Options o;Timer& timer;K kernel;
    int phase=0,layer=0,split=R/2,max_threads_observed=0;
    std::uint64_t cursor=0,generation=0,edges=0,solved=0,join_a=0,join_b=0;
    std::size_t batch=256;Layer current;Map next;M answer;
    std::clock_t cpu_start=std::clock();double saved_at=0;bool resumed=false,dirty=true;
    explicit Engine(const Options& options,Timer& clock):o(options),timer(clock){
        static_assert(R>=2&&R<=40,"unsupported rounds");
        if(denominator()>=int(32*M::LIMBS))throw std::runtime_error("integer width is too small");
        fs::create_directories(o.work);
        if(fs::is_symlink(o.work))throw std::runtime_error("scratch task directory must not be a symlink");
        for(const auto* f:{"checkpoint.bin.tmp","forward.bin.tmp","backward.bin.tmp","progress.json.tmp"}){
            std::error_code ec;fs::remove(o.work/f,ec);
        }
        if(fs::exists(o.work/"checkpoint.bin")){load();resumed=true;dirty=false;}
        else current.emplace_back(o.a,M(1));
    }
    int denominator()const{return 3*activity(o.a)+3*o.cap*(R-1);}
    void signature(Writer& w)const{
        w.u64(0x4749465444503031ULL);w.u64(R);w.u64(NB);w.state(o.a);w.state(o.b);w.u64(o.cap);w.u64(M::LIMBS);w.u64(split);
    }
    void signature(Reader& r)const{
        if(r.u64()!=0x4749465444503031ULL||r.u64()!=R||r.u64()!=NB||r.state()!=o.a||r.state()!=o.b||
           r.u64()!=std::uint64_t(o.cap)||r.u64()!=M::LIMBS||r.u64()!=std::uint64_t(split))
            throw std::runtime_error("checkpoint belongs to a different exact problem");
    }
    void write_layer(Writer& w,const Layer& values)const{
        w.u64(values.size());for(const auto& x:values){w.state(x.first);w.mass(x.second);}
    }
    Layer read_layer(Reader& r,bool sorted)const{
        const auto n=r.u64();if(n>o.nodes)throw Limit("SAVED_FRONTIER_EXCEEDS_RAM_BUDGET");
        Layer result;result.reserve(std::size_t(n));
        for(std::uint64_t i=0;i<n;++i){Word x=r.state();M m=r.template mass<M>();
            if(!x||m.zero()||(sorted&&i&&result.back().first>=x))throw std::runtime_error("invalid checkpoint state order/mass");
            result.emplace_back(x,m);
        }return result;
    }
    void save_frontier(const std::string& name,const Layer& v)const{
        disk_space(v.size());
        Writer w(o.work/name);signature(w);write_layer(w,v);w.finish();
    }
    Layer load_frontier(const std::string& name)const{
        Reader r(o.work/name);signature(r);Layer v=read_layer(r,true);r.finish();return v;
    }
    void save(){
        if(generation&&!dirty){saved_at=timer.elapsed();progress("CHECKPOINT_CURRENT");return;}
        disk_space(current.size()+next.size());
        Writer w(o.work/"checkpoint.bin");signature(w);
        w.u64(++generation);w.u64(phase);w.u64(layer);w.u64(cursor);w.u64(batch);
        w.u64(edges);w.u64(solved);w.u64(join_a);w.u64(join_b);w.mass(answer);write_layer(w,current);
        w.u64(next.size());for(const auto& x:next){w.state(x.first);w.mass(x.second);}w.finish();saved_at=timer.elapsed();dirty=false;
        progress("CHECKPOINTED");
    }
    void disk_space(std::size_t states)const{
        std::error_code ec;const auto info=fs::space(o.work,ec);

        const std::uint64_t need=512+std::uint64_t(states)*(17+4*M::LIMBS)+(64ULL<<20);
        if(!ec&&info.available<need)throw Scratch("SCRATCH_SPACE_LIMIT");
    }
    void load(){
        Reader r(o.work/"checkpoint.bin");signature(r);
        generation=r.u64();phase=int(r.u64());layer=int(r.u64());cursor=r.u64();batch=std::size_t(r.u64());
        edges=r.u64();solved=r.u64();join_a=r.u64();join_b=r.u64();answer=r.template mass<M>();current=read_layer(r,true);
        const auto n=r.u64();if(n>o.nodes||current.size()>o.nodes-n)throw Limit("SAVED_FRONTIER_EXCEEDS_RAM_BUDGET");
        for(std::uint64_t i=0;i<n;++i){Word x=r.state();M m=r.template mass<M>();
            if(!x||m.zero()||!next.emplace(x,m).second)throw std::runtime_error("invalid accumulator checkpoint");
        }r.finish();
        if(phase<0||phase>3||layer<0||layer>R||cursor>current.size()||batch<1||batch>4096)
            throw std::runtime_error("invalid checkpoint phase/cursor");
        if(phase==0&&layer>split)throw std::runtime_error("invalid forward layer");
        if(phase==1&&layer<split)throw std::runtime_error("invalid backward layer");
    }
    void progress(const std::string& reason){
        fs::path p=o.work/"progress.json",tmp=p.string()+".tmp";std::ofstream f(tmp);
        f.exceptions(std::ios::badbit|std::ios::failbit);
        f<<std::setprecision(17)<<"{\"version\":\"endpoint_integer_v1\",\"token\":"<<o.token
         <<",\"status\":\""<<(phase==3?"PAIR_DONE":"PAIR_PROGRESS")<<"\",\"reason\":\""<<reason
         <<"\",\"generation\":"<<generation<<",\"phase\":"<<phase<<",\"round\":"<<layer
         <<",\"cursor\":"<<cursor<<",\"layer_sources\":"<<current.size()<<",\"next_states\":"<<next.size()
         <<",\"completed_sources\":"<<solved<<",\"processed_edges\":"<<edges<<",\"batch\":"<<batch
         <<",\"input\":\""<<hex(o.a)<<"\",\"output\":\""<<hex(o.b)<<"\",\"cap\":"<<o.cap
         <<",\"denominator_power\":"<<denominator()<<",\"numerator\":\""<<(phase==3?answer.str():"0")
         <<"\",\"resumed\":"<<(resumed?"true":"false")<<",\"threads\":"<<o.threads
         <<",\"actual_threads_max\":"<<max_threads_observed
         <<",\"integer_bits\":"<<32*M::LIMBS<<",\"cpu_seconds\":"<<double(std::clock()-cpu_start)/CLOCKS_PER_SEC
         <<",\"job_wall_seconds\":"<<timer.elapsed()<<",\"probability_complete\":"<<(phase==3?"true":"false")<<"}\n";
        f.close();std::error_code ec;fs::rename(tmp,p,ec);
#ifdef _WIN32
        if(ec){fs::remove(p);fs::rename(tmp,p,ec);}
#endif
        if(ec)throw std::runtime_error("cannot commit progress record");
    }
    void expand_batch(){
        timer.check();const auto end=std::min(current.size(),std::size_t(cursor)+batch);
        const int nthreads=std::min(o.threads,int(end-std::size_t(cursor)));
        std::vector<Map> local{std::size_t(nthreads)};
        std::vector<std::uint64_t> local_edges(std::size_t(nthreads),0);
        std::atomic<std::uint64_t> new_nodes{0};std::atomic<int> failure{0};
        auto failed=[&](int value){int old=failure.load();while(old<value&&!failure.compare_exchange_weak(old,value)){};};
        const auto fixed_nodes=current.size()+next.size();
        if(fixed_nodes>=o.nodes)throw Limit("NODE_LIMIT");
        const int shift=3*((phase==0&&layer==0)?activity(o.a):o.cap);

#pragma omp parallel num_threads(nthreads)
        {
            const int tid=omp_get_thread_num(),actual_threads=omp_get_num_threads();auto& dest=local[std::size_t(tid)];
            if(tid==0)max_threads_observed=std::max(max_threads_observed,actual_threads);
            try{
                for(std::size_t i=std::size_t(cursor)+std::size_t(tid);i<end;i+=std::size_t(actual_threads)){
                    if(failure.load())break;
                    timer.check();const auto& entry=current[i];
                    kernel.edges(entry.first,phase==1,o.cap,phase==1&&layer-1>1,[&](Word y,int w,std::uint64_t num){
                        if(w>shift)throw std::runtime_error("transition exceeds exact common denominator");
                        auto inserted=dest.emplace(y,M{});
                        if(inserted.second&&fixed_nodes+(++new_nodes)>o.nodes)throw Limit("NODE_LIMIT");
                        inserted.first->second+=(entry.second*M(num)).shifted(unsigned(shift-w));
                        ++local_edges[std::size_t(tid)];
                    },timer);
                }
            }catch(const Pause&){failed(1);}
             catch(const Limit&){failed(2);}
             catch(const std::bad_alloc&){failed(2);}
             catch(...){failed(3);}
        }
        if(failure.load()){
            batch=std::max<std::size_t>(1,batch/2);dirty=true;
            if(failure.load()==1)throw Pause("BATCH_YIELDED");
            if(failure.load()==2)throw Limit("NODE_OR_MEMORY_LIMIT");
            throw std::runtime_error("integer or transition invariant failed in parallel batch");
        }

        try{next.reserve(next.size()+std::size_t(new_nodes.load()));}
        catch(const std::bad_alloc&){batch=std::max<std::size_t>(1,batch/2);dirty=true;throw Limit("MERGE_RAM_LIMIT");}
        for(auto& m:local)for(auto it=m.begin();it!=m.end();){
            auto item=it++;auto found=next.find(item->first);
            if(found!=next.end()){found->second+=item->second;m.erase(item);}
            else{
                auto inserted=next.insert(m.extract(item));
                if(!inserted.inserted)throw std::runtime_error("unexpected duplicate during transactional merge");
            }
        }
        for(auto n:local_edges)edges+=n;
        solved+=end-std::size_t(cursor);cursor=end;dirty=true;
        batch=std::min<std::size_t>(4096,std::max<std::size_t>(batch,1));
    }
    void finish_layer(){
        dirty=true;
        Layer following;following.reserve(next.size());
        for(const auto& x:next)following.push_back(x);
        std::sort(following.begin(),following.end(),[](const Entry& x,const Entry& y){return x.first<y.first;});
        next.clear();next.rehash(0);current=std::move(following);cursor=0;
        layer+=phase==0?1:-1;
        if(current.empty()){answer=M{};phase=3;save();return;}
        if(phase==0&&layer==split){
            save_frontier("forward.bin",current);current.clear();current.shrink_to_fit();
            current.emplace_back(o.b,M(1));phase=1;layer=R;
        }else if(phase==1&&layer==split){
            save_frontier("backward.bin",current);current.clear();current.shrink_to_fit();phase=2;
        }
        save();
    }
    void join(){
        Layer a=load_frontier("forward.bin"),b=load_frontier("backward.bin");
        if(a.size()>o.nodes-b.size())throw Limit("JOIN_RAM_LIMIT");
        if(join_a>a.size()||join_b>b.size())throw std::runtime_error("invalid join checkpoint offsets");
        std::uint64_t iterations=0;
        while(join_a<a.size()&&join_b<b.size()){
            if((++iterations&1023)==0)timer.check();
            const auto& x=a[std::size_t(join_a)];const auto& y=b[std::size_t(join_b)];
            if(x.first<y.first)++join_a;
            else if(y.first<x.first)++join_b;
            else{answer+=x.second*y.second;++join_a;++join_b;}
            dirty=true;
            if(timer.elapsed()-saved_at>=15)save();
        }
        if(M(1).shifted(unsigned(denominator()))<answer)throw std::runtime_error("endpoint probability exceeds one");
        phase=3;dirty=true;save();
    }
    void run(){
        std::string reason="EXACT_COMPLETE";
        try{
            if(!generation)save();
            while(phase<2){
                timer.check();if(cursor==current.size())finish_layer();
                else{expand_batch();if(timer.elapsed()-saved_at>=15)save();}
            }
            if(phase==2)join();
        }catch(const Pause&){reason="TIME_LIMIT";try{save();}catch(const Scratch&){reason="SCRATCH_SPACE_LIMIT";}}
         catch(const Limit&){reason="RESOURCE_LIMIT";try{save();}catch(const Scratch&){reason="SCRATCH_SPACE_LIMIT";}}
         catch(const Scratch&){reason="SCRATCH_SPACE_LIMIT";}
         catch(const std::bad_alloc&){

            reason="MEMORY_LIMIT_RELOAD_CHECKPOINT";
            progress(reason);return;
         }
        progress(reason);
        fs::copy_file(o.work/"progress.json",o.out,fs::copy_options::overwrite_existing);
    }
};
}
