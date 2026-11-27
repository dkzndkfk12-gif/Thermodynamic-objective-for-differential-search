

#pragma once
#include "../../common/native_memo.hpp"
#include "../../common/integer_graph.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "rounds.hpp"
#include "ddt_support.hpp"
#ifdef __linux__
#include <sys/resource.h>
#endif
namespace adaptive {
using Clock=std::chrono::steady_clock;
using Mask=std::bitset<256>;
using Domains=std::vector<Mask>;
using Box=std::map<int,Mask>;
static volatile std::sig_atomic_t stopped=0;
inline void on_signal(int){stopped=1;}
constexpr std::uint64_t SAT=std::numeric_limits<std::uint64_t>::max()/4;
inline std::uint64_t plus(std::uint64_t a,std::uint64_t b){return a>SAT-b?SAT:a+b;}
inline std::uint64_t times(std::uint64_t a,std::uint64_t b){return b&&a>SAT/b?SAT:a*b;}

class Num {
    std::vector<std::uint32_t> w;
    void trim(){while(!w.empty()&&!w.back())w.pop_back();}
    void check()const{if(w.size()>INTEGER_LIMBS)throw std::runtime_error("INTEGER_OVERFLOW");}
public:
    explicit Num(std::uint32_t x=0){if(x)w.push_back(x);}
    bool zero()const{return w.empty();}
    std::size_t bytes()const{return sizeof(Num)+w.capacity()*4;}
    Num& operator+=(const Num& b){
        auto n=std::max(w.size(),b.w.size());w.resize(n,0);std::uint64_t carry=0;
        for(std::size_t i=0;i<n;++i){auto s=carry+w[i]+(i<b.w.size()?std::uint64_t(b.w[i]):0);w[i]=std::uint32_t(s);carry=s>>32;}
        if(carry)w.push_back(std::uint32_t(carry));
        check();return *this;
    }
    Num& operator-=(const Num& b){
        std::uint64_t borrow=0;if(w.size()<b.w.size())throw std::runtime_error("negative exact integer");
        for(std::size_t i=0;i<w.size();++i){std::uint64_t sub=borrow+(i<b.w.size()?std::uint64_t(b.w[i]):0);auto old=std::uint64_t(w[i]);w[i]=std::uint32_t(old-sub);borrow=old<sub;}
        if(borrow)throw std::runtime_error("negative exact integer");
        trim();return *this;
    }
    Num& operator*=(std::uint32_t b){
        if(!b){w.clear();return *this;}std::uint64_t carry=0;
        for(auto& x:w){auto v=std::uint64_t(x)*b+carry;x=std::uint32_t(v);carry=v>>32;}
        if(carry)w.push_back(std::uint32_t(carry));
        check();return *this;
    }
    Num& operator*=(const Num& b){
        if(zero()||b.zero()){w.clear();return *this;}
        if(b.w.size()==1)return *this*=b.w[0];
        std::vector<std::uint32_t> out(w.size()+b.w.size(),0);
        for(std::size_t i=0;i<w.size();++i){std::uint64_t carry=0;
            for(std::size_t j=0;j<b.w.size();++j){auto v=std::uint64_t(w[i])*b.w[j]+out[i+j]+carry;out[i+j]=std::uint32_t(v);carry=v>>32;}
            out[i+b.w.size()]=std::uint32_t(carry);}
        w=std::move(out);trim();check();return *this;
    }
    std::string str()const{
        if(zero())return "0";
        auto c=w;std::vector<std::uint32_t> blocks;
        while(!c.empty()){std::uint64_t rem=0;for(std::size_t i=c.size();i-- >0;){auto v=(rem<<32)|c[i];c[i]=std::uint32_t(v/1000000000);rem=v%1000000000;}
            blocks.push_back(std::uint32_t(rem));while(!c.empty()&&!c.back())c.pop_back();}
        std::string s=std::to_string(blocks.back());for(std::size_t i=blocks.size()-1;i-- >0;){auto t=std::to_string(blocks[i]);s+=std::string(9-t.size(),'0')+t;}return s;
    }
};
inline std::vector<int> values(const Mask& m){std::vector<int> a;for(int x=1;x<256;++x)if(m[x])a.push_back(x);return a;}
inline int singleton(const Mask& m){if(m.count()!=1)return -1;for(int x=1;x<256;++x)if(m[x])return x;return -1;}
inline std::string hex(const Mask& m){
    const char* h="0123456789abcdef";std::string s(64,'0');
    for(int k=0;k<64;++k){unsigned x=0;for(int j=0;j<4;++j)if(m[4*k+j])x|=1u<<j;s[63-k]=h[x];}return s;
}
inline Mask mask(const std::string& s){
    if(s.empty()||s.size()>64)throw std::runtime_error("invalid domain mask");
    Mask m;for(std::size_t k=0;k<s.size();++k){char c=s[s.size()-1-k];int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
        if(v<0)throw std::runtime_error("invalid domain mask");
        for(int j=0;j<4;++j)if(v&(1<<j))m.set(4*k+j);}
    if(m.none()||m[0])throw std::runtime_error("empty/zero active domain");
    return m;
}
struct Factor {int a=0,b=0,id=0;std::vector<int> av,bv,scope;};
inline void scope(Factor& f){std::set<int> s(f.av.begin(),f.av.end());s.insert(f.bv.begin(),f.bv.end());f.scope.assign(s.begin(),s.end());}
struct Component {std::vector<int> vars;std::vector<Factor> factors;};
struct Plan {std::vector<int> order,critical;std::uint64_t operations=0,peak=0;int branch=-1;};
struct Work {std::vector<int> scope;bool dense=false;};
struct Table {std::string semantic;std::vector<int> scope;std::vector<Num> data;Factor factor;bool dense=false;};

struct Engine {
    int nv=0,nf=0;Domains base;std::vector<int> witness;std::vector<Factor> factors;
    std::vector<Box> queue;Num initial_volume,accumulated;
    std::uint64_t leaf_limit=0,table_bytes=0,cell_bytes=0,nodes=0,zeros=0,splits=0,cache_hits=0,kernel_operations=0;
    std::size_t max_frontier=0;Clock::time_point start,deadline,last_checkpoint;
    std::string output;int ddt[256][256]{};
    std::array<std::vector<int>,256> rows,columns,diagonals;
    std::unordered_map<std::uint32_t,std::vector<Mask>> relations;
    std::unordered_map<std::string,Num> cache;std::size_t cache_bytes=0;
    explicit Engine(const std::string& input,const std::string& out,std::uint64_t bytes,std::uint64_t ops,double secs,std::size_t frontier)
      :leaf_limit(ops),table_bytes(bytes),max_frontier(frontier),start(Clock::now()),
       deadline(start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(secs))),last_checkpoint(start),output(out){
        std::ifstream f(input);std::string magic;int nn;
        if(!(f>>magic>>nv>>nf>>nn)||magic!="SKINNY128_ADAPTIVE15_V2"||nv<0||nv>INTERNAL_CELLS||nf<ROUNDS||nf>MAX_FACTORS||nn<1||nn>1024)
            throw std::runtime_error("invalid adaptive header");
        if(std::size_t(nn)>max_frontier)throw std::runtime_error("request exceeds frontier capacity");
        queue.reserve(max_frontier);
        base.resize(nv);witness.resize(nv);
        for(int v=0;v<nv;++v){int n;f>>n;if(!f||n<1||n>255)throw std::runtime_error("invalid domain");
            for(int j=0;j<n;++j){int x;f>>x;if(!f||x<1||x>255||base[v][x])throw std::runtime_error("invalid domain value");base[v].set(x);}}
        for(int v=0;v<nv;++v){f>>witness[v];if(!f||witness[v]<1||witness[v]>255||!base[v][witness[v]])throw std::runtime_error("invalid witness coordinate");}
        for(int i=0;i<nf;++i){Factor a;a.id=i;f>>a.a>>a.b;if(!f||a.a<0||a.a>255||a.b<0||a.b>255)throw std::runtime_error("invalid DDT constant");
            for(auto* form:{&a.av,&a.bv}){int n;f>>n;if(!f||n<0||n>16)throw std::runtime_error("invalid XOR form");std::set<int> seen;
                for(int j=0;j<n;++j){int v;f>>v;if(!f||v<0||v>=nv||!seen.insert(v).second)throw std::runtime_error("invalid XOR variable");form->push_back(v);}}
            scope(a);factors.push_back(std::move(a));}
        for(int i=0;i<nn;++i){int n;f>>n;if(!f||n<0||n>nv)throw std::runtime_error("invalid box");Box b;
            for(int j=0;j<n;++j){int v;std::string s;f>>v>>s;if(!f||v<0||v>=nv||b.count(v))throw std::runtime_error("invalid box variable");auto m=mask(s);
                if((m&base[v])!=m)throw std::runtime_error("box outside base domain");
                b[v]=m;}queue.push_back(std::move(b));}
        if(f>>magic)throw std::runtime_error("trailing adaptive input");
        for(const auto& b:queue)initial_volume+=volume(b);

        cell_bytes=128+8*std::uint64_t((8*nv+5*nf+32)/32);
        for(int a=1;a<256;++a)for(int x=0;x<256;++x)++ddt[a][SBOX[x]^SBOX[x^a]];
        for(int a=1;a<256;++a)for(int b=1;b<256;++b){int& n=ddt[a][b];if(n%2||n>64)throw std::runtime_error("invalid S8 DDT");n/=2;
            if(n){rows[a].push_back(b);columns[b].push_back(a);diagonals[a^b].push_back(a);}}
    }
    void check()const{if(stopped||Clock::now()>=deadline)throw std::runtime_error("CHUNK_TIME_LIMIT");}
    Num volume(const Box& b)const{Num n(1);for(int v=0;v<nv;++v){auto it=b.find(v);n*=std::uint32_t((it==b.end()?base[v]:it->second).count());}return n;}
    Factor reduce(const Factor& f,const Domains& d)const{
        Factor g;g.id=f.id;g.a=f.a;g.b=f.b;
        for(int v:f.av){int x=singleton(d[v]);if(x<0)g.av.push_back(v);else g.a^=x;}
        for(int v:f.bv){int x=singleton(d[v]);if(x<0)g.bv.push_back(v);else g.b^=x;}scope(g);return g;
    }
    int value(const Factor& f,const std::vector<int>& x)const{int a=f.a,b=f.b;for(int v:f.av)a^=x[v];for(int v:f.bv)b^=x[v];return ddt[a][b];}
    const std::vector<Mask>& relation(const Factor& f){
        int u=f.scope[0],v=f.scope[1];auto role=[&](int x){return int(std::find(f.av.begin(),f.av.end(),x)!=f.av.end())+2*int(std::find(f.bv.begin(),f.bv.end(),x)!=f.bv.end());};
        int r=role(u),s=role(v);auto k=std::uint32_t(f.a|(f.b<<8)|(r<<16)|(s<<18));auto found=relations.find(k);if(found!=relations.end())return found->second;
        if(relations.size()>=256)relations.clear();
        std::vector<Mask> allowed(256);
        for(int x=1;x<256;++x){int a=f.a^((r&1)?x:0),b=f.b^((r&2)?x:0);
            const auto& list=s==1?columns[b]:s==2?rows[a]:diagonals[a^b];
            for(int z:list){int y=(s==2?b:a)^z;if(y)allowed[x].set(y);}}
        return relations.emplace(k,std::move(allowed)).first->second;
    }
    bool propagate(Domains& d){
        return ddt_support::propagate(d,factors,[&](int,int a,int b){return ddt[a][b];}, [](const Factor&){return 0;},[&](){check();});
    }
    std::vector<Component> components(const Domains& d,Num& constant)const{
        std::vector<int> parent(nv);std::iota(parent.begin(),parent.end(),0);
        auto root=[&](int v){while(parent[v]!=v)v=parent[v];return v;};std::vector<Factor> remaining;
        for(const auto& f:factors){auto g=reduce(f,d);if(g.scope.empty()){constant*=std::uint32_t(ddt[g.a][g.b]);continue;}
            for(int v:g.scope)parent[root(v)]=root(g.scope[0]);
            remaining.push_back(std::move(g));}
        std::map<int,Component> cc;
        for(int v=0;v<nv;++v)if(d[v].count()>1)cc[root(v)].vars.push_back(v);
        for(auto& f:remaining)cc[root(f.scope[0])].factors.push_back(std::move(f));
        std::vector<Component> out;for(auto& p:cc){if(p.second.factors.empty()){for(int v:p.second.vars)constant*=std::uint32_t(d[v].count());}
            else out.push_back(std::move(p.second));}return out;
    }
    Plan plan(const Component& c,const Domains& d)const{
        auto size=[&](const std::vector<int>& s){std::uint64_t n=1;for(int v:s)n=times(n,d[v].count());return n;};
        std::vector<Work> live;for(const auto& f:c.factors)live.push_back({f.scope,false});
        std::set<int> todo(c.vars.begin(),c.vars.end());Plan p;std::uint64_t cells=0,worst=0;
        while(!todo.empty()){
            check();
            std::uint64_t best=SAT,best_out=SAT;int chosen=-1;std::vector<int> selected;
            for(int v:todo){std::set<int> union_;std::uint64_t degree=0;for(const auto& t:live)if(std::binary_search(t.scope.begin(),t.scope.end(),v)){union_.insert(t.scope.begin(),t.scope.end());++degree;}
                std::vector<int> s(union_.begin(),union_.end());auto ops=times(size(s),degree);auto out=size(s)/d[v].count();
                if(chosen<0||std::make_pair(ops,out)<std::make_pair(best,best_out)){best=ops;best_out=out;chosen=v;selected=std::move(s);}}
            if(chosen<0)throw std::runtime_error("invalid elimination plan");
            if(best>worst){worst=best;p.critical=selected;}
            auto outscope=selected;outscope.erase(std::find(outscope.begin(),outscope.end(),chosen));auto outsize=size(outscope);
            p.peak=std::max(p.peak,plus(cells,outsize));p.operations=plus(p.operations,best);
            std::vector<Work> next;for(auto& t:live)if(!std::binary_search(t.scope.begin(),t.scope.end(),chosen))next.push_back(std::move(t));
            next.push_back({outscope,true});live=std::move(next);cells=0;
            for(const auto& t:live)if(t.dense)cells=plus(cells,size(t.scope));
            todo.erase(chosen);p.order.push_back(chosen);
        }
        double best=-1;for(int v:p.critical){int degree=0;for(const auto& f:c.factors)if(std::binary_search(f.scope.begin(),f.scope.end(),v))++degree;
            double score=double(degree*degree)/double(d[v].count());if(score>best){best=score;p.branch=v;}}
        return p;
    }
    std::string old_key_unused(const Component& c,const Domains& d)const{
        std::string k;auto put=[&](int x){k.push_back(char(x&255));k.push_back(char((x>>8)&255));};
        put(int(c.vars.size()));for(int v:c.vars){put(v);k+=hex(d[v]);}
        put(int(c.factors.size()));for(const auto& f:c.factors){put(f.id);put(f.a);put(f.b);put(int(f.av.size()));for(int v:f.av)put(v);put(int(f.bv.size()));for(int v:f.bv)put(v);}return k;
    }
    factor_sum::Domains explicit_domains(const Domains&d)const{factor_sum::Domains out;for(const auto&m:d)out.push_back(values(m));return out;}
    std::string key(const Component&c,const Domains&d)const{
        auto dom=explicit_domains(d);std::vector<std::string> inputs;
        for(const auto&f:c.factors)inputs.push_back(native_memo::factor_key(f,dom,[](const Factor&){return 0;},"skinny128"));
        return "component/"+native_memo::message_key(inputs,c.vars,dom);
    }
    Num contract(const Component& c,const Domains& d,const Plan& p){
        check();std::vector<std::vector<int>> dom(nv);std::vector<int> x(nv),pos(nv);for(int v:c.vars)dom[v]=values(d[v]);
        auto size=[&](const std::vector<int>& s){std::uint64_t n=1;for(int v:s)n=times(n,dom[v].size());return n;};
        auto decode=[&](std::size_t i,const std::vector<int>& s){for(auto it=s.rbegin();it!=s.rend();++it){int v=*it;pos[v]=int(i%dom[v].size());x[v]=dom[v][std::size_t(pos[v])];i/=dom[v].size();}};
        std::vector<Table> live;for(const auto& f:c.factors){Table t;t.scope=f.scope;t.factor=f;t.semantic=native_memo::factor_key(f,dom,[](const Factor&){return 0;},"skinny128");live.push_back(std::move(t));}std::uint64_t cells=0;
        for(int v:p.order){check();std::vector<int> bucket;std::set<int> vars;
            for(std::size_t j=0;j<live.size();++j)if(std::binary_search(live[j].scope.begin(),live[j].scope.end(),v)){bucket.push_back(int(j));vars.insert(live[j].scope.begin(),live[j].scope.end());}
            vars.erase(v);Table out;out.scope.assign(vars.begin(),vars.end());out.dense=true;auto n=size(out.scope);
            if(times(plus(cells,n),cell_bytes)>table_bytes)throw std::runtime_error("LEAF_MEMORY_LIMIT");
            std::vector<std::string> ancestry;for(int id:bucket)ancestry.push_back(live[id].semantic);
            out.semantic=native_memo::message_key(ancestry,out.scope,dom,v);
            bool memoized=native_memo::get(out.semantic,std::size_t(n),out.data);
            if(memoized)++cache_hits;else out.data.resize(std::size_t(n));
            struct Projection{int id;std::size_t step=0;std::vector<std::pair<int,std::size_t>> rest;};std::vector<Projection> projections;std::vector<int> lazy;
            for(int j:bucket){if(!live[j].dense){lazy.push_back(j);continue;}Projection r;r.id=j;std::size_t stride=1;for(auto it=live[j].scope.rbegin();it!=live[j].scope.rend();++it){if(*it==v)r.step=stride;else r.rest.push_back({*it,stride});stride*=dom[*it].size();}projections.push_back(std::move(r));}
            std::vector<std::size_t> offsets(projections.size());
            for(std::size_t i=0;!memoized&&i<out.data.size();++i){if(!(i&255))check();decode(i,out.scope);
                for(std::size_t j=0;j<projections.size();++j){offsets[j]=0;for(auto r:projections[j].rest)offsets[j]+=std::size_t(pos[r.first])*r.second;}
                Num sum;
                for(std::size_t k=0;k<dom[v].size();++k){x[v]=dom[v][k];Num product(1);
                    for(int j:lazy){int q=value(live[j].factor,x);if(!q){product=Num();break;}product*=std::uint32_t(q);}
                    if(!product.zero())for(std::size_t j=0;j<projections.size();++j){const Num& q=live[projections[j].id].data[offsets[j]+k*projections[j].step];if(q.zero()){product=Num();break;}product*=q;}
                    sum+=product;}
                out.data[i]=std::move(sum);
            }
            if(!memoized)native_memo::put(out.semantic,out.data);
            std::vector<Table> next;for(std::size_t j=0;j<live.size();++j){if(std::binary_search(bucket.begin(),bucket.end(),int(j))){if(live[j].dense)cells-=live[j].data.size();}else next.push_back(std::move(live[j]));}
            cells=plus(cells,n);next.push_back(std::move(out));live=std::move(next);
        }
        Num z(1);for(const auto& t:live){if(!t.dense||!t.scope.empty()||t.data.size()!=1)throw std::runtime_error("unfinished contraction");z*=t.data[0];}
        kernel_operations=plus(kernel_operations,p.operations);return z;
    }
    void remember(const std::string& k,const Num& z){
        native_memo::put(k,std::vector<Num>{z});
        auto bytes=k.size()+z.bytes()+128;if(bytes>table_bytes/8)return;
        if(cache_bytes+bytes>std::min<std::uint64_t>(64*1024*1024,table_bytes/8)){cache.clear();cache_bytes=0;}
        if(cache.emplace(k,z).second)cache_bytes+=bytes;
    }
    void json_box(std::ostream& o,const Box& b)const{bool first=true;o<<'[';for(const auto& p:b){if(!first)o<<',';first=false;o<<'['<<p.first<<",\""<<hex(p.second)<<"\"]";}o<<']';}
    std::string snapshot(const std::string& status,const std::string& reason)const{
        Num closed=initial_volume;for(const auto& b:queue)closed-=volume(b);double cpu=0;long rss=0;
#ifdef __linux__
        rusage u{};if(!getrusage(RUSAGE_SELF,&u)){cpu=u.ru_utime.tv_sec+u.ru_stime.tv_sec+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)/1e6;rss=u.ru_maxrss;}
#endif
        std::ostringstream o;o<<"{\"status\":\""<<status<<"\",\"reason\":\""<<reason<<"\",\"numerator\":\""<<accumulated.str()<<"\",\"denominator_power\":"<<7*nf
          <<",\"closed_volume\":\""<<closed.str()<<"\",\"initial_volume\":\""<<initial_volume.str()<<"\",\"frontier\":[";
        for(std::size_t i=0;i<queue.size();++i){if(i)o<<',';json_box(o,queue[i]);}
        o<<"],\"solved_nodes\":"<<nodes<<",\"zero_nodes\":"<<zeros<<",\"splits\":"<<splits<<",\"component_cache_hits\":"<<cache_hits
         <<",\"shared_message_hits\":"<<mass_messages::cache().hits<<",\"shared_message_stores\":"<<mass_messages::cache().stores<<",\"kernel_operation_score\":"<<kernel_operations<<",\"cpu_seconds\":"<<cpu<<",\"peak_rss_kib\":"<<rss
         <<",\"wall_seconds\":"<<std::chrono::duration<double>(Clock::now()-start).count()<<'}';return o.str();
    }
    void checkpoint(const std::string& status="EXACT_PROGRESS",const std::string& reason="CHECKPOINT"){
        auto text=snapshot(status,reason);auto tmp=output+".tmp";{std::ofstream f(tmp,std::ios::binary);f<<text<<'\n';f.flush();if(!f)throw std::runtime_error("checkpoint write failed");}
        if(std::rename(tmp.c_str(),output.c_str())!=0)throw std::runtime_error("checkpoint rename failed");
        last_checkpoint=Clock::now();
    }
    std::string inspect(){
        Domains d=base;for(const auto& p:queue.back())d[p.first]=p.second;
        if(!propagate(d))throw std::runtime_error("root eliminated legal witness");
        Num constant(1);auto cc=components(d,constant);
        std::uint64_t work=0,peak=0;int branch=-1;std::uint64_t worst=0;for(const auto& c:cc){auto p=plan(c,d);work=plus(work,p.operations);peak=std::max(peak,p.peak);if(p.operations>worst){worst=p.operations;branch=p.branch;}}
        double cpu=0;
#ifdef __linux__
        rusage u{};if(!getrusage(RUSAGE_SELF,&u))cpu=u.ru_utime.tv_sec+u.ru_stime.tv_sec+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)/1e6;
