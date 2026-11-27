#include "../../common/factor_sum.hpp"
#include <tuple>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "rounds.hpp"
#ifdef __linux__
#include <sys/resource.h>
#endif

using Real=long double;
using Clock=std::chrono::steady_clock;
constexpr Real INF=std::numeric_limits<Real>::infinity();
constexpr int SR[16]={0,10,5,15,14,4,11,1,9,3,12,6,7,13,2,8};
volatile std::sig_atomic_t stopped=0;
void on_signal(int){stopped=1;}
struct Budget:std::runtime_error{using std::runtime_error::runtime_error;};
struct Timer {
    Clock::time_point start=Clock::now(); double limit;
    explicit Timer(double v):limit(v){}
    double seconds() const{return std::chrono::duration<double>(Clock::now()-start).count();}
    void check() const{if(stopped || seconds()>=limit) throw Budget("budget/signal");}
};
using RNG=std::mt19937_64;
int draw(RNG& g,int n){if(n<1)throw std::runtime_error("empty choice");return std::uniform_int_distribution<int>(0,n-1)(g);}
Real unit(RNG& g){return std::generate_canonical<Real,std::numeric_limits<Real>::digits>(g);}
bool accept(RNG& g,Real x){return x>=0 || std::log(unit(g))<x;}
uint64_t mix(uint64_t x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}
uint64_t key_rank(const std::string&s,uint64_t seed){uint64_t x=1469598103934665603ULL^seed;for(unsigned char c:s){x^=c;x*=1099511628211ULL;}return mix(x);}
int choose(const std::vector<Real>& scores,RNG& g){
    Real top=*std::max_element(scores.begin(),scores.end());
    if(!std::isfinite(top))throw std::runtime_error("empty conditional");
    Real total=0;for(auto x:scores)total+=std::exp(x-top);
    Real u=unit(g)*total;int last=-1;
    for(size_t i=0;i<scores.size();++i){Real w=std::exp(scores[i]-top);if(w>0){last=int(i);if(u<w)return int(i);u-=w;}}
    if(last<0)throw std::runtime_error("conditional underflow");
    return last;
}
struct Options{
    std::string mode,out,model,endpoint,anchor,initial,bridge,domains;
    uint64_t seed=1,first=0,paths=32,sweeps=1000000;
    int active=49,limit=32,copies=2,steps=512,pattern=-1,center=12,width=8;
    double seconds=300;
};
Options parse(int argc,char** argv){
    if(argc<2)throw std::runtime_error("core pattern|search|expand|ais --out FILE ...");
    Options o;o.mode=argv[1];std::set<std::string> seen;
    for(int i=2;i<argc;++i){
        std::string k=argv[i];if(i+1>=argc || !seen.insert(k).second)throw std::runtime_error("missing/repeated option");
        std::string v=argv[++i];size_t used=0;
        if(k=="--out")o.out=v;else if(k=="--model")o.model=v;
        else if(k=="--endpoint")o.endpoint=v;else if(k=="--anchor")o.anchor=v;
        else if(k=="--initial")o.initial=v;else if(k=="--bridge")o.bridge=v;
        else if(k=="--domains")o.domains=v;
        else if(k=="--seconds"){o.seconds=std::stod(v,&used);if(used!=v.size())throw std::runtime_error("bad seconds");}
        else{
            if(v.empty() || v.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("expected unsigned integer: "+k);
            uint64_t n=std::stoull(v,&used);if(used!=v.size())throw std::runtime_error("bad integer");
            if(k=="--seed")o.seed=n;else if(k=="--first")o.first=n;else if(k=="--paths")o.paths=n;
            else if(k=="--sweeps")o.sweeps=n;
            else{if(n>1000000)throw std::runtime_error("option too large");
                if(k=="--active")o.active=int(n);else if(k=="--limit")o.limit=int(n);
                else if(k=="--copies")o.copies=int(n);else if(k=="--steps")o.steps=int(n);
                else if(k=="--center")o.center=int(n);else if(k=="--width")o.width=int(n);
                else if(k=="--pattern")o.pattern=int(n);else throw std::runtime_error("unknown option: "+k);}
        }
    }
    if(o.out.empty() || !std::isfinite(o.seconds) || o.seconds<=0 || o.active<ROUNDS || o.active>MAX_FACTORS ||
       o.limit<1 || o.limit>4096 || o.copies<1 || o.copies>8 || o.steps<2 || o.steps>32768 ||
       !o.paths || o.paths>1000000 || !o.sweeps || o.first>UINT64_MAX-o.paths || o.center>INTERNAL_CELLS || o.width<1 || o.width>INTERNAL_CELLS)
        throw std::runtime_error("invalid options");
    if(o.mode!="pattern" && o.mode!="search" && o.mode!="ais" && o.mode!="expand")throw std::runtime_error("bad mode");
    return o;
}
std::string hex(const std::vector<unsigned char>& x,int a,int b){
    std::string s;for(int i=a;i<b;++i)s+="0123456789ABCDEF"[x[i]];return s;
}
std::vector<unsigned char> unhex(const std::string& s){
    std::vector<unsigned char>x;for(char c:s){auto p=std::string("123456789ABCDEF").find(c);if(p==std::string::npos)throw std::runtime_error("bad coordinate");x.push_back(static_cast<unsigned char>(p+1));}return x;
}
void publish(const std::string& path){
    if(std::ifstream(path))throw std::runtime_error("completed output already exists");
    if(std::rename((path+".part").c_str(),path.c_str()))throw std::runtime_error("cannot publish output");
}

using Pattern=std::array<uint16_t,ROUNDS>;
int pop(unsigned x){int n=0;while(x){x&=x-1;++n;}return n;}
Pattern read_pattern(const std::string& path){
    std::ifstream f(path);Pattern p{};std::string s;std::getline(f,s);
    std::replace(s.begin(),s.end(),',',' ');std::istringstream in(s);
    for(int r=0;r<ROUNDS;++r){if(!(in>>s)||s.size()!=16)throw std::runtime_error("bad anchor");for(int c=0;c<16;++c){if(s[c]=='1')p[r]|=uint16_t(1u<<c);else if(s[c]!='0')throw std::runtime_error("bad mask");}}
    if(in>>s)throw std::runtime_error("extra anchor content");
    return p;
}
struct Support{
    std::array<bool,256> valid{};
    Support(){for(int v=0;v<65536;++v){int a=v&15,b=(v>>4)&15,c=(v>>8)&15,d=(v>>12)&15;
        int u=(a!=0)|((b!=0)<<1)|((c!=0)<<2)|((d!=0)<<3);
        int w=((b^c^d)!=0)|(((a^c^d)!=0)<<1)|(((a^b^d)!=0)<<2)|(((a^b^c)!=0)<<3);valid[u|(w<<4)]=true;}}
    int energy(const Pattern& p)const{
        int e=0;for(auto x:p)e+=x==0;
        for(int r=0;r<ROUNDS-1;++r)for(int c=0;c<4;++c){int a=0,b=0;for(int row=0;row<4;++row){a|=((p[r]>>SR[4*c+row])&1)<<row;b|=((p[r+1]>>(4*c+row))&1)<<row;}e+=!valid[a|(b<<4)];}
        return e;
    }
};
void pattern_run(const Options&o,Timer&timer){
    Support support;RNG rng(o.seed);bool anchored=!o.anchor.empty();Pattern base{};
    if(anchored)base=read_pattern(o.anchor);
    std::vector<int> movable;for(int r=0;r<ROUNDS;++r)if(!anchored || (r>0&&r<ROUNDS-1))for(int c=0;c<16;++c)movable.push_back(16*r+c);
    int fixed=anchored?pop(base[0])+pop(base[ROUNDS-1]):0, need=o.active-fixed;
    if(need<1 || need>int(movable.size()))throw std::runtime_error("active count incompatible with boundary masks");
    const int lanes=24;std::vector<Pattern> states(lanes);std::vector<int> energies(lanes);
    std::vector<Real> betas(lanes);for(int i=1;i<lanes;++i)betas[i]=.125L*std::pow(128.0L,Real(i-1)/(lanes-2));
    for(int i=0;i<lanes;++i){auto p=base;if(!anchored)p.fill(0);else for(int r=1;r<ROUNDS-1;++r)p[r]=0;
        auto order=movable;std::shuffle(order.begin(),order.end(),rng);for(int j=0;j<need;++j)p[order[j]/16]|=uint16_t(1u<<(order[j]%16));
        if(anchored && i==lanes-1){int n=0;for(auto m:base)n+=pop(m);if(n==o.active)p=base;}
        states[i]=p;energies[i]=support.energy(p);
    }
    std::set<Pattern> found;std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);out<<"pattern\n";
    auto collect=[&](int i){if(energies[i] || found.size()>=size_t(o.limit) || !found.insert(states[i]).second)return;
        for(int r=0;r<ROUNDS;++r){if(r)out<<',';for(int c=0;c<16;++c)out<<((states[i][r]>>c)&1);}out<<'\n';out.flush();};
    for(int i=0;i<lanes;++i)collect(i);
    try{for(uint64_t step=0;step<o.sweeps && found.size()<size_t(o.limit);++step){
        timer.check();for(int i=0;i<lanes;++i){
            std::vector<int>a,z;for(int q:movable)((states[i][q/16]>>(q%16))&1?a:z).push_back(q);
            int k=1+(draw(rng,8)==0?1:0);k=std::min(k,int(std::min(a.size(),z.size())));
            std::shuffle(a.begin(),a.end(),rng);std::shuffle(z.begin(),z.end(),rng);Pattern p=states[i];
            for(int j=0;j<k;++j){p[a[j]/16]^=uint16_t(1u<<(a[j]%16));p[z[j]/16]^=uint16_t(1u<<(z[j]%16));}
            int e=support.energy(p);if(accept(rng,betas[i]*(energies[i]-e))){states[i]=p;energies[i]=e;}collect(i);
            if(step%16==0){
                int r=draw(rng,ROUNDS-1),c=draw(rng,4);std::vector<int> pos;
                for(int row=0;row<4;++row){int q=16*r+SR[4*c+row];if(!anchored||q/16>0)pos.push_back(q);q=16*(r+1)+4*c+row;if(!anchored||q/16<ROUNDS-1)pos.push_back(q);}
                Pattern zero=states[i];int n=0;for(int q:pos){n+=(zero[q/16]>>(q%16))&1;zero[q/16]&=uint16_t(~(1u<<(q%16)));}
                std::vector<Real> scores(size_t(1)<<pos.size(),-INF);std::vector<int> es(scores.size());
                for(size_t code=0;code<scores.size();++code)if(pop(unsigned(code))==n){auto trial=zero;for(size_t j=0;j<pos.size();++j)if((code>>j)&1)trial[pos[j]/16]|=uint16_t(1u<<(pos[j]%16));es[code]=support.energy(trial);scores[code]=-betas[i]*es[code];}
                int code=choose(scores,rng);states[i]=zero;for(size_t j=0;j<pos.size();++j)if((code>>j)&1)states[i][pos[j]/16]|=uint16_t(1u<<(pos[j]%16));energies[i]=es[code];collect(i);
            }
        }
        for(int i=int(step%2);i+1<lanes;i+=2)if(accept(rng,(betas[i]-betas[i+1])*(energies[i]-energies[i+1]))){std::swap(states[i],states[i+1]);std::swap(energies[i],energies[i+1]);}
    }}catch(const Budget&){}
    out.close();publish(o.out);std::cerr<<"patterns="<<found.size()<<"; support feasibility only\n";
}

