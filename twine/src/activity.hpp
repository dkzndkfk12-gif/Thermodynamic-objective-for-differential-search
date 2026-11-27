#pragma once
#include "twine.hpp"

namespace twine {
struct ActivityEdge { uint16_t next=0; uint8_t cancellations=0; };
class ActivityGraph {
public:
    std::vector<std::vector<ActivityEdge>> edge;
    std::array<uint8_t,65536> active{}, both{};
    ActivityGraph():edge(65536) {
        for(unsigned m=0;m<65536;++m) {
            std::vector<int> cancel;uint16_t base=uint16_t(m);
            for(int j=0;j<8;++j)if(m&(1u<<(2*j))) { ++active[m];base|=uint16_t(1u<<(2*j+1));if(m&(1u<<(2*j+1)))cancel.push_back(2*j+1); }
            both[m]=uint8_t(cancel.size());
            for(unsigned s=0;s<(1u<<cancel.size());++s) {
                uint16_t pre=base,out=0;
                for(size_t k=0;k<cancel.size();++k)if(s&(1u<<k))pre&=uint16_t(~(1u<<cancel[k]));
                for(int i=0;i<16;++i)if(pre&(1u<<i))out|=uint16_t(1u<<PI[i]);
                edge[m].push_back({out,uint8_t(pop(s))});
            }
        }
    }
};

class ActivityGuide {
    const ActivityGraph& g;
    int rounds;
    double bias;
    std::vector<std::vector<long double>> h;
    std::array<std::array<long double,9>,9> prior{};
    long double edge_prior(unsigned m,const ActivityEdge& e) const {
        return prior[g.both[m]][e.cancellations];
    }
public:
    ActivityGuide(const ActivityGraph& graph,int r,double mu):g(graph),rounds(r),bias(mu),h(r+1,std::vector<long double>(65536)) {
        require(r>=1 && std::isfinite(mu) && mu>=0,"invalid activity guide");
        for(int n=0;n<=8;++n)for(int c=0;c<=n;++c)prior[n][c]=std::pow(1.0L/15,c)*std::pow(14.0L/15,n-c);
        std::array<long double,9> penalty{};for(int a=0;a<=8;++a)penalty[a]=std::exp2(-bias*a);
        std::fill(h[r].begin()+1,h[r].end(),1);
        for(int k=r-1;k>=0;--k) {
            long double top=0;
            for(unsigned m=1;m<65536;++m) { long double sum=0;for(const auto& e:g.edge[m])sum+=edge_prior(m,e)*h[k+1][e.next];h[k][m]=penalty[g.active[m]]*sum;top=std::max(top,h[k][m]); }
            require(top>0,"activity DP vanished");for(auto& v:h[k])v/=top;
        }
    }
    Pattern sample(RNG& rng) const {
        long double total=std::accumulate(h[0].begin(),h[0].end(),0.0L),u=unit(rng)*total;
        unsigned m=1;for(;m<65535;++m){u-=h[0][m];if(u<0)break;}
        Pattern p{uint16_t(m)};
        for(int k=0;k<rounds;++k) {
            std::vector<long double> w;total=0;for(const auto& e:g.edge[m]) { auto v=edge_prior(m,e)*h[k+1][e.next];w.push_back(v);total+=v; }
            require(total>0,"activity bridge is empty");u=unit(rng)*total;size_t j=0;for(;j+1<w.size();++j){u-=w[j];if(u<0)break;}
            m=g.edge[m][j].next;p.push_back(uint16_t(m));
        }return p;
    }
    double activity_bias() const { return bias; }
};
}
