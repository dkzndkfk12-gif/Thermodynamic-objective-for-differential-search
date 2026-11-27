#pragma once
#include "twine.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>

namespace twine {
struct DSU {
    std::vector<int> p;
    explicit DSU(int n):p(n) { std::iota(p.begin(),p.end(),0); }
    int root(int a) { return p[a]==a?a:p[a]=root(p[a]); }
    void join(int a,int b) { a=root(a);b=root(b);if(a!=b)p[b]=a; }
};
struct FactorTable { int arity=0;std::vector<uint8_t> count;std::vector<uint16_t> supported; };
inline std::shared_ptr<const FactorTable> factor_table(std::array<int,3> which,int arity,bool physical) {
    int key=physical?256:0;for(int i=0;i<3;++i)key|=(which[i]+1)<<(2*i);
    static std::mutex mutex;static std::map<int,std::shared_ptr<const FactorTable>> cache;
    std::lock_guard<std::mutex> lock(mutex);auto it=cache.find(key);if(it!=cache.end())return it->second;
    auto t=std::make_shared<FactorTable>();t->arity=arity;t->count.resize(size_t(1)<<(4*arity));
    for(size_t code=0;code<t->count.size();++code) {
        std::array<int,3> v{};bool permitted=true;
        for(int i=0;i<arity;++i){v[i]=int((code>>(4*i))&15);permitted&=physical||v[i]!=0;}
        auto get=[&](int k){return which[k]<0?0:v[which[k]];};
        int n=ddt().n[get(0)][get(1)^get(2)];t->count[code]=uint8_t(n);
        if(n && permitted)t->supported.push_back(uint16_t(code));
    }
    cache[key]=t;return t;
}
struct Factor {
    int round=0,cell=0;
    std::vector<int> scope;
    std::shared_ptr<const FactorTable> table;
    int count(const Values& x) const { unsigned code=0;for(size_t k=0;k<scope.size();++k)code|=unsigned(x[scope[k]])<<(4*k);return table->count[code]; }
};
struct Model {
    int rounds=0, ne=0, nv=0, active=0;
    bool physical=false;
    Pattern pattern;
    std::string id;
    std::vector<std::array<int,16>> coordinates;
    Domains domains;
    std::vector<Factor> factors;
    std::vector<std::vector<int>> incident;