struct Factor{std::vector<int>a,b;int round=0,cell=0;};
struct Incidence{int f;unsigned which;};
struct Model{
    std::string id;int active=0,d=0;Real log_volume=0;std::vector<int>used;
    std::vector<Factor>f;std::vector<std::vector<int>>blocks;
    std::vector<std::vector<Incidence>>inc;
};
struct Catalog{int ne=0,D=0,N=0;std::vector<Model>m;std::array<int,256>ddt{};std::vector<std::vector<unsigned char>> allowed;};
Catalog load(const std::string&path){
    std::ifstream in(path);Catalog c;std::string magic;int k;
    if(!(in>>magic>>c.ne>>c.D>>k)||magic!=MODEL_MAGIC||c.ne<2||c.ne>32||c.D<0||c.D>INTERNAL_CELLS||k<1)throw std::runtime_error("bad catalog header");
    c.N=c.ne+c.D;c.allowed.resize(c.N);
    for(auto& d:c.allowed)for(int v=1;v<16;++v)d.push_back(static_cast<unsigned char>(v));
    constexpr int S[16]={12,10,13,3,14,11,15,7,8,9,1,5,0,2,4,6};
    for(int a=1;a<16;++a)for(int x=0;x<16;++x)++c.ddt[16*a+(S[x]^S[x^a])];
    for(int j=0;j<k;++j){Model m;int nf,nb;if(!(in>>m.id>>m.active>>m.d>>nf>>nb)||nf!=m.active||nf<ROUNDS||nf>MAX_FACTORS||m.d<0||m.d>c.D||nb<0||nb>300)throw std::runtime_error("bad pattern header");
        m.log_volume=m.d*std::log(15.0L);
        std::set<int>used;for(int q=0;q<m.d;++q){int v;if(!(in>>v)||v<c.ne||v>=c.N||!used.insert(v).second)throw std::runtime_error("bad coordinate list");m.used.push_back(v);}
        m.inc.resize(c.N);
        for(int f=0;f<nf;++f){Factor ff;int n;if(!(in>>ff.round>>ff.cell)||ff.round<0||ff.round>=ROUNDS||ff.cell<0||ff.cell>=16)throw std::runtime_error("bad factor");
            for(int side=0;side<2;++side){if(!(in>>n)||n<1||n>16)throw std::runtime_error("bad expression");std::set<int>once;
                for(int z=0;z<n;++z){int v;if(!(in>>v)||v<0||v>=c.N||(v>=c.ne&&!used.count(v))||!once.insert(v).second)throw std::runtime_error("bad expression variable");(side?ff.b:ff.a).push_back(v);}
            }
            for(int v=0;v<c.N;++v){unsigned w=unsigned(std::find(ff.a.begin(),ff.a.end(),v)!=ff.a.end())|unsigned(std::find(ff.b.begin(),ff.b.end(),v)!=ff.b.end())<<1;if(w)m.inc[v].push_back({f,w});}m.f.push_back(ff);
        }
        for(int z=0;z<nb;++z){int n;if(!(in>>n)||n<2||n>3)throw std::runtime_error("bad block");std::vector<int>b;std::set<int>s;for(int q=0;q<n;++q){int v;if(!(in>>v)||!used.count(v)||!s.insert(v).second)throw std::runtime_error("bad block variable");b.push_back(v);}m.blocks.push_back(b);}c.m.push_back(std::move(m));
    }
    if(in>>magic)throw std::runtime_error("extra catalog data");
    return c;
}
void load_domains(Catalog& cat,const Options& o){
    if(o.domains.empty())return;
    std::ifstream in(o.domains);std::string magic;int p,n;
    if(!(in>>magic>>p>>n)||magic!="MIDORI_DOMAINS_V1"||p!=o.pattern||p<0||p>=int(cat.m.size())||n!=cat.m[p].d)
        throw std::runtime_error("bad reduced domains header");
    std::set<int>seen;auto&m=cat.m[p];m.log_volume=0;
    for(int i=0;i<n;++i){int v,size;if(!(in>>v>>size)||size<1||size>15||!seen.insert(v).second||std::find(m.used.begin(),m.used.end(),v)==m.used.end())throw std::runtime_error("bad reduced coordinate");
        std::vector<unsigned char>d;std::set<int>unique;
        for(int j=0;j<size;++j){int x;if(!(in>>x)||x<1||x>15||!unique.insert(x).second)throw std::runtime_error("bad reduced domain");d.push_back(static_cast<unsigned char>(x));}
        cat.allowed[v]=d;m.log_volume+=std::log(Real(size));
    }
    if(in>>magic)throw std::runtime_error("extra reduced domain data");
}
struct Level{Real t=0,lambda=0;bool hard=false;std::array<Real,5>lg{};};
Level level(Real t,bool hard=false){Level l;l.t=t;l.lambda=hard?1:t/16;l.hard=hard;l.lg.fill(-INF);Real eps=std::exp(-t);l.lg[0]=hard?-INF:-t;l.lg[2]=hard?-std::log(2.0L):std::log(eps+(1-eps)*.5L);l.lg[4]=0;return l;}
struct Copy{int p=0;std::vector<unsigned char>x,a,b;};
struct State{std::vector<Copy>c;int tag=0;};
unsigned eval(const std::vector<int>&v,const std::vector<unsigned char>&x){unsigned z=0;for(int i:v)z^=x[i];return z;}
void refresh(const Catalog&cat,Copy&c){const auto&m=cat.m[c.p];c.a.resize(m.f.size());c.b.resize(m.f.size());for(size_t f=0;f<m.f.size();++f){c.a[f]=static_cast<unsigned char>(eval(m.f[f].a,c.x));c.b[f]=static_cast<unsigned char>(eval(m.f[f].b,c.x));}}
Real factor_log(const Catalog&cat,unsigned a,unsigned b,const Level&l){return l.lg[(a&&b)?cat.ddt[16*a+b]:0];}
Real constant(const Catalog&cat,int p,const Level&l,int fixed){const auto&m=cat.m[p];Real logk=fixed<0?std::log(Real(cat.m.size())):0;return -logk+l.lambda*(m.log_volume-2*m.active*std::log(2.0L)+logk);}
Real density_copy(const Catalog&cat,const Copy&c,const Level&l,int fixed){Real s=constant(cat,c.p,l,fixed);for(size_t f=0;f<c.a.size();++f)s+=factor_log(cat,c.a[f],c.b[f],l);return s;}
Real density(const Catalog&cat,const State&s,const Level&l,int fixed){Real v=0;for(const auto&c:s.c)v+=density_copy(cat,c,l,fixed);return v;}
int weight(const Catalog&cat,const Copy&c){int w=0;for(size_t f=0;f<c.a.size();++f){int n=(c.a[f]&&c.b[f])?cat.ddt[16*c.a[f]+c.b[f]]:0;if(!n)return -1;w+=n==4?2:3;}return w;}
State random_state(const Catalog&cat,int copies,RNG&g,int fixed,const std::string&end){
    State s;std::vector<unsigned char>e=unhex(end);if(e.empty())for(int i=0;i<cat.ne;++i)e.push_back(static_cast<unsigned char>(draw(g,15)+1));if(int(e.size())!=cat.ne)throw std::runtime_error("endpoint length mismatch");
    for(int j=0;j<copies;++j){Copy c;c.p=fixed<0?draw(g,int(cat.m.size())):fixed;c.x=e;for(int i=cat.ne;i<cat.N;++i)c.x.push_back(cat.allowed[i][draw(g,int(cat.allowed[i].size()))]);refresh(cat,c);s.c.push_back(std::move(c));}return s;
}

