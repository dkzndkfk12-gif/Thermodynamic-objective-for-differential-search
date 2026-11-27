#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <gmpxx.h>
#include "vendor/json.hpp"

namespace craft {
using J = nlohmann::json;
using RNG = std::mt19937_64;
using Clock = std::chrono::steady_clock;
using State = std::array<uint8_t,16>;
using Trail = std::vector<State>;
using Pattern = std::vector<uint16_t>;
using Values = std::vector<uint8_t>;
using Domains = std::vector<uint16_t>;
constexpr std::array<int,16> SBOX{12,10,13,3,14,11,15,7,8,9,1,5,0,2,4,6};

constexpr std::array<int,16> PI{15,12,13,14,10,9,8,11,6,5,4,7,1,2,3,0};
inline void require(bool p, const std::string& s) { if (!p) throw std::runtime_error(s); }
inline int pop(unsigned x) { return __builtin_popcount(x); }
inline int first(unsigned x) { require(x!=0,"empty domain"); return __builtin_ctz(x); }
inline double seconds(Clock::time_point a) { return std::chrono::duration<double>(Clock::now()-a).count(); }
inline uint64_t mix(uint64_t x) { x+=0x9e3779b97f4a7c15ULL; x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); }
inline int draw(RNG& g,int n) { require(n>0,"empty draw"); return std::uniform_int_distribution<int>(0,n-1)(g); }
inline double unit(RNG& g) { return std::generate_canonical<double,53>(g); }
inline double logadd(double a,double b) { if(a==-INFINITY)return b; if(b==-INFINITY)return a; if(a<b)std::swap(a,b); return a+std::log1p(std::exp(b-a)); }
inline int draw_log(const std::vector<double>& x,RNG& g) {
    double h=*std::max_element(x.begin(),x.end()); require(std::isfinite(h),"empty conditional");
    double total=0; for(double v:x)total+=std::exp(v-h);
    double u=unit(g)*total; int last=-1;
    for(size_t i=0;i<x.size();++i)if(std::isfinite(x[i])) { last=int(i); u-=std::exp(x[i]-h); if(u<0)return int(i); }
    require(last>=0,"conditional roundoff"); return last;
}
struct DDT {
    std::array<std::array<uint8_t,16>,16> n{};
    DDT() { for(int a=0;a<16;++a)for(int x=0;x<16;++x)++n[a][SBOX[x]^SBOX[x^a]]; }
    int weight(int a,int b) const { int v=n[a][b]; return v ? 4-first(unsigned(v)) : -1; }
};
inline const DDT& ddt() { static const DDT d; return d; }
inline std::string hex(const State& x) { std::string s; for(auto v:x){require(v<16,"bad nibble");s+="0123456789ABCDEF"[v];}return s; }
inline State unhex(std::string s) {
    if(s.size()==18 && s.substr(0,2)=="0x")s=s.substr(2);
    require(s.size()==16,"state must contain 16 hexadecimal digits"); State x{};
    for(int i=0;i<16;++i) { char c=s[i]; if(c>='a'&&c<='f')c=char(c-'a'+'A'); auto p=std::string("0123456789ABCDEF").find(c); require(p!=std::string::npos,"invalid hex"); x[i]=uint8_t(p); }
    return x;
}
inline bool nonzero(const State& x) { return std::any_of(x.begin(),x.end(),[](int v){return v!=0;}); }
inline uint16_t mask(const State& x) { uint16_t m=0;for(int i=0;i<16;++i)if(x[i])m|=uint16_t(1u<<i);return m; }
inline Pattern activity(const Trail& t) { Pattern p;for(const auto& x:t)p.push_back(mask(x));return p; }
inline std::string pattern_key(const Pattern& p) { std::ostringstream s;s<<std::hex<<std::setfill('0');for(auto m:p)s<<std::setw(4)<<m;return s.str(); }
inline State permute(const State& x) { State y{};for(int i=0;i<16;++i)y[PI[i]]=x[i];return y; }
inline State unpermute(const State& x) { State y{};for(int i=0;i<16;++i)y[i]=x[PI[i]];return y; }

inline State mix_columns(State x) {
    for(int c=0;c<4;++c){x[c]^=x[8+c]^x[12+c];x[4+c]^=x[12+c];}return x;
}
inline State linear(const State& x) { return permute(mix_columns(x)); }
inline State inverse_linear(const State& x) { return mix_columns(unpermute(x)); }
inline uint16_t permute_mask(uint16_t x) { uint16_t y=0;for(int c=0;c<16;++c)if(x&(1u<<c))y|=uint16_t(1u<<PI[c]);return y; }
inline const std::array<uint16_t,16>& linear_rows() {
    static const auto rows=[](){std::array<uint16_t,16> a{};for(int c=0;c<16;++c){State e{};e[c]=1;auto y=linear(e);for(int i=0;i<16;++i)if(y[i])a[i]|=uint16_t(1u<<c);}return a;}();return rows;
}
inline int round_weight(const State& x,const State& y) {
    auto a=linear(x);int w=0;for(int c=0;c<16;++c){int t=ddt().weight(a[c],y[c]);if(t<0)return -1;w+=t;}return w;
}
inline int trail_weight(const Trail& t) { require(t.size()>1,"empty trail");int w=0;for(size_t r=1;r<t.size();++r){int q=round_weight(t[r-1],t[r]);if(q<0)return -1;w+=q;}return w; }

template<class Fn> void transitions(const State& state,bool reverse,Fn fn) {
    State x=reverse?state:linear(state), y{};
    auto visit=[&](auto&& self,int j,int w)->void {
        if(j==16){fn(reverse?inverse_linear(y):y,w);return;}
        for(int b=0;b<16;++b){int a=reverse?b:x[j],z=reverse?x[j]:b;if(ddt().n[a][z]){y[j]=uint8_t(b);self(self,j+1,w+ddt().weight(a,z));}}
    };visit(visit,0,0);
}
struct Dyadic {
    mpz_class n=0; unsigned e=0;
    Dyadic()=default;
    Dyadic(mpz_class a,unsigned b):n(std::move(a)),e(b) { normalize(); }
    void normalize() { require(n>=0,"negative mass");if(n==0){e=0;return;}unsigned k=unsigned(std::min<mp_bitcnt_t>(e,mpz_scan1(n.get_mpz_t(),0)));n>>=k;e-=k; }
    Dyadic& operator+=(const Dyadic& b) { unsigned f=std::max(e,b.e);n=(n<<(f-e))+(b.n<<(f-b.e));e=f;normalize();return *this; }
    int compare(const Dyadic& b) const { unsigned f=std::max(e,b.e);mpz_class a1=n<<(f-e),b1=b.n<<(f-b.e);return mpz_cmp(a1.get_mpz_t(),b1.get_mpz_t()); }
    Dyadic difference(const Dyadic& b) const { unsigned f=std::max(e,b.e);mpz_class a1=n<<(f-e),b1=b.n<<(f-b.e);require(a1>=b1,"mass decreased");return {a1-b1,f}; }
    double neglog2() const { if(n==0)return INFINITY;signed long exp=0;double m=mpz_get_d_2exp(&exp,n.get_mpz_t());return double(e)-double(exp)-std::log2(m); }
    J json() const { return { {"numerator",n.get_str()},{"denominator_power",e},{"negative_log2",n==0?J(nullptr):J(neglog2())} }; }
};
inline J trail_json(const Trail& t) { J j=J::array();for(const auto& x:t)j.push_back(hex(x));return j; }
inline Trail trail_from_json(const J& j) { Trail t;for(const auto& s:j)t.push_back(unhex(s.get<std::string>()));return t; }
}
