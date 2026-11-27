#pragma once

#include "segment_graph.hpp"
#include <deque>
#include <sstream>
namespace unseen_mass {
using W=segment_mass::W;
using Q=mpq_class;
struct Region {std::string id;W in_mask=0,in_value=0,out_mask=0,out_value=0;};
inline W block_mask(unsigned bits){return bits==128?~W(0):(W(1)<<bits)-1;}
inline void validate_region(const segment_mass::Spec& s,const Region& r){
    W mask=block_mask(4*s.N);
    if((r.in_mask&~mask)||(r.out_mask&~mask)||(r.in_value&~r.in_mask)||(r.out_value&~r.out_mask))
        throw std::runtime_error("unseen region mask/value mismatch");
}
inline bool fixed_endpoints(const segment_mass::Spec& s,const Region& r){return r.in_mask==block_mask(4*s.N)&&r.out_mask==block_mask(4*s.N);}
inline std::vector<unsigned char> domains(const segment_mass::Spec& s,const Region& r){
    validate_region(s,r);unsigned bits=4*s.N;std::vector<unsigned char>d((s.R+1)*bits,3);
    for(unsigned b=0;b<bits;++b){if((r.in_mask>>b)&1)d[b]=1u<<unsigned((r.in_value>>b)&1);
        if((r.out_mask>>b)&1)d[s.R*bits+b]=1u<<unsigned((r.out_value>>b)&1);}
    return d;
}
struct BoundBudget:std::runtime_error{using std::runtime_error::runtime_error;};
struct BoundResult {Q upper=1;bool complete=false;bool relaxation_exact=false;std::uint64_t split_buckets=0,peak_bytes=0;};
struct Factor {std::vector<unsigned> vars;std::vector<Q> values;unsigned numerator_bits=1,denominator_bits=1;std::uint64_t bytes=0;};
struct BoundMemory {
    std::uint64_t limit,used=0,peak=0;
    explicit BoundMemory(std::uint64_t n):limit(n){}
    std::uint64_t acquire(unsigned scope,unsigned num,unsigned den){
        if(scope>=sizeof(std::size_t)*8-1)throw BoundBudget("bound table scope exceeds address space");
        std::uint64_t entries=std::uint64_t(1)<<scope;
        std::uint64_t cell=sizeof(Q)+64+8*((std::uint64_t(num)+63)/64+(std::uint64_t(den)+63)/64);
        if(entries>limit/std::max<std::uint64_t>(1,cell))throw BoundBudget("bound table exceeds memory lease");
        std::uint64_t n=entries*cell+scope*sizeof(unsigned)+128;
        if(used>limit||n>limit-used)throw BoundBudget("bound live tables exceed memory lease");
        used+=n;peak=std::max(peak,used);return n;
    }
    void release(std::uint64_t n){if(n>used)throw std::runtime_error("unseen bound memory accounting");used-=n;}
};
inline BoundResult bound(const segment_mass::Spec& s,const Region& r,unsigned width,std::uint64_t memory,const std::function<void()>& check){
    validate_region(s,r);BoundResult result;unsigned bits=4*s.N;
    if(r.in_mask==block_mask(bits)&&r.in_value==0){result.upper=0;result.complete=true;result.relaxation_exact=true;return result;}
    width=std::max(7u,width);if(width>24)throw BoundBudget("bound width exceeds supported table index budget");
    auto d=domains(s,r);BoundMemory mem(memory);std::vector<Factor> fs;
    for(unsigned round=0;round<s.R;++round)for(unsigned cell=0;cell<s.N;++cell){
        check();std::array<unsigned,8> all{};for(unsigned b=0;b<4;++b){all[b]=round*bits+4*cell+b;all[4+b]=(round+1)*bits+s.perm[4*cell+b];}
        Factor f;for(unsigned v:all)if(d[v]==3)f.vars.push_back(v);std::sort(f.vars.begin(),f.vars.end());
        f.numerator_bits=5;f.denominator_bits=5;f.bytes=mem.acquire(unsigned(f.vars.size()),5,5);f.values.resize(std::size_t(1)<<f.vars.size());
        for(std::size_t k=0;k<f.values.size();++k){unsigned a=0,b=0;
            for(unsigned t=0;t<8;++t){unsigned v=all[t],value;
                if(d[v]==3){auto pos=std::lower_bound(f.vars.begin(),f.vars.end(),v)-f.vars.begin();value=unsigned((k>>pos)&1);}else value=d[v]==2;
                if(t<4)a|=value<<t;else b|=value<<(t-4);}
            f.values[k]=Q(s.ddt[a][b],16);f.values[k].canonicalize();}
        fs.push_back(std::move(f));
    }
    std::vector<unsigned> order;

    for(unsigned round=1;round<s.R;++round)for(unsigned b=0;b<bits;++b)if(d[round*bits+b]==3)order.push_back(round*bits+b);
    const std::size_t sums=order.size();
    for(unsigned round:{0u,s.R})for(unsigned b=0;b<bits;++b)if(d[round*bits+b]==3)order.push_back(round*bits+b);
    for(std::size_t step=0;step<order.size();++step){
        check();unsigned v=order[step];bool sum=step<sums;std::vector<Factor> bucket,rest;
        for(auto& f:fs){if(std::binary_search(f.vars.begin(),f.vars.end(),v))bucket.push_back(std::move(f));else rest.push_back(std::move(f));}
        fs=std::move(rest);if(bucket.empty()){if(sum){Factor f;f.values={Q(2)};f.numerator_bits=2;f.bytes=mem.acquire(0,2,1);fs.push_back(std::move(f));}continue;}

        std::sort(bucket.begin(),bucket.end(),[](const Factor&a,const Factor&b){return a.vars.size()>b.vars.size();});
        struct Group{std::vector<unsigned> idx,vars;};std::vector<Group> groups;
        for(unsigned i=0;i<bucket.size();++i){bool placed=false;for(auto& g:groups){std::vector<unsigned> u;
                std::set_union(g.vars.begin(),g.vars.end(),bucket[i].vars.begin(),bucket[i].vars.end(),std::back_inserter(u));
                if(u.size()<=width+1){g.idx.push_back(i);g.vars=std::move(u);placed=true;break;}}
            if(!placed)groups.push_back({{i},bucket[i].vars});}
        if(groups.size()>1)++result.split_buckets;
        for(unsigned gi=0;gi<groups.size();++gi){
            check();const auto& g=groups[gi];Factor out;out.vars=g.vars;out.vars.erase(std::lower_bound(out.vars.begin(),out.vars.end(),v));
            out.numerator_bits=(sum&&gi==0)?1:0;out.denominator_bits=0;
            for(unsigned i:g.idx){out.numerator_bits+=bucket[i].numerator_bits;out.denominator_bits+=bucket[i].denominator_bits;}
            out.bytes=mem.acquire(unsigned(out.vars.size()),out.numerator_bits,out.denominator_bits);out.values.resize(std::size_t(1)<<out.vars.size());
            struct Projection{unsigned i;std::vector<int> p;};std::vector<Projection> projections;
            for(unsigned i:g.idx){Projection p{i,{}};for(unsigned fv:bucket[i].vars)p.p.push_back(fv==v?-1:int(std::lower_bound(out.vars.begin(),out.vars.end(),fv)-out.vars.begin()));projections.push_back(std::move(p));}
            for(std::size_t k=0;k<out.values.size();++k){if((k&255)==0)check();Q vals[2]={Q(1),Q(1)};
                for(unsigned x=0;x<2;++x)for(const auto& p:projections){std::size_t z=0;for(unsigned t=0;t<p.p.size();++t)z|=std::size_t(p.p[t]<0?x:unsigned((k>>p.p[t])&1))<<t;vals[x]*=bucket[p.i].values[z];}
                if(sum&&gi==0)out.values[k]=vals[0]+vals[1];else out.values[k]=std::max(vals[0],vals[1]);}
            fs.push_back(std::move(out));
        }
        for(const auto& f:bucket)mem.release(f.bytes);
    }
    Q u=1;for(const auto& f:fs){check();if(!f.vars.empty()||f.values.size()!=1)throw std::runtime_error("unseen bound uneliminated variable");u*=f.values[0];}

    result.upper=std::min(Q(1),u);result.complete=true;result.peak_bytes=mem.peak;
    result.relaxation_exact=result.split_buckets==0 && r.in_value!=0;
    return result;
}

struct Constraint {std::array<unsigned,8> vars{};std::vector<unsigned char> tuples;};
struct Change {unsigned var;unsigned char old;};
struct Frame {unsigned var;unsigned char remaining;std::size_t mark;};

struct Solver {
    const segment_mass::Spec& spec;Region region;std::vector<unsigned char>d;
    std::vector<Constraint> constraints;std::vector<std::vector<unsigned>> adjacent;
    std::vector<Change> undo;std::vector<Frame> stack;std::uint64_t nodes=0,total_trails=0;
    bool exhausted=false;Q fixed_mass=0;std::mt19937_64 rng;
    Solver(const segment_mass::Spec&s,const Region&r,std::uint64_t seed):spec(s),region(r),d(domains(s,r)),adjacent(d.size()),rng(seed){
        unsigned bits=4*s.N;for(unsigned round=0;round<s.R;++round)for(unsigned cell=0;cell<s.N;++cell){Constraint c;
            for(unsigned b=0;b<4;++b){c.vars[b]=round*bits+4*cell+b;c.vars[b+4]=(round+1)*bits+s.perm[4*cell+b];}
            for(unsigned a=0;a<16;++a)for(unsigned b=0;b<16;++b)if(s.ddt[a][b])c.tuples.push_back(static_cast<unsigned char>(a|(b<<4)));
            unsigned id=unsigned(constraints.size());for(unsigned v:c.vars)adjacent[v].push_back(id);constraints.push_back(std::move(c));}
    }
    void set(unsigned v,unsigned char mask){if(d[v]!=mask){undo.push_back({v,d[v]});d[v]=mask;}}
    void rollback(std::size_t mark){if(mark>undo.size())throw std::runtime_error("unseen rollback mark");while(undo.size()>mark){auto c=undo.back();undo.pop_back();d[c.var]=c.old;}}
    bool next(){while(!stack.empty()){auto& f=stack.back();rollback(f.mark);if(f.remaining){unsigned char bit=f.remaining&static_cast<unsigned char>(-int(f.remaining));f.remaining^=bit;set(f.var,bit);return true;}stack.pop_back();}exhausted=true;return false;}
    bool propagate(const std::function<void()>&check){
        std::deque<unsigned>queue;std::vector<unsigned char>queued(constraints.size(),1);for(unsigned i=0;i<constraints.size();++i)queue.push_back(i);
        std::uint64_t work=0;
        auto nonzero=[&](){unsigned candidates=0,last=0;bool fixed_one=false;for(unsigned v=0;v<4*spec.N;++v){if(d[v]==2)fixed_one=true;if(d[v]&2){++candidates;last=v;}}
            if(!candidates)return false;
            if(!fixed_one&&candidates==1&&d[last]!=2){set(last,2);for(unsigned c:adjacent[last])if(!queued[c]){queued[c]=1;queue.push_back(c);}}
            return true;};
        if(!nonzero())return false;
        while(!queue.empty()){if((++work&63)==0)check();unsigned ci=queue.front();queue.pop_front();queued[ci]=0;const auto& c=constraints[ci];std::array<unsigned char,8>sup{};bool any=false;
            for(unsigned t:c.tuples){bool valid=true;for(unsigned b=0;b<8;++b)if(!(d[c.vars[b]]&(1u<<((t>>b)&1)))){valid=false;break;}if(!valid)continue;any=true;for(unsigned b=0;b<8;++b)sup[b]|=1u<<((t>>b)&1);}
            if(!any)return false;
            for(unsigned b=0;b<8;++b){unsigned v=c.vars[b];unsigned char mask=d[v]&sup[b];if(!mask)return false;if(mask!=d[v]){set(v,mask);for(unsigned a:adjacent[v])if(!queued[a]){queued[a]=1;queue.push_back(a);}}}
            if(!nonzero())return false;
        }return true;
    }
    int choose(){
        int best=-1;unsigned score=0,ties=0;
        for(unsigned v=0;v<d.size();++v)if(d[v]==3){unsigned s=0;for(unsigned ci:adjacent[v])for(unsigned w:constraints[ci].vars)if(d[w]!=3)++s;
            if(best<0||s>score){best=int(v);score=s;ties=1;}else if(s==score){++ties;if(rng()%ties==0)best=int(v);}}
        return best;
    }
    std::vector<W> path()const{unsigned bits=4*spec.N;std::vector<W>p(spec.R+1,0);for(unsigned r=0;r<=spec.R;++r)for(unsigned b=0;b<bits;++b){auto v=d[r*bits+b];if(v!=1&&v!=2)throw std::runtime_error("unseen non-singleton witness");if(v==2)p[r]|=W(1)<<b;}return p;}

    bool leaf(std::vector<W>&p,const std::function<void()>&check){while(!exhausted){check();++nodes;
            if(!propagate(check)){next();continue;}int v=choose();if(v<0){p=path();segment_mass::Z z=1;
                for(unsigned r=0;r<spec.R;++r)z*=spec.coefficient(p[r],p[r+1]);
                if(z<=0||!p.front()||!p.back())throw std::runtime_error("unseen invalid generated witness");
                ++total_trails;if(fixed_endpoints(spec,region)){Q q(z,segment_mass::Z(1)<<(4*spec.N*spec.R));q.canonicalize();fixed_mass+=q;}
                next();return true;}
            std::size_t mark=undo.size();stack.push_back({unsigned(v),2,mark});set(unsigned(v),1);
        }return false;}
};
}
