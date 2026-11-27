#pragma once
#include "factor_sum.hpp"
#include <array>
namespace integer_graph {
using Fac=factor_sum::Factor<factor_sum::Integer>;
struct Graph {factor_sum::Domains domains;std::vector<Fac> factors;};
template<class Factors,class Entry,class Type>
Graph bytes(const std::vector<int>&vars,const factor_sum::Domains&dom,const Factors&factors,
            Entry entry,Type type,const std::string&cipher) {
    Graph g;std::map<int,int> local;for(int v:vars)if(dom[v].size()>1){local[v]=int(g.domains.size());g.domains.push_back(dom[v]);}
    for(const auto&f:factors){int a=f.a,b=f.b;std::vector<int> aa,bb;
        for(int v:f.av)if(dom[v].size()==1)a^=dom[v][0];else aa.push_back(local.at(v));
        for(int v:f.bv)if(dom[v].size()==1)b^=dom[v][0];else bb.push_back(local.at(v));
        std::set<int> scope(aa.begin(),aa.end());scope.insert(bb.begin(),bb.end());Fac t;t.scope.assign(scope.begin(),scope.end());const int ty=type(f);
        t.semantic="DDT_NORMALIZED_V1/"+cipher+"/"+std::to_string(ty)+"/"+std::to_string(a)+"/"+std::to_string(b)+"/"+factor_sum::ints(aa)+"/"+factor_sum::ints(bb);
        t.function=[a,b,aa,bb,ty,entry](const std::vector<int>&x){int u=a,v=b;for(int i:aa)u^=x[i];for(int i:bb)v^=x[i];return mpz_class(entry(ty,u,v));};g.factors.push_back(std::move(t));}

    for(int v=0;v<int(g.domains.size());++v){Fac t;t.scope={v};t.semantic="domain-unity";t.function=[](const std::vector<int>&){return mpz_class(1);};g.factors.push_back(std::move(t));}
    return g;
}
struct Bits {int constant=0;std::array<std::vector<int>,8> vars;};
inline int eval(const Bits&b,int bit,const std::vector<int>&x){int v=(b.constant>>bit)&1;for(int i:b.vars[bit])v^=x[i];return v;}
inline std::string form_key(const Bits&b){std::string s=std::to_string(b.constant)+":";for(const auto&v:b.vars)factor_sum::append(s,factor_sum::ints(v));return s;}
inline std::vector<int> scope(const Bits&a,const Bits&b,const std::vector<int>&bits){std::set<int>s;for(int k:bits){s.insert(a.vars[k].begin(),a.vars[k].end());s.insert(b.vars[k].begin(),b.vars[k].end());}return {s.begin(),s.end()};}

template<class Factors,class Permute,class SmallEntry,class Type>
Graph midori_bits(const std::vector<int>&vars,const factor_sum::Domains&dom,const Factors&factors,
                 Permute permute,SmallEntry small,Type type) {
    Graph g;std::map<int,std::array<int,8>> local;std::map<int,int> fixed;
    for(int v:vars){std::array<int,8> ids;ids.fill(-1);int base=dom[v][0],vary=0;for(int x:dom[v])vary|=base^x;
        fixed[v]=base&~vary;for(int k=0;k<8;++k)if(vary&(1<<k)){ids[k]=int(g.domains.size());g.domains.push_back({0,1});}local[v]=ids;
        Fac t;for(int id:ids)if(id>=0)t.scope.push_back(id);std::sort(t.scope.begin(),t.scope.end());auto allowed=dom[v];std::sort(allowed.begin(),allowed.end());
        t.semantic="MIDORI_BYTE_DOMAIN_V1/"+std::to_string(fixed[v])+"/"+factor_sum::ints(std::vector<int>(ids.begin(),ids.end()))+"/"+factor_sum::ints(allowed);
        const int b=fixed[v];t.function=[ids,b,allowed](const std::vector<int>&x){int y=b;for(int k=0;k<8;++k)if(ids[k]>=0)y|=x[ids[k]]<<k;return mpz_class(std::binary_search(allowed.begin(),allowed.end(),y)?1:0);};g.factors.push_back(std::move(t));}
    auto form=[&](int constant,const std::vector<int>&vs){Bits b;b.constant=constant;for(int v:vs){if(dom[v].size()==1){b.constant^=dom[v][0];continue;}b.constant^=fixed.at(v);for(int k=0;k<8;++k)if(local.at(v)[k]>=0)b.vars[k].push_back(local.at(v)[k]);}return b;};
    for(const auto&f:factors){Bits a=form(f.a,f.av),b=form(f.b,f.bv);int ty=type(f);

        for(const auto&form:{a,b}){Fac t;Bits empty;t.scope=scope(form,empty,{0,1,2,3,4,5,6,7});t.semantic="MIDORI_ACTIVE_FORM_V1/"+form_key(form);
            t.function=[form](const std::vector<int>&x){int v=0;for(int k=0;k<8;++k)v|=eval(form,k,x)<<k;return mpz_class(v?1:0);};g.factors.push_back(std::move(t));}
        for(int half=0;half<2;++half){std::vector<int> order;
            for(int k=0;k<4;++k){int target=4*half+k,original=-1;for(int bit=0;bit<8;++bit)if(permute(1<<bit,ty)&(1<<target))original=bit;
                if(original<0)throw std::runtime_error("invalid Midori bit permutation");
                order.push_back(original);}
            Fac t;t.scope=scope(a,b,order);t.semantic="MIDORI_SB1_DDT_OVER_TWO_V1/"+form_key(a)+"/"+form_key(b)+"/"+factor_sum::ints(order);
            t.function=[a,b,order,small](const std::vector<int>&x){int u=0,v=0;for(int k=0;k<4;++k){u|=eval(a,order[k],x)<<k;v|=eval(b,order[k],x)<<k;}return mpz_class(small(u,v));};g.factors.push_back(std::move(t));}
    }return g;
}
inline mpz_class contract(Graph g,const factor_sum::Plan&p,std::uint64_t cells,const std::function<void()>&check){
    auto r=factor_sum::eliminate(g.domains,std::move(g.factors),p,cells,check);
    if(!r.marginal.scope.empty()||r.marginal.data.size()!=1)throw std::runtime_error("incomplete integer contraction");
    return r.marginal.data[0];
}

template<class Factors,class Entry,class Type>
int branch_value(int variable,const factor_sum::Domains&dom,const Factors&factors,
                 const std::vector<int>&witness,Entry entry,Type type,const std::function<void()>&check) {
    const auto cap=mass_messages::setting("THERMO_BRANCH_WORK",65536);
    int fallback=std::find(dom[variable].begin(),dom[variable].end(),witness[variable])!=dom[variable].end()?witness[variable]:dom[variable][0];
    if(!cap)return fallback;
    std::set<int> candidates;std::vector<std::size_t> selected;
    for(std::size_t i=0;i<factors.size();++i){const auto&f=factors[i];if(std::find(f.av.begin(),f.av.end(),variable)!=f.av.end()||std::find(f.bv.begin(),f.bv.end(),variable)!=f.bv.end()){
        selected.push_back(i);for(int v:f.av)if(v!=variable&&dom[v].size()>1)candidates.insert(v);for(int v:f.bv)if(v!=variable&&dom[v].size()>1)candidates.insert(v);}}
    int neighbour=-1;std::vector<std::size_t> touched=selected;
    for(int v:candidates){auto list=selected;for(std::size_t i=0;i<factors.size();++i)if(std::find(list.begin(),list.end(),i)==list.end()){
            const auto&f=factors[i];if(std::find(f.av.begin(),f.av.end(),v)!=f.av.end()||std::find(f.bv.begin(),f.bv.end(),v)!=f.bv.end())list.push_back(i);}
        auto work=factor_sum::mul(factor_sum::mul(dom[variable].size(),dom[v].size()),list.size());
        if(work<=cap&&(neighbour<0||list.size()>touched.size())){neighbour=v;touched=std::move(list);}}
    if(neighbour<0)return fallback;
    std::vector<int>x(dom.size());for(std::size_t v=0;v<dom.size();++v)x[v]=std::find(dom[v].begin(),dom[v].end(),witness[v])!=dom[v].end()?witness[v]:dom[v][0];
    int best=fallback;mpz_class maximum=-1;
    for(int u:dom[variable]){check();x[variable]=u;mpz_class sum=0;
        for(int v:dom[neighbour]){x[neighbour]=v;mpz_class product=1;for(auto id:touched){const auto&f=factors[id];int a=f.a,b=f.b;for(int z:f.av)a^=x[z];for(int z:f.bv)b^=x[z];product*=entry(type(f),a,b);if(product==0)break;}sum+=product;}
        if(sum>maximum||(sum==maximum&&u==fallback)){maximum=sum;best=u;}}
    return best;
}
}
