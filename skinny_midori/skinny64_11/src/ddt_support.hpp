

#pragma once
#include <array>
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <set>
#include <vector>
namespace ddt_support {
using Mask = std::bitset<16>;
using Domains = std::vector<Mask>;
struct Pairs {
    std::array<std::uint64_t,4> w{};
    void set(int x){w[x>>6]|=std::uint64_t(1)<<(x&63);}
    Pairs& operator|=(const Pairs& b){for(int i=0;i<4;++i)w[i]|=b.w[i];return *this;}
    bool intersects(const Pairs& b)const{for(int i=0;i<4;++i)if(w[i]&b.w[i])return true;return false;}
};
inline Pairs shift(Pairs a,unsigned value){
    constexpr std::uint64_t low[6]={0x5555555555555555ULL,0x3333333333333333ULL,
        0x0F0F0F0F0F0F0F0FULL,0x00FF00FF00FF00FFULL,0x0000FFFF0000FFFFULL,0x00000000FFFFFFFFULL};
    for(unsigned k=0;k<6;++k)if(value&(1u<<k)){unsigned step=1u<<k;
        for(auto& v:a.w)v=((v&low[k])<<step)|((v>>step)&low[k]);}
    if(value&64){std::swap(a.w[0],a.w[1]);std::swap(a.w[2],a.w[3]);}
    if(value&128){std::swap(a.w[0],a.w[2]);std::swap(a.w[1],a.w[3]);}
    return a;
}
template<class Factors,class Check>
bool propagate(Domains& d,const Factors& factors,const int (&ddt)[16][16],Check check){
    Pairs allowed;for(int a=0;a<16;++a)for(int b=0;b<16;++b)if(ddt[a][b])allowed.set(a|(b<<4));
    bool changed=true;
    while(changed){changed=false;
        for(const auto& f:factors){check();
            std::set<int> scope(f.av.begin(),f.av.end());scope.insert(f.bv.begin(),f.bv.end());
            std::vector<int> vars(scope.begin(),scope.end()),roles;
            for(int v:vars)roles.push_back((std::find(f.av.begin(),f.av.end(),v)!=f.av.end()?1:0)+
                                          (std::find(f.bv.begin(),f.bv.end(),v)!=f.bv.end()?16:0));
            std::vector<Pairs> prefix(vars.size()+1);prefix[0].set(0);
            for(std::size_t j=0;j<vars.size();++j)
                for(int x=1;x<16;++x)if(d[vars[j]][x])prefix[j+1]|=shift(prefix[j],roles[j]*x);
            auto back=shift(allowed,f.a|(f.b<<4));
            if(!prefix.back().intersects(back))return false;
            for(std::size_t j=vars.size();j-- >0;){Mask keep;Pairs previous;
                for(int x=1;x<16;++x)if(d[vars[j]][x]){auto possible=shift(back,roles[j]*x);
                    if(prefix[j].intersects(possible)){keep.set(x);previous|=possible;}}
                if(keep.none())return false;
                if(keep!=d[vars[j]]){d[vars[j]]=keep;changed=true;}
                back=previous;
            }
        }
    }
    return true;
}
}