    Model(int r,Pattern p={},bool full=false):rounds(r),physical(full),pattern(std::move(p)),coordinates(r+1) {
        require(r>=1 && (full||pattern.size()==size_t(r+1)),"bad model dimensions");
        if(!full)for(auto m:pattern)require(m!=0,"zero difference pattern");
        id=full?"all-patterns":pattern_key(pattern);DSU d((r+1)*16);
        for(int k=0;k<r;++k)for(int j=0;j<8;++j) {
            d.join(k*16+2*j,(k+1)*16+PI[2*j]);
            if(!full && !(pattern[k]&(1u<<(2*j))))d.join(k*16+2*j+1,(k+1)*16+PI[2*j+1]);
        }
        std::map<int,bool> activity_by_root;
        if(!full)for(int k=0;k<=r;++k)for(int c=0;c<16;++c) {
            int q=d.root(16*k+c);bool a=pattern[k]&(1u<<c);auto [it,ok]=activity_by_root.emplace(q,a);
            require(ok||it->second==a,"incompatible pass-through/inactive activity");
        }
        std::map<int,int> ids;
        auto identify=[&](int k,int c) {
            int q=d.root(k*16+c);if(!full&&!activity_by_root.at(q))return -1;
            auto [it,inserted]=ids.emplace(q,int(ids.size()));(void)inserted;return it->second;
        };
        for(int k:{0,r})for(int c=0;c<16;++c)coordinates[k][c]=identify(k,c);
        ne=int(ids.size());
        for(int k=1;k<r;++k)for(int c=0;c<16;++c)coordinates[k][c]=identify(k,c);
        nv=int(ids.size());domains.assign(nv,full?0xFFFF:0xFFFE);incident.resize(nv);
        for(int k=0;k<r;++k)for(int j=0;j<8;++j) {
            if(!full && !(pattern[k]&(1u<<(2*j))))continue;
            Factor f;f.round=k;f.cell=j;
            std::array<int,3> v{coordinates[k][2*j],coordinates[k][2*j+1],coordinates[k+1][PI[2*j+1]]};
            for(int q:v)if(q>=0)f.scope.push_back(q);
            std::sort(f.scope.begin(),f.scope.end());f.scope.erase(std::unique(f.scope.begin(),f.scope.end()),f.scope.end());
            std::array<int,3> which{};for(int z=0;z<3;++z)which[z]=v[z]<0?-1:int(std::lower_bound(f.scope.begin(),f.scope.end(),v[z])-f.scope.begin());
            f.table=factor_table(which,int(f.scope.size()),full);
            for(int q:f.scope)incident[q].push_back(int(factors.size()));
            factors.push_back(std::move(f));
        }
        active=int(factors.size());
    }
    Trail decode(const Values& x) const {
        require(x.size()==size_t(nv),"wrong assignment size");Trail t(rounds+1);
        for(int r=0;r<=rounds;++r)for(int c=0;c<16;++c){int v=coordinates[r][c];t[r][c]=v<0?0:x[v];}
        return t;
    }
    Values encode(const Trail& t) const {
        require(t.size()==size_t(rounds+1),"wrong trail length");Values x(nv,255);
        for(int r=0;r<=rounds;++r)for(int c=0;c<16;++c) {
            int v=coordinates[r][c];if(v<0){require(t[r][c]==0,"inactive nibble is nonzero");continue;}
            require(domains[v]&(1u<<t[r][c]),"value outside model domain");require(x[v]==255||x[v]==t[r][c],"linear equalities fail");x[v]=t[r][c];
        }
        return x;
    }
    bool endpoints_nonzero(const Values& x) const {
        for(int r:{0,rounds}) { bool nz=false;for(int v:coordinates[r])nz|=v>=0 && x[v]!=0;if(!nz)return false; }return true;
    }
    bool legal(const Values& x) const {
        if(x.size()!=size_t(nv)||!endpoints_nonzero(x))return false;
        for(int v=0;v<nv;++v)if(x[v]>=16||!(domains[v]&(1u<<x[v])))return false;
        for(const auto& f:factors)if(!f.count(x))return false;
        return true;
    }
    std::vector<int> all_factors() const { std::vector<int> x(factors.size());std::iota(x.begin(),x.end(),0);return x; }
};
inline bool propagate(const Model& m,Domains& dom,const std::vector<int>& selected) {
    std::vector<uint8_t> allowed(m.factors.size()),queued(m.factors.size());std::vector<int> queue=selected;
    for(int f:selected)allowed[f]=queued[f]=1;
    for(size_t cursor=0;cursor<queue.size();++cursor) {
        int id=queue[cursor];queued[id]=0;const auto& f=m.factors[id];std::array<unsigned,3> support{};
        for(auto code:f.table->supported) {
            bool ok=true;for(size_t k=0;k<f.scope.size();++k)if(!(dom[f.scope[k]]&(1u<<((code>>(4*k))&15)))){ok=false;break;}
            if(!ok)continue;
            bool full=true;for(size_t k=0;k<f.scope.size();++k){support[k]|=1u<<((code>>(4*k))&15);full&=support[k]==dom[f.scope[k]];}if(full)break;
        }
        for(size_t k=0;k<f.scope.size();++k) {
            int v=f.scope[k];if(!support[k])return false;
            if(support[k]!=dom[v]) { dom[v]=uint16_t(support[k]);for(int q:m.incident[v])if(allowed[q]&&!queued[q]){queued[q]=1;queue.push_back(q);} }
        }
    }return true;
}
inline int branch_variable(const Model& m,const Domains& d,const std::vector<int>& vars) {
    int best=-1;for(int v:vars)if(pop(d[v])>1 && (best<0||pop(d[v])<pop(d[best])||(pop(d[v])==pop(d[best])&&m.incident[v].size()>m.incident[best].size())))best=v;return best;
}
struct SearchFrame { Domains dom;bool prepared=false;int variable=-1;uint16_t remaining=0; };

struct WitnessFinder {
    const Model& m;std::vector<SearchFrame> stack;std::vector<int> factors,vars;Values answer;uint64_t nodes=0;
    explicit WitnessFinder(const Model& model):m(model),stack{{m.domains,false,-1,0}},factors(m.all_factors()),vars(m.nv) { std::iota(vars.begin(),vars.end(),0); }
    bool finished() const { return !answer.empty()||stack.empty(); }
    void step(RNG& rng) {
        if(finished())return;
        auto& f=stack.back();
        if(!f.prepared) {
            ++nodes;if(!propagate(m,f.dom,factors)){stack.pop_back();return;}
            f.variable=branch_variable(m,f.dom,vars);
            if(f.variable<0){Values x(m.nv);for(int v=0;v<m.nv;++v)x[v]=uint8_t(first(f.dom[v]));if(m.legal(x))answer=std::move(x);stack.pop_back();return;}
            f.remaining=f.dom[f.variable];f.prepared=true;
        }
        if(!f.remaining){stack.pop_back();return;}
        int k=draw(rng,pop(f.remaining)),value=-1;for(int a=0;a<16;++a)if(f.remaining&(1u<<a)){if(k--==0){value=a;break;}}
        f.remaining&=uint16_t(~(1u<<value));Domains d=f.dom;d[f.variable]=uint16_t(1u<<value);stack.push_back({std::move(d),false,-1,0});
    }
};
}
