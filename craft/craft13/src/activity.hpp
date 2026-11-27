#pragma once
#include "craft.hpp"

namespace craft {
inline unsigned pack_activity(uint16_t m){unsigned z=0;for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(m&(1u<<(4*r+c)))z|=1u<<(4*c+r);return z;}
inline uint16_t unpack_activity(unsigned z){uint16_t m=0;for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(z&(1u<<(4*c+r)))m|=uint16_t(1u<<(4*r+c));return m;}
struct ActivityGraph {
    struct Edge{unsigned next;long double prior;};
    std::array<std::array<unsigned,16>,16> column_counts{};
    std::array<std::vector<Edge>,16> column;
    ActivityGraph(){

        for(unsigned code=0;code<65536;++code){
            unsigned a=code&15,b=(code>>4)&15,c=(code>>8)&15,d=(code>>12)&15;
            unsigned in=(a!=0)|((b!=0)<<1)|((c!=0)<<2)|((d!=0)<<3);
            unsigned out=((a^c^d)!=0)|(((b^d)!=0)<<1)|((c!=0)<<2)|((d!=0)<<3);++column_counts[in][out];
        }
        for(unsigned a=0;a<16;++a){unsigned total=0;for(auto n:column_counts[a])total+=n;for(unsigned b=0;b<16;++b)if(column_counts[a][b])column[a].push_back({b,0});
            for(auto& e:column[a])e.prior=static_cast<long double>(column_counts[a][e.next])/total;
        }
    }
    template<class Fn> void transitions(uint16_t m,Fn fn) const {
        auto input=pack_activity(m);
        auto rec=[&](auto&& self,int c,unsigned out,long double prior)->void{
            if(c==4){fn(permute_mask(unpack_activity(out)),prior);return;}
            for(const auto& e:column[(input>>(4*c))&15])self(self,c+1,out|(e.next<<(4*c)),prior*e.prior);
        };rec(rec,0,0,1);
    }
    std::vector<long double> contract(std::vector<long double> values) const {
        std::vector<long double> next(65536);
        for(int axis=0;axis<4;++axis){unsigned shift=4*axis,clear=~(15u<<shift);
            for(unsigned code=0;code<65536;++code){long double sum=0;for(const auto& e:column[(code>>shift)&15])sum+=e.prior*values[(code&clear)|(e.next<<shift)];next[code]=sum;}values.swap(next);
        }return values;
    }
};

class ActivityGuide {
    const ActivityGraph& graph;
    int rounds;
    double bias;
    std::vector<std::vector<long double>> h;
    std::vector<long double> initial_cdf;
    std::array<long double,17> penalty{};
public:
    ActivityGuide(const ActivityGraph& g,int r,double mu):graph(g),rounds(r),bias(mu),h(r+1,std::vector<long double>(65536)){
        require(r>=1&&std::isfinite(mu)&&mu>=0,"invalid activity guide");
        for(int a=0;a<=16;++a)penalty[a]=std::exp2(-static_cast<long double>(mu)*a);
        std::fill(h[r].begin()+1,h[r].end(),1);
        for(int k=r-1;k>=0;--k){
            std::vector<long double> values(65536);
            for(unsigned code=0;code<65536;++code){auto mask=permute_mask(unpack_activity(code));values[code]=h[k+1][mask]*penalty[pop(mask)];}
            values=graph.contract(std::move(values));long double top=0;
            for(unsigned code=1;code<65536;++code){auto mask=unpack_activity(code);h[k][mask]=values[code];top=std::max(top,values[code]);}
            require(top>0,"empty activity dynamic program");for(auto& v:h[k])v/=top;
        }
        initial_cdf.resize(65536);std::partial_sum(h[0].begin(),h[0].end(),initial_cdf.begin());
    }
    Pattern sample(RNG& rng) const {
        long double u=std::generate_canonical<long double,64>(rng)*initial_cdf.back();
        auto it=std::upper_bound(initial_cdf.begin(),initial_cdf.end(),u);
        unsigned mask=unsigned(std::min<size_t>(initial_cdf.size()-1,size_t(it-initial_cdf.begin())));require(mask!=0,"sampled zero activity");Pattern p{uint16_t(mask)};
        for(int k=0;k<rounds;++k){
            std::vector<std::pair<uint16_t,long double>> choices;long double sum=0;
            graph.transitions(uint16_t(mask),[&](uint16_t to,long double prior){long double w=prior*penalty[pop(to)]*h[k+1][to];sum+=w;choices.emplace_back(to,w);});
            require(sum>0,"empty activity bridge");u=std::generate_canonical<long double,64>(rng)*sum;size_t j=0;
            for(;j+1<choices.size();++j){u-=choices[j].second;if(u<0)break;}mask=choices[j].first;p.push_back(uint16_t(mask));
        }return p;
    }
    double activity_bias() const{return bias;}
};
}