void gibbs(const Catalog&cat,State&s,const Level&l,const std::vector<int>&vars,int chosen,RNG&g){
    bool shared=false;for(int v:vars)shared|=v<cat.ne;
    int begin=shared?0:chosen,end=shared?int(s.c.size()):chosen+1,n=1;for(int v:vars)n*=int(cat.allowed[v].size());
    struct E{int f;unsigned a=0,b=0;};std::vector<std::vector<E>>effects(s.c.size());
    for(int ci=begin;ci<end;++ci){std::map<int,E>mp;const auto&m=cat.m[s.c[ci].p];
        for(size_t j=0;j<vars.size();++j)if(vars[j]<cat.ne||ci==chosen)for(auto e:m.inc[vars[j]]){auto&z=mp[e.f];z.f=e.f;if(e.which&1)z.a|=1u<<j;if(e.which&2)z.b|=1u<<j;}
        for(auto z:mp)effects[ci].push_back(z.second);
    }
    std::vector<Real>scores(n);for(int code=0;code<n;++code){int z=code;std::array<unsigned,3>val{};for(size_t j=0;j<vars.size();++j){const auto&d=cat.allowed[vars[j]];val[j]=d[z%int(d.size())];z/=int(d.size());}Real sc=0;
        for(int ci=begin;ci<end && std::isfinite(sc);++ci){const auto&c=s.c[ci];for(auto e:effects[ci]){unsigned a=c.a[e.f],b=c.b[e.f];for(size_t j=0;j<vars.size();++j){unsigned d=c.x[vars[j]]^val[j];if(e.a&(1u<<j))a^=d;if(e.b&(1u<<j))b^=d;}sc+=factor_log(cat,a,b,l);if(!std::isfinite(sc))break;}}scores[code]=sc;
    }
    int code=choose(scores,g);std::array<unsigned char,3>v{};for(size_t j=0;j<vars.size();++j){const auto&d=cat.allowed[vars[j]];v[j]=d[code%int(d.size())];code/=int(d.size());}
    for(int ci=begin;ci<end;++ci){auto&c=s.c[ci];for(size_t j=0;j<vars.size();++j)if(vars[j]<cat.ne||ci==chosen)c.x[vars[j]]=v[j];refresh(cat,c);}
}

