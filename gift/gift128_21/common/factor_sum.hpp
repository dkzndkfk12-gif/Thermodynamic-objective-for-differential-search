#pragma once
#include "message_cache.hpp"
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <gmpxx.h>
namespace factor_sum {
inline constexpr std::uint64_t limit=UINT64_MAX/8;
inline std::uint64_t add(std::uint64_t a,std::uint64_t b){return b>limit-a?limit:a+b;}
inline std::uint64_t mul(std::uint64_t a,std::uint64_t b){return b&&a>limit/b?limit:a*b;}
using Domains=std::vector<std::vector<int>>;
inline std::uint64_t size(const std::vector<int>& s,const Domains& d){std::uint64_t n=1;for(int v:s)n=mul(n,d.at(v).size());return n;}
inline void append(std::string& s,const std::string& x){s+=std::to_string(x.size())+":"+x;}
inline std::string ints(const std::vector<int>& v){std::string s;for(int x:v)s+=std::to_string(x)+",";return s;}
struct Integer {
    using Value=mpz_class;
    static Value zero(){return 0;}static Value one(){return 1;}
    static Value plus(const Value&a,const Value&b){return a+b;}
    static Value times(const Value&a,const Value&b){return a*b;}
    static std::string tag(){return "integer-product-sum-v1";}
    static std::string encode(const std::vector<Value>& v){std::string s;for(const auto&x:v)s+=x.get_str()+"\n";return s;}
    static bool decode(const std::string&s,std::size_t n,std::vector<Value>&v){
        std::istringstream in(s);std::string x;v.clear();v.reserve(n);
        while(std::getline(in,x)){if(x.empty()||x.find_first_not_of("0123456789")!=std::string::npos||v.size()==n)return false;v.emplace_back(x);}
        return v.size()==n;
    }
};
struct Log {
    using Value=long double;
    static Value zero(){return -std::numeric_limits<Value>::infinity();}static Value one(){return 0;}
    static Value plus(Value a,Value b){if(a==zero())return b;if(b==zero())return a;auto hi=std::max(a,b);return hi+std::log1p(std::exp(std::min(a,b)-hi));}
    static Value times(Value a,Value b){return a+b;}
    static std::string tag(){return "log-product-sum-v1-"+std::to_string(sizeof(Value));}
    static std::string encode(const std::vector<Value>&v){return {reinterpret_cast<const char*>(v.data()),v.size()*sizeof(Value)};}
    static bool decode(const std::string&s,std::size_t n,std::vector<Value>&v){
        if(s.size()!=n*sizeof(Value))return false;
        v.resize(n);std::memcpy(v.data(),s.data(),s.size());
        for(auto x:v)if(std::isnan(x)||x==std::numeric_limits<Value>::infinity())return false;
        return true;
    }
};
template<class Ring> struct Factor {
    using V=typename Ring::Value;
    std::vector<int> scope;std::vector<V> data;std::function<V(const std::vector<int>&)> function;
    std::string semantic;
    bool dense=false;
};
struct Plan {std::vector<int> order;std::uint64_t operations=0,peak=0;};
template<class Ring> Plan plan(const Domains&d,const std::vector<Factor<Ring>>&f,
                              const std::set<int>&keep,const std::function<void()>&check) {
    struct Work {std::vector<int> scope;bool dense;};
    std::vector<Work> live;for(const auto&t:f)live.push_back({t.scope,t.dense});
    std::set<int> todo;for(int v=0;v<int(d.size());++v)if(!keep.count(v))todo.insert(v);
    Plan p;while(!todo.empty()){
        check();int selected=-1;std::uint64_t best=limit,bestout=limit;std::vector<int> merged;
        for(int v:todo){std::set<int> s;std::uint64_t degree=0;
            for(const auto&t:live)if(std::binary_search(t.scope.begin(),t.scope.end(),v)){s.insert(t.scope.begin(),t.scope.end());++degree;}
            if(!degree)s.insert(v);
            std::vector<int> sc(s.begin(),s.end());auto n=size(sc,d);auto cost=mul(n,std::max<std::uint64_t>(1,degree));
            auto out=n==limit?limit:n/d[v].size();
            if(selected<0||std::make_pair(cost,out)<std::make_pair(best,bestout)){selected=v;best=cost;bestout=out;merged=std::move(sc);}}
        std::uint64_t resident=0;for(const auto&t:live)if(t.dense)resident=add(resident,size(t.scope,d));
        p.peak=std::max(p.peak,add(resident,bestout));p.operations=add(p.operations,best);
        merged.erase(std::find(merged.begin(),merged.end(),selected));
        std::vector<Work> next;for(auto&t:live)if(!std::binary_search(t.scope.begin(),t.scope.end(),selected))next.push_back(std::move(t));
        next.push_back({std::move(merged),true});live=std::move(next);p.order.push_back(selected);todo.erase(selected);
    }
    return p;
}
template<class Ring> struct Trace {int variable;std::vector<int> scope;std::vector<typename Ring::Value> joint;};
template<class Ring> struct Result {Factor<Ring> marginal;std::vector<Trace<Ring>> trace;};
inline std::size_t index(const std::vector<int>&scope,const std::vector<int>&positions,const Domains&d){std::size_t n=0;for(int v:scope)n=n*d[v].size()+std::size_t(positions[v]);return n;}
inline void decode(std::size_t n,const std::vector<int>&scope,const Domains&d,std::vector<int>&positions,std::vector<int>&x){
    for(auto it=scope.rbegin();it!=scope.rend();++it){auto v=*it;positions[v]=int(n%d[v].size());x[v]=d[v][positions[v]];n/=d[v].size();}
}

template<class Ring> Result<Ring> eliminate(const Domains&d,std::vector<Factor<Ring>> live,const Plan&p,
        std::uint64_t maxcells,const std::function<void()>&check,bool traceback=false) {
    using V=typename Ring::Value;std::vector<int> pos(d.size()),x(d.size());
    for(std::size_t i=0;i<d.size();++i){if(d[i].empty())throw std::runtime_error("empty contraction domain");x[i]=d[i][0];}
    for(auto&f:live){if(!std::is_sorted(f.scope.begin(),f.scope.end()))throw std::runtime_error("unsorted factor scope");
        std::string s=Ring::tag();append(s,f.semantic);for(int v:f.scope){append(s,std::to_string(v));append(s,ints(d[v]));}f.semantic=std::move(s);}
    Result<Ring> answer;std::uint64_t tracecells=0;
    for(int v:p.order){check();std::vector<std::size_t> bucket;std::set<int> vars;
        for(std::size_t i=0;i<live.size();++i)if(std::binary_search(live[i].scope.begin(),live[i].scope.end(),v)){bucket.push_back(i);vars.insert(live[i].scope.begin(),live[i].scope.end());}
        vars.erase(v);Factor<Ring> out;out.scope.assign(vars.begin(),vars.end());out.dense=true;auto n=size(out.scope,d);
        std::uint64_t resident=tracecells;for(const auto&t:live)if(t.dense)resident=add(resident,t.data.size());
        if(add(resident,add(n,traceback?mul(n,d[v].size()):0))>maxcells)throw std::runtime_error("LEAF_MEMORY_LIMIT");
        std::vector<std::string> ancestry;for(auto id:bucket)ancestry.push_back(live[id].semantic);std::sort(ancestry.begin(),ancestry.end());
        out.semantic=Ring::tag()+"/sum/"+std::to_string(v);append(out.semantic,ints(d[v]));for(const auto&s:ancestry)append(out.semantic,s);
        Trace<Ring> trace;trace.variable=v;trace.scope=out.scope;
        bool cached=false;std::string encoded;
        const std::string cachekey=out.semantic+(traceback?"/backward-trace":"/marginal");
        if(mass_messages::cache().get(cachekey,encoded)){
            std::vector<V> payload;cached=Ring::decode(encoded,std::size_t(n)*(traceback?1+d[v].size():1),payload);
            if(cached){out.data.assign(payload.begin(),payload.begin()+std::size_t(n));if(traceback)trace.joint.assign(payload.begin()+std::size_t(n),payload.end());}
        }
        if(!cached){out.data.assign(std::size_t(n),Ring::zero());if(traceback)trace.joint.resize(std::size_t(mul(n,d[v].size())));
            for(std::size_t i=0;i<n;++i){if(!(i&255))check();decode(i,out.scope,d,pos,x);V sum=Ring::zero();
                for(std::size_t k=0;k<d[v].size();++k){pos[v]=int(k);x[v]=d[v][k];V product=Ring::one();
                    for(auto id:bucket){const auto&t=live[id];product=Ring::times(product,t.dense?t.data[index(t.scope,pos,d)]:t.function(x));}
                    sum=Ring::plus(sum,product);if(traceback)trace.joint[i*d[v].size()+k]=std::move(product);}
                out.data[i]=std::move(sum);}
            if(cachekey.size()<mass_messages::slot_bytes/2 && mul(n,traceback?1+d[v].size():1)<=mass_messages::slot_bytes/sizeof(V)){
                auto payload=out.data;if(traceback)payload.insert(payload.end(),trace.joint.begin(),trace.joint.end());mass_messages::cache().put(cachekey,Ring::encode(payload));
            }
        }
        if(traceback){tracecells=add(tracecells,trace.joint.size());answer.trace.push_back(std::move(trace));}
        std::vector<Factor<Ring>> next;for(std::size_t i=0;i<live.size();++i)if(!std::binary_search(bucket.begin(),bucket.end(),i))next.push_back(std::move(live[i]));
        next.push_back(std::move(out));live=std::move(next);
    }
    std::set<int> vars;for(const auto&t:live)vars.insert(t.scope.begin(),t.scope.end());answer.marginal.scope.assign(vars.begin(),vars.end());answer.marginal.dense=true;
    auto n=size(answer.marginal.scope,d);std::uint64_t resident=tracecells;for(const auto&t:live)if(t.dense)resident=add(resident,t.data.size());
    if(add(resident,n)>maxcells)throw std::runtime_error("LEAF_MEMORY_LIMIT");
    answer.marginal.data.resize(std::size_t(n));
    for(std::size_t i=0;i<n;++i){if(!(i&255))check();decode(i,answer.marginal.scope,d,pos,x);V product=Ring::one();
        for(const auto&t:live)product=Ring::times(product,t.dense?t.data[index(t.scope,pos,d)]:t.function(x));
        answer.marginal.data[i]=std::move(product);}
    return answer;
}
inline mpz_class parse_integer(const std::string&s){return mpz_class(s);}
template<class N> N convert(const mpz_class&z){N n;for(char c:z.get_str()){n*=std::uint32_t(10);n+=N(std::uint32_t(c-'0'));}return n;}
}
