#pragma once
#include "craft.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>

namespace craft {
struct DSU {
    std::vector<int> p;
    explicit DSU(int n):p(n){std::iota(p.begin(),p.end(),0);}
    int root(int a){return p[a]==a?a:p[a]=root(p[a]);}
    void join(int a,int b){a=root(a);b=root(b);if(a!=b)p[b]=a;}
};
using Expr=std::vector<int>;
inline uint8_t evaluate(const Expr& e,const Values& x){uint8_t a=0;for(int v:e)a^=x[v];return a;}
inline Expr xor_expr(const Expr& a,const Expr& b){Expr c;std::set_symmetric_difference(a.begin(),a.end(),b.begin(),b.end(),std::back_inserter(c));return c;}
struct Factor {
    int round=0,cell=0;
    bool weighted=true,physical=false;
    Expr a,b,scope;
    int count(const Values& x) const {
        int av=evaluate(a,x);if(!weighted)return av?16:0;
        int bv=evaluate(b,x);if(!physical&&(!av||!bv))return 0;
        return ddt().n[av][bv];
    }
};

struct CutBasis {
    std::array<uint16_t,16> expression{};
    std::vector<int> free_cells;
    explicit CutBasis(std::vector<uint16_t> equations){
        std::array<uint16_t,16> piv{};
        for(auto row:equations){for(int c=0;c<16;++c)if(row&(1u<<c)){if(piv[c])row^=piv[c];else{piv[c]=row;break;}}}
        for(int c=0;c<16;++c)if(!piv[c]){expression[c]=uint16_t(1u<<free_cells.size());free_cells.push_back(c);}
        for(int c=15;c>=0;--c)if(piv[c])for(int j=c+1;j<16;++j)if(piv[c]&(1u<<j))expression[c]^=expression[j];
    }
};
struct Model {
    int rounds=0,ne=0,nv=0,active=0;
    bool physical=false;
    Pattern pattern;
    std::string id;
    std::vector<std::array<Expr,16>> coordinates;
    std::vector<std::pair<int,int>> variable_cells;
    Domains domains;
    std::vector<Factor> factors;
    std::vector<std::vector<int>> incident;
    Model(int r,Pattern p={},bool full=false):rounds(r),physical(full),pattern(std::move(p)),coordinates(r+1){
        require(r>=1&&(full||pattern.size()==size_t(r+1)),"bad model dimensions");
        if(!full)for(auto mask:pattern)require(mask!=0,"zero difference pattern");
        id=full?"all-patterns":pattern_key(pattern);
        std::vector<int> order{0,r};for(int k=1;k<r;++k)order.push_back(k);
        for(int k:order){
            std::vector<uint16_t> eq;
            if(!full){
                for(int c=0;c<16;++c)if(!(pattern[k]&(1u<<c)))eq.push_back(uint16_t(1u<<c));
                if(k<r)for(int c=0;c<16;++c)if(!(pattern[k+1]&(1u<<c)))eq.push_back(linear_rows()[c]);
            }
            CutBasis basis(std::move(eq));int offset=nv;
            for(int c:basis.free_cells){variable_cells.emplace_back(k,c);++nv;}
            for(int c=0;c<16;++c)for(size_t j=0;j<basis.free_cells.size();++j)if(basis.expression[c]&(1u<<j))coordinates[k][c].push_back(offset+int(j));
            if(k==r)ne=nv;
        }
        domains.assign(nv,full?0xFFFF:0xFFFE);incident.resize(nv);
        auto add=[&](Factor f){
            f.scope=f.a;f.scope.insert(f.scope.end(),f.b.begin(),f.b.end());std::sort(f.scope.begin(),f.scope.end());
            require(std::adjacent_find(f.scope.begin(),f.scope.end())==f.scope.end(),"factor sides overlap");
            for(int v:f.scope)incident[v].push_back(int(factors.size()));
            active+=f.weighted;factors.push_back(std::move(f));
        };
        for(int k=0;k<r;++k)for(int c=0;c<16;++c){
            if(!full&&!(pattern[k+1]&(1u<<c)))continue;
            Factor f;f.round=k;f.cell=c;f.physical=full;f.b=coordinates[k+1][c];
            for(int j=0;j<16;++j)if(linear_rows()[c]&(1u<<j))f.a=xor_expr(f.a,coordinates[k][j]);
            require(full||(!f.a.empty()&&!f.b.empty()),"impossible activity pattern");add(std::move(f));
        }
        if(!full){
            std::set<Expr> seen;
            for(int k=0;k<=r;++k)for(int c=0;c<16;++c)if(pattern[k]&(1u<<c)){
                auto e=coordinates[k][c];require(!e.empty(),"active coordinate identically zero");

                if(e.size()>1&&seen.insert(e).second){Factor f;f.weighted=false;f.round=k;f.cell=c;f.a=std::move(e);add(std::move(f));}
            }
        }
    }
    Trail decode(const Values& x) const {
        require(x.size()==size_t(nv),"wrong assignment size");Trail t(rounds+1);
        for(int r=0;r<=rounds;++r)for(int c=0;c<16;++c)t[r][c]=evaluate(coordinates[r][c],x);
        return t;
    }
    Values encode(const Trail& t) const {
        require(t.size()==size_t(rounds+1),"wrong trail length");Values x(nv);
        for(int v=0;v<nv;++v){auto [r,c]=variable_cells[v];x[v]=t[r][c];require(x[v]<16&&(domains[v]&(1u<<x[v])),"value outside model domain");}
        require(decode(x)==t,"linear activity equations fail");
        if(!physical)require(activity(t)==pattern,"wrong physical activity pattern");
        return x;
    }
    bool endpoints_nonzero(const Values& x) const {
        for(int r:{0,rounds}){bool nz=false;for(const auto& e:coordinates[r])nz|=evaluate(e,x)!=0;if(!nz)return false;}return true;
    }
    bool legal(const Values& x) const {
        if(x.size()!=size_t(nv))return false;
        for(int v=0;v<nv;++v)if(x[v]>=16||!(domains[v]&(1u<<x[v])))return false;
        if(!endpoints_nonzero(x))return false;
        for(const auto& f:factors)if(!f.count(x))return false;
        return true;
    }
    std::vector<int> all_factors() const {std::vector<int> ids(factors.size());std::iota(ids.begin(),ids.end(),0);return ids;}
};
inline uint16_t xor_sets(uint16_t a,uint16_t b){
    if(!a||!b)return 0;
    if(a==0xFFFF||b==0xFFFF)return 0xFFFF;
    uint16_t c=0;for(unsigned aa=a;aa;aa&=aa-1){int i=first(aa);for(unsigned bb=b;bb;bb&=bb-1)c|=uint16_t(1u<<(i^first(bb)));}return c;
}
inline uint16_t possible_xors(const Expr& vars,const Domains& dom,int omit=-1){uint16_t s=1;for(int v:vars)if(v!=omit)s=xor_sets(s,dom[v]);return s;}

inline bool propagate(const Model& m,Domains& dom,const std::vector<int>& selected){
    std::vector<uint8_t> allowed(m.factors.size()),queued(m.factors.size());std::vector<int> queue=selected;
    for(int f:selected)allowed[f]=queued[f]=1;
    for(size_t cursor=0;cursor<queue.size();++cursor){
        int id=queue[cursor];queued[id]=0;const auto& f=m.factors[id];
        uint16_t pa=possible_xors(f.a,dom),pb=possible_xors(f.b,dom),aa=0,bb=0;
        if(!f.weighted)aa=uint16_t(pa&0xFFFE);
        else for(unsigned a=pa;a;a&=a-1)for(unsigned b=pb;b;b&=b-1){int x=first(a),y=first(b);if((f.physical||(x&&y))&&ddt().n[x][y]){aa|=uint16_t(1u<<x);bb|=uint16_t(1u<<y);}}
        if(!aa||(f.weighted&&!bb))return false;
        auto prune=[&](const Expr& vars,uint16_t permitted){
            for(int v:vars){auto others=possible_xors(vars,dom,v);auto support=uint16_t(dom[v]&xor_sets(others,permitted));if(!support)return false;
                if(support!=dom[v]){dom[v]=support;for(int q:m.incident[v])if(allowed[q]&&!queued[q]){queued[q]=1;queue.push_back(q);}}}
            return true;
        };
        if(!prune(f.a,aa)||(f.weighted&&!prune(f.b,bb)))return false;
    }return true;
}
inline int branch_variable(const Model& m,const Domains& d,const std::vector<int>& vars){
    int best=-1;for(int v:vars)if(pop(d[v])>1&&(best<0||pop(d[v])<pop(d[best])||(pop(d[v])==pop(d[best])&&m.incident[v].size()>m.incident[best].size())))best=v;return best;
}
struct SearchFrame {Domains dom;bool prepared=false;int variable=-1;uint16_t remaining=0;};
struct WitnessFinder {
    const Model& m;std::vector<SearchFrame> stack;std::vector<int> factors,vars;Values answer;uint64_t nodes=0;
    explicit WitnessFinder(const Model& model):m(model),stack{{m.domains,false,-1,0}},factors(m.all_factors()),vars(m.nv){std::iota(vars.begin(),vars.end(),0);}
    bool finished() const {return !answer.empty()||stack.empty();}
    void step(RNG& rng){
        if(finished())return;
        auto& f=stack.back();
        if(!f.prepared){
            ++nodes;if(!propagate(m,f.dom,factors)){stack.pop_back();return;}
            f.variable=branch_variable(m,f.dom,vars);
            if(f.variable<0){Values x(m.nv);for(int v=0;v<m.nv;++v)x[v]=uint8_t(first(f.dom[v]));if(m.legal(x))answer=std::move(x);stack.pop_back();return;}
            f.remaining=f.dom[f.variable];f.prepared=true;
        }
        if(!f.remaining){stack.pop_back();return;}
        int k=draw(rng,pop(f.remaining)),value=-1;for(int a=0;a<16;++a)if(f.remaining&(1u<<a))if(k--==0){value=a;break;}
        f.remaining&=uint16_t(~(1u<<value));Domains d=f.dom;d[f.variable]=uint16_t(1u<<value);stack.push_back({std::move(d),false,-1,0});
    }
};
}