void pattern_gibbs(const Catalog&cat,Copy&c,const Level&l,RNG&g,bool joint){
    int v=joint?cat.ne+draw(g,cat.D):-1,n=joint?15:1;unsigned char old=joint?c.x[v]:0;
    std::vector<Real>scores(cat.m.size()*size_t(n));
    for(size_t p=0;p<cat.m.size();++p)for(int j=0;j<n;++j){if(joint)c.x[v]=static_cast<unsigned char>(j+1);Real sc=constant(cat,int(p),l,-1);
        for(const auto&f:cat.m[p].f){sc+=factor_log(cat,eval(f.a,c.x),eval(f.b,c.x),l);if(!std::isfinite(sc))break;}scores[p*size_t(n)+j]=sc;}
    if(joint)c.x[v]=old;
    int selected=choose(scores,g);c.p=selected/n;if(joint)c.x[v]=static_cast<unsigned char>(selected%n+1);refresh(cat,c);
}

#define MS_Q 16
#define MS_CIPHER "midori64"
inline Real ms_log(const Catalog&cat,int cell,unsigned a,unsigned b,const Level&l){(void)cell;return factor_log(cat,a,b,l);}
#include "../../common/native_collapsed_search.hpp"
void sweep(const Catalog&cat,State&s,const Level&l,int fixed,bool endpoints_free,uint64_t iter,RNG&g,Timer&timer){
    timer.check();const auto ce=mass_messages::setting("THERMO_COLLAPSED_EVERY",16);
    if(endpoints_free && ce && iter%ce==0 && (l.hard||l.t>=8))collapsed_update(cat,s,l,g,timer);
    if(endpoints_free){std::vector<int>order(cat.ne);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),g);for(int v:order){timer.check();gibbs(cat,s,l,{v},0,g);}if(iter%4==0){int a=draw(g,cat.ne),b;do{b=draw(g,cat.ne);}while(a==b);gibbs(cat,s,l,{a,b},0,g);}}
    for(int ci=0;ci<int(s.c.size());++ci){auto&c=s.c[ci];auto order=cat.m[c.p].used;std::shuffle(order.begin(),order.end(),g);
        for(int v:order){timer.check();gibbs(cat,s,l,{v},ci,g);}const auto&blocks=cat.m[c.p].blocks;
        if(!blocks.empty()){const auto&b=blocks[draw(g,int(blocks.size()))];if(b.size()==2||iter%16==0)gibbs(cat,s,l,b,ci,g);}
        if(fixed<0 && cat.m.size()>1)pattern_gibbs(cat,c,l,g,cat.D>0 && iter%8==0);
        std::vector<bool>used(cat.N,false);for(int v:cat.m[c.p].used)used[v]=true;
        for(int v=cat.ne;v<cat.N;++v)if(!used[v])c.x[v]=static_cast<unsigned char>(draw(g,15)+1);
    }
}
Real schedule(int k,int n){Real u=Real(k)/n;constexpr Real xs[]={0,.1L,.2L,.75L,.9L,1},ts[]={0,.75L,1,4,8,16};for(int j=1;j<6;++j)if(u<=xs[j])return ts[j-1]+(ts[j]-ts[j-1])*(u-xs[j-1])/(xs[j]-xs[j-1]);return 16;}
void ais_run(const Options&o,const Catalog&cat,Timer&timer){
    if(o.endpoint.empty()||o.pattern>=int(cat.m.size()))throw std::runtime_error("AIS requires valid endpoints/pattern");
    std::vector<Level>levels;for(int k=0;k<=o.steps;++k)levels.push_back(level(schedule(k,o.steps)));Level hard=level(16,true);
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"path_id\tlog_weight\tpattern\tendpoint\tinternal\ttrail_weight\n"<<std::setprecision(std::numeric_limits<Real>::max_digits10);
    for(uint64_t q=0;q<o.paths;++q){timer.check();uint64_t id=o.first+q;RNG rng(mix(o.seed^mix(id)));State s=random_state(cat,1,rng,o.pattern,o.endpoint);Real w=0;
        for(int k=1;k<=o.steps;++k){w+=density(cat,s,levels[k],o.pattern)-density(cat,s,levels[k-1],o.pattern);sweep(cat,s,levels[k],o.pattern,false,uint64_t(k),rng,timer);}
        int tw=weight(cat,s.c[0]);if(tw<0)w=-INF;else w+=density(cat,s,hard,o.pattern)-density(cat,s,levels.back(),o.pattern);
        if(std::isnan(w)||w==INF)throw std::runtime_error("invalid AIS weight");
        out<<id<<'\t';if(std::isfinite(w))out<<w;else out<<"-inf";out<<'\t'<<s.c[0].p<<'\t'<<o.endpoint<<'\t'<<hex(s.c[0].x,cat.ne,cat.N)<<'\t'<<tw<<'\n';
    }
    out.close();timer.check();publish(o.out);
}
#include "../../common/native_joint_search.hpp"
void search_run(const Options&o,const Catalog&cat,Timer&timer){
    if(cat.m.size()>1){joint_search_run(o,cat,timer);return;}
    RNG rng(o.seed);std::vector<Real>ts={0,.125L,.25L,.5L,.75L,1,1.25L,1.5L,1.75L,2,2.5L,3,3.5L,4,5,6,8,10,12,16};
    std::vector<Level>levels;std::vector<State>states;for(auto t:ts){levels.push_back(level(t));states.push_back(random_state(cat,o.copies,rng,-1,""));states.back().tag=int(states.size())-1;}
    struct Hit{uint64_t hard=0,soft=0;int p=0,w=0;std::string x;double first=0,last=0;};
    std::map<std::string,Hit>buffer;
    bool hard_active=false;uint64_t steps=0;std::vector<uint64_t>attempt(ts.size(),0),accepted(ts.size(),0);
    if(!o.initial.empty()){
        auto x=unhex(o.initial);if(int(x.size())!=cat.N||cat.m.size()!=1)throw std::runtime_error("bad search starting witness");
        State start=states.back();for(auto&c:start.c){c.p=0;c.x=x;refresh(cat,c);if(weight(cat,c)<0)throw std::runtime_error("illegal search starting witness");}
        states.back()=start;states.push_back(start);levels.push_back(level(16,true));hard_active=true;
    }
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"endpoint\thard_visits\tsoft_visits\tpattern\tinternal\ttrail_weight\tfirst_seconds\tlast_seconds\n"<<std::setprecision(17);
    uint64_t rows=0,observations=0,trimmed=0;

    auto trim=[&](size_t keep){
        std::vector<std::string> keys;for(const auto&e:buffer)keys.push_back(e.first);
        std::set<std::string> selected;
        std::sort(keys.begin(),keys.end(),[&](const auto&a,const auto&b){const auto&x=buffer.at(a);const auto&y=buffer.at(b);if(x.hard!=y.hard)return x.hard>y.hard;if(x.soft!=y.soft)return x.soft>y.soft;return a<b;});
        for(size_t j=0;j<keys.size()&&j<(keep+1)/2;++j)selected.insert(keys[j]);
        std::sort(keys.begin(),keys.end(),[&](const auto&a,const auto&b){auto x=key_rank(a,o.seed),y=key_rank(b,o.seed);return x!=y?x<y:a<b;});
        for(const auto&k:keys){if(selected.size()>=keep)break;selected.insert(k);}
        for(auto it=buffer.begin();it!=buffer.end();){if(!selected.count(it->first)){it=buffer.erase(it);++trimmed;}else ++it;}
    };
    auto flush=[&](){for(const auto&entry:buffer){const auto&h=entry.second;
        out<<entry.first<<'\t'<<h.hard<<'\t'<<h.soft<<'\t'<<h.p<<'\t'<<h.x<<'\t'<<h.w<<'\t'<<h.first<<'\t'<<h.last<<'\n';++rows;}
        out.flush();buffer.clear();};
    auto collect=[&](const State&s,bool hard){
        int best=-1,bw=100000;for(int i=0;i<int(s.c.size());++i){int w=weight(cat,s.c[i]);if(w>=0&&w<bw){best=i;bw=w;}}
        if(best<0)return;
        const auto&c=s.c[best];std::string e=hex(c.x,0,cat.ne);auto it=buffer.find(e);
        if(it==buffer.end()){
            if(buffer.size()>=8192)trim(4096);
            Hit h;h.p=c.p;h.w=bw;h.x=hex(c.x,cat.ne,cat.N);h.first=timer.seconds();it=buffer.emplace(e,h).first;
        }
        auto&h=it->second;if(hard)++h.hard;else ++h.soft;
        if(bw<h.w){h.p=c.p;h.w=bw;h.x=hex(c.x,cat.ne,cat.N);}h.last=timer.seconds();++observations;
    };
    try{for(uint64_t k=0;k<o.sweeps;++k){
        for(size_t lane=0;lane<states.size();++lane)sweep(cat,states[lane],levels[lane],-1,true,k,rng,timer);
        ++steps;if(k%4==0)for(size_t lane=states.size()-3;lane<states.size();++lane)collect(states[lane],levels[lane].hard);
        if(!hard_active && std::isfinite(density(cat,states.back(),level(16,true),-1))){states.push_back(states.back());levels.push_back(level(16,true));hard_active=true;}
        for(size_t j=k%2;j+1<states.size();j+=2){++attempt[j];Real d=density(cat,states[j+1],levels[j],-1)+density(cat,states[j],levels[j+1],-1)-density(cat,states[j],levels[j],-1)-density(cat,states[j+1],levels[j+1],-1);if(!std::isnan(d)&&accept(rng,d)){std::swap(states[j],states[j+1]);++accepted[j];}}
    }}catch(const Budget&){}
    trim(size_t(o.limit));flush();out.close();publish(o.out);
    std::ofstream diag(o.out+".search.json");diag<<"{"<<collapsed_diagnostics()<<"\"sweeps\":"<<steps<<",\"hard_lane_activated\":"<<(hard_active?"true":"false")<<",\"observations\":"<<observations<<",\"stream_rows\":"<<rows<<",\"archive_discarded_entries\":"<<trimmed<<",\"collection_stride\":4,\"equilibrium_certified\":false,\"first_time_is_retained_observation\":true,\"swaps\":[";
    for(size_t i=0;i<attempt.size();++i){if(i)diag<<',';diag<<"["<<attempt[i]<<','<<accepted[i]<<']';}diag<<"]}\n";
}

