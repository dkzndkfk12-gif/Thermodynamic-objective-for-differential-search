#pragma once

#include <gmpxx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "segment_budget.hpp"
namespace segment_mass {
using W=__uint128_t;
using Z=mpz_class;
struct Hash { std::size_t operator()(W w)const{return std::uint64_t(w)^std::uint64_t(w>>64)*0x9e3779b97f4a7c15ULL;} };
struct Stop {};
struct ResourceStop {};
inline double clock(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline unsigned active(W x){unsigned a=0;while(x){a+=bool(x&15);x>>=4;}return a;}
inline std::string hex(W x,unsigned n){std::string s(n,'0');for(unsigned i=0;i<n;++i){s[n-1-i]="0123456789ABCDEF"[unsigned(x&15)];x>>=4;}return s;}

inline W word(const std::string& s,unsigned n){if(s.size()!=n)throw std::runtime_error("segment state width");W w=0;for(unsigned i=0;i<n;++i){char c=s[i];unsigned v=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:16;if(v>15)throw std::runtime_error("segment state digit");w=(w<<4)|W(v);}return w;}
inline double log2z(const Z& z){if(z<=0)return -INFINITY;long e=0;double d=mpz_get_d_2exp(&e,z.get_mpz_t());return std::log2(d)+double(e);}
inline Z power(const Z& z,unsigned m){Z a;mpz_pow_ui(a.get_mpz_t(),z.get_mpz_t(),m);return a;}
inline Z below(const Z& bound,std::mt19937_64& rng){
    if(bound<=0)throw std::runtime_error("empty integer conditional");
    if(bound==1)return 0;
    unsigned bits=unsigned(mpz_sizeinbase(bound.get_mpz_t(),2));
    for(;;){Z x=0;for(unsigned k=0;k<bits;k+=64){std::uint64_t v=rng();if(bits-k<64)v&=(std::uint64_t(1)<<(bits-k))-1;Z t;mpz_import(t.get_mpz_t(),1,1,sizeof(v),0,0,&v);x+=t<<k;}if(x<bound)return x;}
}
inline unsigned draw(const std::vector<Z>& weights,std::mt19937_64& rng,unsigned m=1){
    Z total=0;std::vector<Z> p;p.reserve(weights.size());for(const auto& w:weights){p.push_back(power(w,m));total+=p.back();}
    Z x=below(total,rng);for(unsigned i=0;i<p.size();++i){if(x<p[i])return i;x-=p[i];}throw std::runtime_error("integer draw invariant");
}
struct Spec {
    unsigned N=0,R=0;std::array<unsigned,16> sbox{};std::vector<unsigned> perm,iperm;
    unsigned ddt[16][16]{};
    Spec(unsigned n,unsigned r,const std::vector<unsigned>& s,const std::vector<unsigned>& p):N(n),R(r),perm(p){
        if(n<1||n>32||r<1||r>64||s.size()!=16||p.size()!=4*n)throw std::runtime_error("segment specification dimensions");
        std::set<unsigned> ss(s.begin(),s.end()),pp(p.begin(),p.end());
        if(ss.size()!=16||*ss.rbegin()!=15||pp.size()!=4*n||*pp.rbegin()!=4*n-1)throw std::runtime_error("segment permutation not bijective");
        std::copy(s.begin(),s.end(),sbox.begin());iperm.resize(4*n);for(unsigned i=0;i<4*n;++i)iperm[p[i]]=i;
        for(unsigned a=0;a<16;++a)for(unsigned x=0;x<16;++x)++ddt[a][s[x]^s[x^a]];
    }
    W route(W w,bool inverse=false)const{W v=0;const auto& p=inverse?iperm:perm;for(unsigned i=0;i<4*N;++i)if((w>>i)&1)v|=W(1)<<p[i];return v;}
    Z coefficient(W u,W v)const{W b=route(v,true);Z z=1;for(unsigned i=0;i<N;++i){z*=ddt[unsigned(u&15)][unsigned(b&15)];u>>=4;b>>=4;}return z;}
    std::string identity()const{std::string s="SEGMENT_DDT_V1/"+std::to_string(N)+"/";for(auto v:sbox)s+=std::to_string(v)+",";s+='/';for(auto v:perm)s+=std::to_string(v)+",";return s;}
};
struct Row {std::vector<W> neighbors;bool complete=false;std::uint64_t work=0;};

inline Row enumerate(const Spec& s,W x,bool inverse,unsigned cap,std::uint64_t budget,
                     std::mt19937_64& rng,const std::function<void()>& check){
    Row row;std::vector<unsigned> cells;W b=inverse?s.route(x,true):x;
    for(unsigned i=0;i<s.N;++i)if((b>>(4*i))&15)cells.push_back(i);
    if(inverse&&cells.size()>cap){row.complete=true;return row;}
    std::shuffle(cells.begin(),cells.end(),rng);
    std::vector<std::vector<unsigned>> options;
    for(unsigned i:cells){unsigned a=unsigned((b>>(4*i))&15);std::vector<unsigned> vs;for(unsigned v=1;v<16;++v)if(inverse?s.ddt[v][a]:s.ddt[a][v])vs.push_back(v);std::shuffle(vs.begin(),vs.end(),rng);options.push_back(vs);}
    bool exhausted=false;
    std::function<void(unsigned,W)> dfs=[&](unsigned k,W y){
        if(exhausted)return;
        if(row.work>=budget){exhausted=true;return;}
        if((++row.work&255)==0)check();
        if(k==cells.size()){row.neighbors.push_back(y);return;}
        unsigned c=cells[k];
        for(unsigned v:options[k]){
            W part=inverse?(W(v)<<(4*c)):s.route(W(v)<<(4*c));

            W next=y|part;if(active(next)<=cap)dfs(k+1,next);
            if(exhausted)return;
        }
    };dfs(0,0);row.complete=!exhausted;return row;
}
struct Edge {unsigned u,v;Z c;};
struct Node {W w;std::vector<unsigned> out,in;std::array<std::uint64_t,3> work{};std::array<bool,3> complete{};};
struct Graph {
    const Spec& s;unsigned input_cap,cap;std::uint64_t bytes,used=0;std::vector<Node> nodes;std::vector<Edge> edges;
    std::unordered_map<W,unsigned,Hash> index;std::set<std::pair<unsigned,unsigned>> pairs;
    Graph(const Spec& ss,unsigned in,unsigned k,std::uint64_t memory):s(ss),input_cap(in),cap(k),bytes(memory){}

    void reserve(std::uint64_t n){if(used>bytes/2||n>bytes/2-used)throw ResourceStop();used+=n;}
    unsigned node(W w){auto it=index.find(w);if(it!=index.end())return it->second;if(!w||(s.N<32&&(w>>(4*s.N))))throw std::runtime_error("segment state outside nonzero block domain");reserve(320);unsigned i=unsigned(nodes.size());nodes.push_back({w,{},{},{},{}});index.emplace(w,i);return i;}
    bool add(W u,W v){Z c=s.coefficient(u,v);if(c<=0)throw std::runtime_error("illegal segment edge");unsigned a=node(u),b=node(v);if(pairs.count({a,b}))return false;reserve(192+unsigned(mpz_size(c.get_mpz_t()))*sizeof(mp_limb_t));pairs.emplace(a,b);unsigned i=unsigned(edges.size());edges.push_back({a,b,c});nodes[a].out.push_back(i);nodes[b].in.push_back(i);return true;}
    bool allowed(unsigned layer,unsigned i)const{return layer==s.R||active(nodes[i].w)<=(layer==0?input_cap:cap);}
    using Messages=std::vector<std::vector<Z>>;
    struct TraversalStats {std::uint64_t vertices=0,edges=0;};
    std::uint64_t message_bytes_for_nodes(std::uint64_t count)const{

        return message_reservation_bytes(s.N,s.R,count);
    }
    std::uint64_t message_bytes()const{return message_bytes_for_nodes(nodes.size());}
    Messages messages(bool backward,int endpoint,const std::function<void()>& check,TraversalStats* stats=nullptr)const{

        if(message_bytes()>((bytes-used)/4))throw ResourceStop();
        if(endpoint>=0&&std::size_t(endpoint)>=nodes.size())throw std::runtime_error("segment message endpoint outside graph");
        Messages d;d.reserve(s.R+1);for(unsigned t=0;t<=s.R;++t)d.emplace_back(nodes.size());
        unsigned begin=backward?s.R:0;std::vector<unsigned> current,next;
        current.reserve(nodes.size());next.reserve(nodes.size());
        if(endpoint<0){for(unsigned i=0;i<nodes.size();++i)if(allowed(begin,i)){d[begin][i]=1;current.push_back(i);}}
        else if(allowed(begin,unsigned(endpoint))){d[begin][endpoint]=1;current.push_back(unsigned(endpoint));}
        for(unsigned t=0;t<s.R;++t){check();unsigned r=backward?s.R-1-t:t;std::uint64_t q=0;
            if(current.empty())break;
            next.clear();

            for(unsigned u:current){
                if(stats)++stats->vertices;
                for(unsigned k:backward?nodes[u].in:nodes[u].out){
                    if((++q&4095)==0)check();
                    if(stats)++stats->edges;
                    const auto& e=edges[k];unsigned v=backward?e.u:e.v;
                    if(!allowed(backward?r:r+1,v))continue;
                    Z& target=d[backward?r:r+1][v];
                    if(target==0)next.push_back(v);
                    target+=e.c*d[backward?r+1:r][u];
                }
            }
            current.swap(next);
        }return d;
    }
    std::vector<W> sample(unsigned a,unsigned b,const Messages& back,std::mt19937_64& rng)const{
        std::vector<W> path{nodes.at(a).w};unsigned u=a;
        for(unsigned r=0;r<s.R;++r){std::vector<unsigned> ei;std::vector<Z> w;
            for(unsigned k:nodes[u].out){const auto& e=edges[k];if(allowed(r+1,e.v)&&back[r+1][e.v]>0){ei.push_back(k);w.push_back(e.c*back[r+1][e.v]);}}
            u=edges.at(ei.at(draw(w,rng))).v;path.push_back(nodes[u].w);
        }if(u!=b)throw std::runtime_error("segment path endpoint invariant");return path;
    }

    static Z single_layer_gain(const Messages& left,const Messages& right,unsigned r,unsigned u,unsigned v,const Z& c){return left.at(r).at(u)*c*right.at(r+1).at(v);}
};
}
