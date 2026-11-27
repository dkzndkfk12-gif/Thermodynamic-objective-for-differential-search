#pragma once
#include "activity.hpp"

namespace craft {
inline uint16_t binary_linear(uint16_t mask){State x{};for(int i=0;i<16;++i)x[i]=uint8_t((mask>>i)&1);return craft::mask(linear(x));}
inline mpz_class random_below(const mpz_class& n,RNG& rng){
    require(n>0,"empty integer measure");if(n==1)return 0;mpz_class top=n-1;auto bits=mpz_sizeinbase(top.get_mpz_t(),2);std::vector<uint64_t> data((bits+63)/64);
    for(;;){for(auto& x:data)x=rng();if(bits%64)data.back()&=(uint64_t(1)<<(bits%64))-1;mpz_class x;mpz_import(x.get_mpz_t(),data.size(),-1,sizeof(uint64_t),0,0,data.data());if(x<n)return x;}
}
struct CommonTransfer {
    Pattern pattern;
    int input=0,active=0;
    std::vector<std::array<mpz_class,16>> prefix;
    CommonTransfer(Pattern p,int u):pattern(std::move(p)),input(u),prefix(pattern.size()){
        require(u>0&&u<16&&pattern.size()>1,"bad common-value transfer");prefix[0][u]=1;
        for(size_t r=0;r+1<pattern.size();++r){
            require(binary_linear(pattern[r])==pattern[r+1],"pattern is not a common-value linear orbit");int a=pop(pattern[r+1]);active+=a;
            for(int x=1;x<16;++x)for(int y=1;y<16;++y){int n=ddt().n[x][y];if(n)prefix[r+1][y]+=prefix[r][x]<<(n==4?a:0);}
        }
    }
    Dyadic mass(int v) const {require(v>0&&v<16,"invalid common output");return {prefix.back()[v],unsigned(3*active)};}
    Trail sample(int output,RNG& rng) const {
        require(output>0&&output<16&&prefix.back()[output]>0,"empty common endpoint");std::vector<int> value(pattern.size());value.back()=output;
        for(int r=int(pattern.size())-2;r>=0;--r){int a=pop(pattern[r+1]),v=value[r+1];auto ticket=random_below(prefix[r+1][v],rng);int chosen=0;
            for(int u=1;u<16;++u){int n=ddt().n[u][v];if(!n)continue;mpz_class w=prefix[r][u]<<(n==4?a:0);if(ticket<w){chosen=u;break;}ticket-=w;}require(chosen>0,"common backward normalization");value[r]=chosen;
        }
        Trail t(pattern.size());for(size_t r=0;r<t.size();++r)for(int c=0;c<16;++c)if(pattern[r]&(1u<<c))t[r][c]=uint8_t(value[r]);return t;
    }
};
inline Dyadic common_certificate(const Trail& t){
    require(t.size()>1,"common certificate needs a trail");int u=0,v=0;
    for(auto x:t.front())if(x){require(!u||u==x,"common input has different active values");u=x;}
    for(auto x:t.back())if(x){require(!v||v==x,"common output has different active values");v=x;}
    return CommonTransfer(activity(t),u).mass(v);
}

class CommonGuide {
    int rounds;
    std::vector<unsigned> mapping,score;
    std::vector<std::vector<long double>> cdf;
public:
    explicit CommonGuide(int r):rounds(r),mapping(65536),score(65536){
        unsigned low=unsigned(16*r);
        for(unsigned m=0;m<65536;++m)mapping[m]=binary_linear(uint16_t(m));
        for(unsigned m=1;m<65536;++m){unsigned x=m;for(int k=0;k<r;++k){x=mapping[x];score[m]+=unsigned(pop(x));}low=std::min(low,score[m]);}
        for(double mu:{0.5,1.0,2.0,4.0,8.0}){std::vector<long double> row(65536);for(unsigned m=1;m<65536;++m)row[m]=std::exp2(-static_cast<long double>(mu)*(score[m]-low));std::partial_sum(row.begin(),row.end(),row.begin());cdf.push_back(std::move(row));}
    }
    Trail sample(RNG& rng) const {
        const auto& row=cdf[size_t(draw(rng,int(cdf.size())))];auto u=std::generate_canonical<long double,64>(rng)*row.back();
        unsigned m=unsigned(std::min<size_t>(65535,size_t(std::upper_bound(row.begin(),row.end(),u)-row.begin())));require(m!=0,"zero common support");Pattern p{uint16_t(m)};
        for(int r=0;r<rounds;++r){m=mapping[m];p.push_back(uint16_t(m));}
        CommonTransfer transfer(std::move(p),1+draw(rng,15));std::array<mpz_class,16> weights;mpz_class total=0;
        for(int v=1;v<16;++v){weights[v]=transfer.prefix.back()[v]*transfer.prefix.back()[v];total+=weights[v];}
        auto ticket=random_below(total,rng);int chosen=0;
        for(int v=1;v<16;++v){if(ticket<weights[v]){chosen=v;break;}ticket-=weights[v];}
        return transfer.sample(chosen,rng);
    }
};
}