using Nibbles=std::array<unsigned char,16>;
Nibbles linear(const Nibbles&x){
    Nibbles y{},z{};for(int i=0;i<16;++i)y[i]=x[SR[i]];
    for(int c=0;c<4;++c){int t=y[4*c]^y[4*c+1]^y[4*c+2]^y[4*c+3];for(int r=0;r<4;++r)z[4*c+r]=static_cast<unsigned char>(t^y[4*c+r]);}
    return z;
}
Nibbles inverse_linear(const Nibbles&x){
    Nibbles y{};for(int c=0;c<4;++c){int t=x[4*c]^x[4*c+1]^x[4*c+2]^x[4*c+3];for(int r=0;r<4;++r)y[SR[4*c+r]]=static_cast<unsigned char>(t^x[4*c+r]);}
    return y;
}
std::string nibble_text(const Nibbles&x){std::string s;for(auto v:x)s+="0123456789ABCDEF"[v];return s;}
struct BridgeState{std::array<unsigned char,MAX_FACTORS>a{},b{};int distance=0;};
struct BridgeMove{struct Effect{int f;bool a,b;};std::vector<Effect>f;std::vector<int>coords;};
struct BridgeLevel{std::array<Real,17>lg{};Real lambda=0;bool hard=false;};
struct BridgeSystem{
    std::array<int,256>ddt{};std::array<bool,MAX_FACTORS>baseline{};Nibbles final_state{};int center,width;
    explicit BridgeSystem(const Options&o):center(o.center),width(o.width){
        constexpr int S[16]={12,10,13,3,14,11,15,7,8,9,1,5,0,2,4,6};
        for(int a=0;a<16;++a)for(int x=0;x<16;++x)++ddt[16*a+(S[x]^S[x^a])];
    }
    BridgeState read(const std::string&path){
        std::ifstream in(path);std::string magic,s;
        if(!(in>>magic)||magic!=BRIDGE_MAGIC)throw std::runtime_error("bad bridge header");
        std::array<Nibbles,ROUNDS+1>x{};
        for(auto&row:x){if(!(in>>s)||s.size()!=16)throw std::runtime_error("bad bridge state");
            for(int c=0;c<16;++c){auto p=std::string("0123456789ABCDEF").find(s[c]);if(p==std::string::npos)throw std::runtime_error("bad bridge nibble");row[c]=static_cast<unsigned char>(p);}}
        if(in>>s)throw std::runtime_error("extra bridge input");
        if(std::all_of(x[0].begin(),x[0].end(),[](auto v){return v==0;})||std::all_of(x[ROUNDS].begin(),x[ROUNDS].end(),[](auto v){return v==0;}))throw std::runtime_error("zero endpoints");
        final_state=x[ROUNDS];BridgeState state;
        for(int r=0;r<ROUNDS;++r){auto y=inverse_linear(x[r+1]);for(int c=0;c<16;++c){int f=16*r+c;state.a[f]=x[r][c];state.b[f]=y[c];baseline[f]=x[r][c]!=0;}}
        if(weight(state)<0)throw std::runtime_error("bridge start is not DDT legal");
        return state;
    }
    int weight(const BridgeState&s)const{
        int w=0;for(int f=0;f<MAX_FACTORS;++f){int n=ddt[16*s.a[f]+s.b[f]];if(!n)return -1;if(n==16)continue;w+=n==4?2:3;}return w;
    }
    BridgeLevel level(Real t,bool hard=false)const{
        BridgeLevel l;l.hard=hard;l.lambda=hard?1:std::min(1.0L,t/8);l.lg.fill(-INF);
        Real alpha=std::exp(-t);for(int n:{0,2,4,16})l.lg[n]=hard?(n?std::log(Real(n)/16):-INF):std::log(alpha+(1-alpha)*n)-std::log(16.0L);
        return l;
    }
    Real bias(int distance,const BridgeLevel&l)const{Real d=Real(distance-center)/width;return -.5L*l.lambda*d*d;}
    Real density(const BridgeState&s,const BridgeLevel&l)const{
        Real v=bias(s.distance,l);for(int f=0;f<MAX_FACTORS;++f)v+=l.lg[ddt[16*s.a[f]+s.b[f]]];return v;
    }
    BridgeMove move(const std::array<Nibbles,ROUNDS+1>&direction)const{
        BridgeMove m;
        for(int r=0;r<ROUNDS;++r){auto y=inverse_linear(direction[r+1]);for(int c=0;c<16;++c){int f=16*r+c;bool a=direction[r][c]!=0,b=y[c]!=0;if(a||b)m.f.push_back({f,a,b});if(a){if(r==0)throw std::runtime_error("endpoint-changing move");m.coords.push_back(f);}}}
        if(std::any_of(direction[ROUNDS].begin(),direction[ROUNDS].end(),[](auto v){return v!=0;}))throw std::runtime_error("endpoint-changing move");
        return m;
    }
    void update(BridgeState&s,const BridgeLevel&l,const BridgeMove&m,RNG&rng)const{
        std::vector<Real>scores(16);std::array<int,16>distance{};
        for(int delta=0;delta<16;++delta){int d=s.distance;
            for(int f:m.coords)d+=int(((s.a[f]^delta)!=0)!=baseline[f])-int((s.a[f]!=0)!=baseline[f]);
            distance[delta]=d;Real value=bias(d,l);
            for(auto f:m.f){unsigned a=s.a[f.f]^(f.a?delta:0),b=s.b[f.f]^(f.b?delta:0);value+=l.lg[ddt[16*a+b]];if(!std::isfinite(value))break;}
            scores[delta]=value;
        }
        int delta=choose(scores,rng);for(auto f:m.f){if(f.a)s.a[f.f]^=delta;if(f.b)s.b[f.f]^=delta;}s.distance=distance[delta];
    }
    std::string pattern(const BridgeState&s)const{
        std::string p;for(int r=0;r<ROUNDS;++r){if(r)p+=',';for(int c=0;c<16;++c)p+=(s.a[16*r+c]?'1':'0');}return p;
    }
    std::string states(const BridgeState&s)const{
        std::string text;for(int r=0;r<ROUNDS;++r){if(r)text+=',';for(int c=0;c<16;++c)text+="0123456789ABCDEF"[s.a[16*r+c]];}return text+','+nibble_text(final_state);
    }
};
void expand_run(const Options&o,Timer&timer){
    if(o.bridge.empty())throw std::runtime_error("expand requires a legal bridge witness");
    BridgeSystem sys(o);auto start=sys.read(o.bridge);RNG rng(o.seed);
    std::vector<BridgeMove>x_basis,y_basis,collective;
    for(int r=1;r<ROUNDS;++r)for(int c=0;c<16;++c){std::array<Nibbles,ROUNDS+1>d{};d[r][c]=1;x_basis.push_back(sys.move(d));d[r]=linear(d[r]);y_basis.push_back(sys.move(d));}

    for(int lo=1;lo<ROUNDS-1;++lo)for(int hi=lo+1;hi<ROUNDS;++hi)for(int c=0;c<16;++c)for(int reverse=0;reverse<2;++reverse){
        std::array<Nibbles,ROUNDS+1>d{};Nibbles value{};value[c]=1;
        for(int r=lo;r<=hi;++r){d[r]=value;value=reverse?inverse_linear(value):linear(value);}collective.push_back(sys.move(d));
    }
    std::vector<BridgeLevel>levels;for(Real t:{0.0L,.25L,.5L,1.0L,1.5L,2.0L,2.5L,3.0L,4.0L,5.0L,6.0L,8.0L,10.0L,12.0L,16.0L,20.0L,24.0L})levels.push_back(sys.level(t));
    levels.push_back(sys.level(24,true));std::vector<BridgeState>states(levels.size(),start);
    std::vector<int>order(INTERNAL_CELLS);std::iota(order.begin(),order.end(),0);
    struct Hit{std::string states;int weight=0,distance=0;uint64_t n=0;double first=0,last=0;};
    std::map<std::string,Hit>buffer;uint64_t rows=0,steps=0,legal=0,swaps=0,attempted=0;
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"pattern\tstates\ttrail_weight\tactivity_distance\tobservations\tfirst_seconds\tlast_seconds\n"<<std::setprecision(17);
    auto flush=[&](){for(const auto&e:buffer){const auto&h=e.second;out<<e.first<<'\t'<<h.states<<'\t'<<h.weight<<'\t'<<h.distance<<'\t'<<h.n<<'\t'<<h.first<<'\t'<<h.last<<'\n';++rows;}out.flush();buffer.clear();};
    uint64_t trimmed=0;
    auto trim=[&](size_t keep){std::vector<std::string>keys;for(const auto&e:buffer)keys.push_back(e.first);
        std::sort(keys.begin(),keys.end(),[&](const auto&a,const auto&b){const auto&x=buffer.at(a);const auto&y=buffer.at(b);if(x.weight!=y.weight)return x.weight<y.weight;auto ka=key_rank(a,o.seed),kb=key_rank(b,o.seed);return ka!=kb?ka<kb:a<b;});
        for(size_t j=keep;j<keys.size();++j){buffer.erase(keys[j]);++trimmed;}};
    auto collect=[&](const BridgeState&s){int w=sys.weight(s);if(w<0)return;++legal;if(s.distance==0)return;
        auto key=sys.pattern(s);auto it=buffer.find(key);if(it==buffer.end()){if(buffer.size()>=256)trim(128);Hit h;h.states=sys.states(s);h.weight=w;h.distance=s.distance;h.first=timer.seconds();it=buffer.emplace(key,h).first;}
        auto&h=it->second;++h.n;h.last=timer.seconds();if(w<h.weight){h.weight=w;h.states=sys.states(s);}
    };
    try{for(uint64_t step=0;step<o.sweeps;++step){
        for(size_t lane=0;lane<states.size();++lane){
            std::shuffle(order.begin(),order.end(),rng);const auto&basis=step%2?y_basis:x_basis;
            for(int v:order){timer.check();sys.update(states[lane],levels[lane],basis[v],rng);}
            for(int j=0;j<8;++j){timer.check();sys.update(states[lane],levels[lane],collective[draw(rng,int(collective.size()))],rng);}
        }
        ++steps;if(step%4==0)for(size_t lane=states.size()-5;lane<states.size();++lane)collect(states[lane]);
        for(size_t j=step%2;j+1<states.size();j+=2){++attempted;Real ratio=sys.density(states[j+1],levels[j])+sys.density(states[j],levels[j+1])-sys.density(states[j],levels[j])-sys.density(states[j+1],levels[j+1]);if(!std::isnan(ratio)&&accept(rng,ratio)){std::swap(states[j],states[j+1]);++swaps;}}
    }}catch(const Budget&){}
    trim(size_t(o.limit));flush();out.close();publish(o.out);
    std::ofstream diag(o.out+".expand.json");diag<<"{"<<collapsed_diagnostics()<<"\"sweeps\":"<<steps<<",\"stream_rows\":"<<rows<<",\"legal_observations\":"<<legal<<",\"archive_discarded_entries\":"<<trimmed<<",\"swap_attempts\":"<<attempted<<",\"swaps\":"<<swaps<<",\"umbrella_center\":"<<o.center<<",\"umbrella_width\":"<<o.width<<",\"exhaustive\":false,\"mass_estimated_here\":false}\n";
}
int main(int argc,char**argv){
    std::signal(SIGINT,on_signal);std::signal(SIGTERM,on_signal);Options o;Clock::time_point start=Clock::now();std::clock_t cpu=std::clock();int result=0;
    try{o=parse(argc,argv);Timer timer(o.seconds);if(o.mode=="pattern")pattern_run(o,timer);else if(o.mode=="expand")expand_run(o,timer);else{Catalog cat=load(o.model);if(o.mode=="search")search_run(o,cat,timer);else{load_domains(cat,o);ais_run(o,cat,timer);}}}
    catch(const Budget&e){std::cerr<<"INCOMPLETE: "<<e.what()<<'\n';result=2;}
    catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<'\n';result=1;}
    if(!o.out.empty()){
        long rss=0;double usage_cpu=-1;
#ifdef __linux__
        rusage usage{};if(getrusage(RUSAGE_SELF,&usage)==0){rss=usage.ru_maxrss;usage_cpu=usage.ru_utime.tv_sec+usage.ru_stime.tv_sec+(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)/1000000.0;}
#endif
        double wall=std::chrono::duration<double>(Clock::now()-start).count(),process_cpu=double(std::clock()-cpu)/CLOCKS_PER_SEC;
        bool suspect=process_cpu>wall*1.03+.01||(usage_cpu>=0&&usage_cpu>wall*1.03+.01);
        std::ofstream f(o.out+".metrics.json");f<<std::setprecision(17)<<"{\"wall_seconds\":"<<wall<<",\"cpu_seconds\":"<<process_cpu<<",\"rusage_cpu_seconds\":"<<usage_cpu<<",\"cpu_accounting_suspect\":"<<(suspect?"true":"false")<<",\"peak_rss_kib_linux\":"<<rss<<",\"exit_code\":"<<result<<"}\n";
    }
    return result;
}
