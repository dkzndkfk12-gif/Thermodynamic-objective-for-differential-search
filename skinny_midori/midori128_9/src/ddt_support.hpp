
#pragma once
#include <array>
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <set>
#include <vector>
namespace ddt_support {
using Mask=std::bitset<256>;
using Domains=std::vector<Mask>;
struct Pairs {
    std::array<std::uint64_t,1024> w{};
    void set(int x){w[x>>6]|=std::uint64_t(1)<<(x&63);}
    Pairs& operator|=(const Pairs& b){for(unsigned i=0;i<w.size();++i)w[i]|=b.w[i];return *this;}
    bool intersects(const Pairs& b)const{for(unsigned i=0;i<w.size();++i)if(w[i]&b.w[i])return true;return false;}
    bool full()const{for(auto x:w)if(x!=~std::uint64_t(0))return false;return true;}
};
inline Pairs shift(const Pairs& a,unsigned value){
    constexpr std::uint64_t low[6]={0x5555555555555555ULL,0x3333333333333333ULL,
        0x0F0F0F0F0F0F0F0FULL,0x00FF00FF00FF00FFULL,0x0000FFFF0000FFFFULL,0x00000000FFFFFFFFULL};
    Pairs b;
    for(unsigned i=0;i<a.w.size();++i){auto x=a.w[i];
        for(unsigned k=0;k<6;++k)if(value&(1u<<k)){unsigned step=1u<<k;x=((x&low[k])<<step)|((x>>step)&low[k]);}
        b.w[i^(value>>6)]=x;
    }return b;
}
template<class Factors,class Entry,class Kind,class Check>
bool propagate(Domains& d,const Factors& factors,Entry entry,Kind kind,Check check){

    static const auto allowed=[&](){std::array<Pairs,4> out;
        for(int t=0;t<4;++t)for(int a=1;a<256;++a)for(int b=1;b<256;++b)
            if(entry(t,a,b))out[t].set(a|(b<<8));
        return out;}();
    bool changed=true;
    while(changed){changed=false;
        for(const auto& f:factors){check();
            std::set<int> scope(f.av.begin(),f.av.end());scope.insert(f.bv.begin(),f.bv.end());
            std::vector<int> vars(scope.begin(),scope.end()),roles;
            for(int v:vars)roles.push_back((std::find(f.av.begin(),f.av.end(),v)!=f.av.end()?1:0)+
                                          (std::find(f.bv.begin(),f.bv.end(),v)!=f.bv.end()?256:0));
            std::vector<Pairs> prefix(vars.size()+1);prefix[0].set(0);
            for(std::size_t j=0;j<vars.size();++j){
                if(d[vars[j]].none())return false;
                if(prefix[j].full()){prefix[j+1]=prefix[j];continue;}
                for(int x=1;x<256;++x)if(d[vars[j]][x]){
                    if((x&31)==0)check();
                    prefix[j+1]|=shift(prefix[j],roles[j]*x);
                    if(prefix[j+1].full())break;
                }
            }
            auto back=shift(allowed[kind(f)],f.a|(f.b<<8));
            if(!prefix.back().intersects(back))return false;
            for(std::size_t j=vars.size();j-- >0;){
                if(back.full())break;
                Mask keep;Pairs previous;
                for(int x=1;x<256;++x)if(d[vars[j]][x]){
                    if((x&31)==0)check();
                    auto possible=shift(back,roles[j]*x);
                    if(prefix[j].intersects(possible)){keep.set(x);previous|=possible;}
                }
                if(keep.none())return false;
                if(keep!=d[vars[j]]){d[vars[j]]=keep;changed=true;}
                back=previous;
            }
        }
    }return true;
}
}