#endif
        std::ostringstream o;o<<"{\"status\":\"PLAN\",\"cpu_seconds\":"<<cpu<<",\"operation_score\":"<<work<<",\"estimated_table_bytes\":"<<times(peak,cell_bytes)
          <<",\"initial_volume\":\""<<initial_volume.str()<<"\",\"cell_bytes\":"<<cell_bytes<<",\"branch\":"<<branch<<",\"components\":"<<cc.size()
          <<",\"fits_leaf_budget\":"<<((work<=leaf_limit&&times(peak,cell_bytes)<=table_bytes)?"true":"false")<<'}';return o.str();
    }
    std::string run(){
        checkpoint();std::string reason="CHUNK_COMPLETE";
        try{while(!queue.empty()){

            check();const Box box=queue.back();Domains d=base;for(const auto& p:box)d[p.first]=p.second;
            bool legal=propagate(d);Num mass(1);int branch=-1;std::vector<Component> cc;std::vector<Plan> pp;std::vector<std::string> keys;
            if(legal){cc=components(d,mass);std::uint64_t worst=0;
                for(const auto& c:cc){auto k=key(c,d);std::vector<Num> reused;if(!cache.count(k)&&native_memo::get(k,1,reused))remember(k,reused[0]);auto hit=cache.find(k);if(hit!=cache.end()){mass*=hit->second;++cache_hits;pp.push_back(Plan());keys.push_back(std::move(k));continue;}
                    auto p=plan(c,d);if((p.operations>leaf_limit||times(p.peak,cell_bytes)>table_bytes)&&p.operations>=worst){worst=p.operations;branch=p.branch;}
                    keys.push_back(std::move(k));pp.push_back(std::move(p));}
                if(mass.zero()){legal=false;branch=-1;}
            }
            if(legal&&branch<0){for(std::size_t i=0;i<cc.size();++i){if(pp[i].order.empty())continue;
                auto z=contract(cc[i],d,pp[i]);remember(keys[i],z);mass*=z;if(mass.zero())break;}}
            if(!legal||branch<0){
                if(!legal)mass=Num();
                Num next=accumulated;next+=mass;
                accumulated=std::move(next);queue.pop_back();++nodes;if(mass.zero())++zeros;
            }
            else{
                if(queue.size()+1>max_frontier){reason="FRONTIER_READY";break;}
                auto list=values(d[branch]);if(list.size()<2)throw std::runtime_error("invalid adaptive branch");int x=integer_graph::branch_value(branch,explicit_domains(d),factors,witness,[&](int,int a,int b){return ddt[a][b];},[](const Factor&){return 0;},[&](){check();});
                Mask chosen;chosen.set(x);Mask rest=d[branch];rest.reset(x);Box left=box,right=box;left[branch]=chosen;right[branch]=rest;
                queue.back()=std::move(right);queue.push_back(std::move(left));++splits;
            }
            if(std::chrono::duration<double>(Clock::now()-last_checkpoint).count()>=.75)checkpoint();
        }}catch(const std::exception& e){
            auto why=std::string(e.what());
            if(why=="CHUNK_TIME_LIMIT")reason=stopped?"STOP":"CHUNK_TIME_LIMIT";
            else if(why=="LEAF_MEMORY_LIMIT"||why=="std::bad_alloc")reason="LEAF_MEMORY_LIMIT";
            else throw;
        }
        checkpoint(queue.empty()?"EXACT_DONE":"EXACT_PROGRESS",reason);return snapshot(queue.empty()?"EXACT_DONE":"EXACT_PROGRESS",reason);
    }
};
inline int main_entry(int argc,char** argv){
    try{
        if(argc!=10)throw std::runtime_error("usage: exact MODE MODEL CHECKPOINT ADDRESS_BYTES TABLE_BYTES LEAF_OPS SECONDS MAX_FRONTIER REVISION");
        std::string mode=argv[1];auto address=std::stoull(argv[4]),tables=std::stoull(argv[5]),ops=std::stoull(argv[6]);double sec=std::stod(argv[7]);auto frontier=std::stoull(argv[8]);
        if(std::string(argv[9])!="2"||(mode!="plan"&&mode!="eval")||address<tables||tables<1048576||!ops||!(sec>0)||sec>86400||frontier<2||frontier>1024)
            throw std::runtime_error("invalid adaptive budget");
#ifdef __linux__
        rlimit lim{};if(getrlimit(RLIMIT_AS,&lim))throw std::runtime_error("getrlimit failed");lim.rlim_cur=std::min<rlim_t>(address,lim.rlim_max);if(setrlimit(RLIMIT_AS,&lim))throw std::runtime_error("setrlimit failed");
#endif
        std::signal(SIGTERM,on_signal);std::signal(SIGINT,on_signal);
        Engine e(argv[2],argv[3],tables,ops,sec,std::size_t(frontier));
        std::cout<<(mode=="plan"?e.inspect():e.run())<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
}
