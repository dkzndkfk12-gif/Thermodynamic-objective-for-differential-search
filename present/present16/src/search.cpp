

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
#include <iterator>
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
#ifdef __linux__
#include <sys/resource.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

using Real=long double;
using Clock=std::chrono::steady_clock;
constexpr Real INF=std::numeric_limits<Real>::infinity();
constexpr int R=16, CELLS=16*R, INTERNAL=16*(R-1);
constexpr int P(int i){return i==63?63:16*i%63;}
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
uint64_t text_hash(const std::string&s,uint64_t seed){uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return mix(h^seed);}
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
    std::string mode,out,model,anchor,initial,bridge,endpoint,domains;
    uint64_t seed=1,sweeps=1000000,memory_bytes=0,corridor_nodes=0;
    int start_active=0;
    int active=32,limit=32,copies=2,center=12,width=8,paths=32,steps=256,pattern=0;
    double seconds=300;
};
Options parse(int argc,char** argv){
    if(argc<2)throw std::runtime_error("core seed|pattern|search|ensemble|ais|expand --out FILE ...");
    Options o;o.mode=argv[1];std::set<std::string> seen;
    for(int i=2;i<argc;++i){
        std::string k=argv[i];if(i+1>=argc || !seen.insert(k).second)throw std::runtime_error("missing/repeated option");
        std::string v=argv[++i];size_t used=0;
        if(k=="--out")o.out=v;else if(k=="--model")o.model=v;
        else if(k=="--anchor")o.anchor=v;
        else if(k=="--initial")o.initial=v;else if(k=="--bridge")o.bridge=v;
        else if(k=="--endpoint")o.endpoint=v;else if(k=="--domains")o.domains=v;
        else if(k=="--seconds"){o.seconds=std::stod(v,&used);if(used!=v.size())throw std::runtime_error("bad seconds");}
        else{
            if(v.empty() || v.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("expected unsigned integer: "+k);
            uint64_t n=std::stoull(v,&used);if(used!=v.size())throw std::runtime_error("bad integer");
            if(k=="--seed")o.seed=n;
            else if(k=="--sweeps")o.sweeps=n;
            else if(k=="--memory-bytes")o.memory_bytes=n;
            else if(k=="--corridor-nodes")o.corridor_nodes=n;
            else{if(n>1000000)throw std::runtime_error("option too large");
                if(k=="--active")o.active=int(n);else if(k=="--limit")o.limit=int(n);
                else if(k=="--start-active")o.start_active=int(n);
                else if(k=="--copies")o.copies=int(n);
                else if(k=="--paths")o.paths=int(n);else if(k=="--steps")o.steps=int(n);
                else if(k=="--pattern")o.pattern=int(n);
                else if(k=="--center")o.center=int(n);else if(k=="--width")o.width=int(n);
                else throw std::runtime_error("unknown option: "+k);}
        }
    }
    if(o.out.empty() || !std::isfinite(o.seconds) || o.seconds<=0 || o.active<R || o.active>CELLS ||
       o.limit<1 || o.limit>4096 || o.copies<1 || o.copies>8 ||
       !o.sweeps || o.center>INTERNAL || o.width<1 || o.width>INTERNAL ||
       o.paths<1 || o.steps<2 || o.steps>32768 || o.pattern!=0)
        throw std::runtime_error("invalid options");
    if(o.mode!="pattern" && o.mode!="search" && o.mode!="expand" && o.mode!="ais" && o.mode!="seed" && o.mode!="ensemble" && o.mode!="corridor")throw std::runtime_error("bad mode");
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

using Pattern=std::array<uint16_t,R>;
int pop(unsigned x){int n=0;while(x){x&=x-1;++n;}return n;}
Pattern read_pattern(const std::string& path){
    std::ifstream f(path);Pattern p{};std::string s;std::getline(f,s);
    std::replace(s.begin(),s.end(),',',' ');std::istringstream in(s);
    for(int r=0;r<R;++r){if(!(in>>s)||s.size()!=16)throw std::runtime_error("bad anchor");for(int c=0;c<16;++c){if(s[c]=='1')p[r]|=uint16_t(1u<<c);else if(s[c]!='0')throw std::runtime_error("bad mask");}}
    if(in>>s)throw std::runtime_error("extra anchor content");
    return p;
}
struct Support{
    std::array<std::array<bool,16>,16> possible{};
    Support(){
        constexpr int S[16]={12,5,6,11,9,0,10,13,3,14,15,8,4,7,1,2};
        bool ddt[16][16]={};
        for(int a=1;a<16;++a)for(int x=0;x<16;++x)ddt[a][S[x]^S[x^a]]=true;
        for(int in=0;in<16;++in)for(int out=0;out<16;++out)
            for(int a=1;a<16;++a)if(!(a&~in))for(int b=1;b<16;++b)
                if(!(b&~out)&&ddt[a][b])possible[in][out]=true;
    }
    int energy(const Pattern& p)const{
        int e=0;for(auto x:p)e+=x==0;

        for(int r=0;r<R-1;++r)for(int g=0;g<4;++g){int a=0,b=0;
            for(int j=0;j<4;++j){a|=((p[r]>>(4*g+j))&1)<<j;b|=((p[r+1]>>(4*j+g))&1)<<j;}
            e+=((a==0)!=(b==0));
        }

        for(int r=0;r<R;++r)for(int c=0;c<16;++c)if((p[r]>>c)&1u){
            int in=r==0?15:0,out=r==R-1?15:0;
            if(r>0)for(int bit=0;bit<4;++bit){int dst=4*c+bit,src=(dst==63?63:4*dst%63);
                if((p[r-1]>>(src/4))&1u)in|=1<<bit;}
            if(r<R-1)for(int bit=0;bit<4;++bit)if((p[r+1]>>(P(4*c+bit)/4))&1u)out|=1<<bit;
            e+=!possible[in][out];
        }
        return e;
    }
};
void pattern_run(const Options&o,Timer&timer){
    Support support;RNG rng(o.seed);bool anchored=!o.anchor.empty();Pattern base{};
    if(anchored)base=read_pattern(o.anchor);
    std::vector<int> movable;for(int r=0;r<R;++r)if(!anchored || (r>0&&r<R-1))for(int c=0;c<16;++c)movable.push_back(16*r+c);
    int fixed=anchored?pop(base[0])+pop(base[R-1]):0, need=o.active-fixed;
    if(need<0 || need>int(movable.size()))throw std::runtime_error("active count incompatible with boundary masks");
    const int lanes=24;std::vector<Pattern> states(lanes);std::vector<int> energies(lanes);
    std::vector<Real> betas(lanes);for(int i=1;i<lanes;++i)betas[i]=.125L*std::pow(128.0L,Real(i-1)/(lanes-2));
    for(int i=0;i<lanes;++i){auto p=base;if(!anchored)p.fill(0);else for(int r=1;r<R-1;++r)p[r]=0;
        auto order=movable;std::shuffle(order.begin(),order.end(),rng);for(int j=0;j<need;++j)p[order[j]/16]|=uint16_t(1u<<(order[j]%16));
        if(anchored && i==lanes-1){int n=0;for(auto m:base)n+=pop(m);if(n==o.active)p=base;}
        states[i]=p;energies[i]=support.energy(p);
    }
    std::set<Pattern> found;std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);out<<"pattern\n";
    auto collect=[&](int i){if(energies[i] || found.size()>=size_t(o.limit) || !found.insert(states[i]).second)return;
        for(int r=0;r<R;++r){if(r)out<<',';for(int c=0;c<16;++c)out<<((states[i][r]>>c)&1);}out<<'\n';out.flush();};
    for(int i=0;i<lanes;++i)collect(i);
    try{for(uint64_t step=0;step<o.sweeps && found.size()<size_t(o.limit);++step){
        timer.check();for(int i=0;i<lanes;++i){
            std::vector<int>a,z;for(int q:movable)((states[i][q/16]>>(q%16))&1?a:z).push_back(q);
            int k=1+(draw(rng,8)==0?1:0);k=std::min(k,int(std::min(a.size(),z.size())));
            std::shuffle(a.begin(),a.end(),rng);std::shuffle(z.begin(),z.end(),rng);Pattern p=states[i];
            for(int j=0;j<k;++j){p[a[j]/16]^=uint16_t(1u<<(a[j]%16));p[z[j]/16]^=uint16_t(1u<<(z[j]%16));}
            int e=support.energy(p);if(accept(rng,betas[i]*(energies[i]-e))){states[i]=p;energies[i]=e;}collect(i);
            if(step%16==0){
                int r=draw(rng,R-1),c=draw(rng,4);std::vector<int> pos;
                for(int row=0;row<4;++row){int q=16*r+4*c+row;if(!anchored||q/16>0)pos.push_back(q);q=16*(r+1)+4*row+c;if(!anchored||q/16<R-1)pos.push_back(q);}
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

struct Term{int v,src,dst;};
struct Factor{std::vector<Term>a,b;int round=0,cell=0;};
struct Incidence{int f=0;std::array<unsigned char,16>a{},b{};};
struct Model{
    std::string id;int active=0,d=0;Real log_volume=0;std::vector<int>used,moves;
    std::vector<Factor>f;std::vector<std::vector<int>>blocks;
    std::vector<std::vector<Incidence>>inc;
};
struct Catalog{int ne=0,D=0,N=0;std::vector<Model>m;std::array<int,256>ddt{};std::vector<std::vector<unsigned char>> allowed;};
void refresh_moves(Catalog& c){
    for(auto& m:c.m){
        m.moves.clear();for(int v:m.used)if(c.allowed[v].size()>1)m.moves.push_back(v);
        std::set<std::vector<int>> unique;
        for(const auto& b:m.blocks){std::vector<int> live;for(int v:b)if(c.allowed[v].size()>1)live.push_back(v);
            if(live.size()>1)unique.insert(live);}
        m.blocks.assign(unique.begin(),unique.end());
    }
}
Catalog load(const std::string&path){
    std::ifstream in(path);Catalog c;std::string magic;int k;
    if(!(in>>magic>>c.ne>>c.D>>k)||magic!="PRESENT_MF16_V2"||c.ne<2||c.ne>32||c.D<1||c.D>INTERNAL||k!=1)throw std::runtime_error("bad catalog header");
    c.N=c.ne+c.D;c.allowed.resize(c.N);
    for(auto& d:c.allowed){int n;if(!(in>>n)||n<1||n>15)throw std::runtime_error("bad bit-support domain");
        std::set<int>seen;for(int j=0;j<n;++j){int v;if(!(in>>v)||v<1||v>15||!seen.insert(v).second)throw std::runtime_error("bad domain value");d.push_back(static_cast<unsigned char>(v));}}
    constexpr int S[16]={12,5,6,11,9,0,10,13,3,14,15,8,4,7,1,2};
    for(int a=1;a<16;++a)for(int x=0;x<16;++x)++c.ddt[16*a+(S[x]^S[x^a])];
    for(int j=0;j<k;++j){Model m;int nf,nb;if(!(in>>m.id>>m.active>>m.d>>nf>>nb)||nf!=m.active||nf<R||nf>CELLS||m.d<1||m.d>c.D||nb<0||nb>300)throw std::runtime_error("bad pattern header");
        m.log_volume=0;
        std::set<int>used;for(int q=0;q<m.d;++q){int v;if(!(in>>v)||v<c.ne||v>=c.N||!used.insert(v).second)throw std::runtime_error("bad coordinate list");m.used.push_back(v);}
        for(int v:m.used)m.log_volume+=std::log(Real(c.allowed[v].size()));
        m.inc.resize(c.N);
        for(int f=0;f<nf;++f){Factor ff;int n;if(!(in>>ff.round>>ff.cell)||ff.round<0||ff.round>=R||ff.cell<0||ff.cell>=16)throw std::runtime_error("bad factor");
            for(int side=0;side<2;++side){if(!(in>>n)||n<1||n>4)throw std::runtime_error("bad bit expression");std::set<int>dest;
                for(int z=0;z<n;++z){Term t;if(!(in>>t.v>>t.src>>t.dst)||t.v<0||t.v>=c.N||(t.v>=c.ne&&!used.count(t.v))||t.src<0||t.src>3||t.dst<0||t.dst>3||!dest.insert(t.dst).second)throw std::runtime_error("bad expression bit");(side?ff.b:ff.a).push_back(t);}
            }
            for(int v=0;v<c.N;++v){Incidence e;e.f=f;bool seen=false;
                for(int side=0;side<2;++side)for(const auto&t:(side?ff.b:ff.a))if(t.v==v){seen=true;for(int delta=0;delta<16;++delta)(side?e.b:e.a)[delta]^=static_cast<unsigned char>(((delta>>t.src)&1)<<t.dst);}
                if(seen)m.inc[v].push_back(e);
            }m.f.push_back(ff);
        }
        for(int z=0;z<nb;++z){int n;if(!(in>>n)||n<2||n>3)throw std::runtime_error("bad block");std::vector<int>b;std::set<int>s;for(int q=0;q<n;++q){int v;if(!(in>>v)||!used.count(v)||!s.insert(v).second)throw std::runtime_error("bad block variable");b.push_back(v);}m.blocks.push_back(b);}c.m.push_back(std::move(m));
    }
    if(in>>magic)throw std::runtime_error("extra catalog data");
    refresh_moves(c);return c;
}
struct Level{Real t=0,lambda=0,beta=1;bool hard=false;std::array<Real,5>lg{};};
Level level(Real t,bool hard=false,Real beta=1){Level l;l.t=t;l.beta=beta;l.lambda=hard?1:t/16;l.hard=hard;l.lg.fill(-INF);Real eps=std::exp(-t);l.lg[0]=hard?-INF:-t;l.lg[2]=hard?-beta*std::log(2.0L):std::log(eps+(1-eps)*std::exp(-beta*std::log(2.0L)));l.lg[4]=0;return l;}
struct Copy{int p=0;std::vector<unsigned char>x,a,b;};
struct State{std::vector<Copy>c;int tag=0;};
unsigned eval(const std::vector<Term>&v,const std::vector<unsigned char>&x){unsigned z=0;for(const auto&t:v)z^=((x[t.v]>>t.src)&1u)<<t.dst;return z;}
void refresh(const Catalog&cat,Copy&c){const auto&m=cat.m[c.p];c.a.resize(m.f.size());c.b.resize(m.f.size());for(size_t f=0;f<m.f.size();++f){c.a[f]=static_cast<unsigned char>(eval(m.f[f].a,c.x));c.b[f]=static_cast<unsigned char>(eval(m.f[f].b,c.x));}}
Real factor_log(const Catalog&cat,unsigned a,unsigned b,const Level&l){return l.lg[(a&&b)?cat.ddt[16*a+b]:0];}
Real constant(const Catalog&cat,int p,const Level&l,int fixed){const auto&m=cat.m[p];Real logk=fixed<0?std::log(Real(cat.m.size())):0;return -logk+l.lambda*(m.log_volume-2*l.beta*m.active*std::log(2.0L)+logk);}
Real density_copy(const Catalog&cat,const Copy&c,const Level&l,int fixed){Real s=constant(cat,c.p,l,fixed);for(size_t f=0;f<c.a.size();++f)s+=factor_log(cat,c.a[f],c.b[f],l);return s;}
Real density(const Catalog&cat,const State&s,const Level&l,int fixed){Real v=0;for(const auto&c:s.c)v+=density_copy(cat,c,l,fixed);return v;}
int weight(const Catalog&cat,const Copy&c){int w=0;for(size_t f=0;f<c.a.size();++f){int n=(c.a[f]&&c.b[f])?cat.ddt[16*c.a[f]+c.b[f]]:0;if(!n)return -1;w+=n==4?2:3;}return w;}
State random_state(const Catalog&cat,int copies,RNG&g,int fixed,const std::string&end){
    State s;std::vector<unsigned char>e=unhex(end);if(e.empty())for(int i=0;i<cat.ne;++i)e.push_back(cat.allowed[i][draw(g,int(cat.allowed[i].size()))]);if(int(e.size())!=cat.ne)throw std::runtime_error("endpoint length mismatch");
    for(int i=0;i<cat.ne;++i)if(std::find(cat.allowed[i].begin(),cat.allowed[i].end(),e[i])==cat.allowed[i].end())throw std::runtime_error("endpoint domain mismatch");
    for(int j=0;j<copies;++j){Copy c;c.p=fixed<0?draw(g,int(cat.m.size())):fixed;c.x=e;for(int i=cat.ne;i<cat.N;++i)c.x.push_back(cat.allowed[i][draw(g,int(cat.allowed[i].size()))]);refresh(cat,c);s.c.push_back(std::move(c));}return s;
}

void gibbs(const Catalog&cat,State&s,const Level&l,const std::vector<int>&vars,int chosen,RNG&g){
    bool shared=false;for(int v:vars)shared|=v<cat.ne;
    int begin=shared?0:chosen,end=shared?int(s.c.size()):chosen+1,n=1;for(int v:vars)n*=int(cat.allowed[v].size());
    struct E{int f=0;unsigned char a[3][16]={},b[3][16]={};};std::vector<std::vector<E>>effects(s.c.size());
    for(int ci=begin;ci<end;++ci){std::map<int,E>mp;const auto&m=cat.m[s.c[ci].p];
        for(size_t j=0;j<vars.size();++j)if(vars[j]<cat.ne||ci==chosen)for(auto e:m.inc[vars[j]]){auto&z=mp[e.f];z.f=e.f;for(int delta=0;delta<16;++delta){z.a[j][delta]^=e.a[delta];z.b[j][delta]^=e.b[delta];}}
        for(auto z:mp)effects[ci].push_back(z.second);
    }
    std::vector<Real>scores(n);for(int code=0;code<n;++code){int z=code;std::array<unsigned,3>val{};for(size_t j=0;j<vars.size();++j){const auto&d=cat.allowed[vars[j]];val[j]=d[z%int(d.size())];z/=int(d.size());}Real sc=0;
        for(int ci=begin;ci<end && std::isfinite(sc);++ci){const auto&c=s.c[ci];for(auto e:effects[ci]){unsigned a=c.a[e.f],b=c.b[e.f];for(size_t j=0;j<vars.size();++j){unsigned d=c.x[vars[j]]^val[j];a^=e.a[j][d];b^=e.b[j][d];}sc+=factor_log(cat,a,b,l);if(!std::isfinite(sc))break;}}scores[code]=sc;
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
void sweep(const Catalog&cat,State&s,const Level&l,int fixed,bool endpoints_free,uint64_t iter,RNG&g,Timer&timer){
    timer.check();if(endpoints_free){std::vector<int>order(cat.ne);std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),g);for(int v:order){timer.check();gibbs(cat,s,l,{v},0,g);}if(iter%4==0){int a=draw(g,cat.ne),b;do{b=draw(g,cat.ne);}while(a==b);gibbs(cat,s,l,{a,b},0,g);}}
    for(int ci=0;ci<int(s.c.size());++ci){auto&c=s.c[ci];auto order=cat.m[c.p].moves;std::shuffle(order.begin(),order.end(),g);
        for(int v:order){timer.check();gibbs(cat,s,l,{v},ci,g);}const auto&blocks=cat.m[c.p].blocks;
        if(!blocks.empty()){const auto&b=blocks[draw(g,int(blocks.size()))];if(b.size()==2||iter%16==0)gibbs(cat,s,l,b,ci,g);}
        if(fixed<0 && cat.m.size()>1)pattern_gibbs(cat,c,l,g,iter%8==0);
        std::vector<bool>used(cat.N,false);for(int v:cat.m[c.p].used)used[v]=true;
        for(int v=cat.ne;v<cat.N;++v)if(!used[v])c.x[v]=static_cast<unsigned char>(draw(g,15)+1);
    }
}
void search_run(const Options&o,const Catalog&cat,Timer&timer){
    RNG rng(o.seed);std::vector<Real>ts={0,.125L,.25L,.5L,.75L,1,1.25L,1.5L,1.75L,2,2.5L,3,3.5L,4,5,6,8,10,12,16};
    std::vector<Level>levels;std::vector<State>states;for(auto t:ts){levels.push_back(level(t));states.push_back(random_state(cat,o.copies,rng,-1,""));states.back().tag=int(states.size())-1;}
    struct Hit{uint64_t count=0;int p=0,w=0;std::string x;double first=0,last=0;};

    std::map<std::string,Hit>hard_hits,soft_hits,reservoir;
    std::map<std::pair<uint64_t,std::string>,std::string>hash_order;
    uint64_t hard_reductions=0,soft_reductions=0;
    bool hard_active=false;uint64_t steps=0;std::vector<uint64_t>attempt(ts.size(),0),accepted(ts.size(),0);
    if(!o.initial.empty()){
        auto x=unhex(o.initial);if(int(x.size())!=cat.N||cat.m.size()!=1)throw std::runtime_error("bad search starting witness");
        State start=states.back();for(auto&c:start.c){c.p=0;c.x=x;refresh(cat,c);if(weight(cat,c)<0)throw std::runtime_error("illegal search starting witness");}
        states.back()=start;states.push_back(start);levels.push_back(level(16,true));hard_active=true;
    }
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"endpoint\thard_visits\tsoft_visits\tpattern\tinternal\ttrail_weight\tfirst_seconds\tlast_seconds\n"<<std::setprecision(17);
    uint64_t rows=0,observations=0;
    auto update=[&](Hit&h,const Copy&c,int w){
        if(h.count==0){h.p=c.p;h.w=w;h.x=hex(c.x,cat.ne,cat.N);h.first=timer.seconds();}
        else if(w<h.w){h.p=c.p;h.w=w;h.x=hex(c.x,cat.ne,cat.N);}
        ++h.count;h.last=timer.seconds();
    };
    auto collect=[&](const State&s,bool hard){
        int best=-1,bw=100000;for(int i=0;i<int(s.c.size());++i){int w=weight(cat,s.c[i]);if(w>=0&&w<bw){best=i;bw=w;}}
        if(best<0)return;
        const auto&c=s.c[best];std::string e=hex(c.x,0,cat.ne);
        auto& pool=hard?hard_hits:soft_hits;auto it=pool.find(e);
        if(it!=pool.end()||pool.size()<4096)update(pool[e],c,bw);
        else{
            if(hard)++hard_reductions;else ++soft_reductions;
            for(auto j=pool.begin();j!=pool.end();){if(--j->second.count==0)j=pool.erase(j);else ++j;}
        }
        auto key=std::make_pair(text_hash(e,o.seed),e);
        if(reservoir.count(e))update(reservoir[e],c,bw);
        else if(reservoir.size()<64 || key<hash_order.rbegin()->first){
            if(reservoir.size()==64){auto last=std::prev(hash_order.end());reservoir.erase(last->second);hash_order.erase(last);}
            update(reservoir[e],c,bw);hash_order.emplace(key,e);
        }
        ++observations;
    };
    try{for(uint64_t k=0;k<o.sweeps;++k){
        for(size_t lane=0;lane<states.size();++lane)sweep(cat,states[lane],levels[lane],-1,true,k,rng,timer);
        ++steps;if(k%4==0)for(size_t lane=states.size()-3;lane<states.size();++lane)collect(states[lane],levels[lane].hard);
        if(!hard_active && std::isfinite(density(cat,states.back(),level(16,true),-1))){states.push_back(states.back());levels.push_back(level(16,true));hard_active=true;}
        for(size_t j=k%2;j+1<states.size();j+=2){++attempt[j];Real d=density(cat,states[j+1],levels[j],-1)+density(cat,states[j],levels[j+1],-1)-density(cat,states[j],levels[j],-1)-density(cat,states[j+1],levels[j+1],-1);if(!std::isnan(d)&&accept(rng,d)){std::swap(states[j],states[j+1]);++accepted[j];}}
    }}catch(const Budget&){}
    std::set<std::string>selected;
    auto emit=[&](const std::string&e,const Hit&h){if(selected.size()>=size_t(o.limit)||!selected.insert(e).second)return;
        auto a=hard_hits.find(e),b=soft_hits.find(e);
        out<<e<<'\t'<<(a==hard_hits.end()?0:a->second.count)<<'\t'<<(b==soft_hits.end()?0:b->second.count)<<'\t'<<h.p<<'\t'<<h.x<<'\t'<<h.w<<'\t'<<h.first<<'\t'<<h.last<<'\n';++rows;};
    auto take=[&](const std::map<std::string,Hit>&pool,size_t n){
        std::vector<std::string>order;for(const auto&e:pool)order.push_back(e.first);
        std::sort(order.begin(),order.end(),[&](const auto&a,const auto&b){return pool.at(a).count!=pool.at(b).count?pool.at(a).count>pool.at(b).count:a<b;});
        size_t old=selected.size();for(const auto&e:order){if(selected.size()-old>=n)break;emit(e,pool.at(e));}
    };
    take(hard_hits,size_t(std::max(1,o.limit/2)));take(soft_hits,size_t(std::max(1,o.limit/4)));
    size_t before=selected.size();for(const auto&e:hash_order){if(selected.size()-before>=size_t(std::max(1,o.limit/4)))break;emit(e.second,reservoir.at(e.second));}
    take(hard_hits,size_t(o.limit));take(soft_hits,size_t(o.limit));
    for(const auto&e:hash_order)emit(e.second,reservoir.at(e.second));
    out.close();publish(o.out);
    std::ofstream diag(o.out+".search.json");diag<<"{\"sweeps\":"<<steps<<",\"hard_lane_activated\":"<<(hard_active?"true":"false")<<",\"observations\":"<<observations<<",\"selected_endpoints\":"<<rows<<",\"hard_counter_reductions\":"<<hard_reductions<<",\"soft_counter_reductions\":"<<soft_reductions<<",\"visits_are_probability\":false,\"collection_stride\":4,\"equilibrium_certified\":false,\"swaps\":[";
    for(size_t i=0;i<attempt.size();++i){if(i)diag<<',';diag<<"["<<attempt[i]<<','<<accepted[i]<<']';}diag<<"]}\n";
}

void load_domains(Catalog& cat,const Options& o){
    if(o.domains.empty()||o.endpoint.empty())throw std::runtime_error("AIS requires fixed endpoints and domains");
    std::ifstream in(o.domains);std::string magic;int k=0,n=0;
    if(!(in>>magic>>k>>n)||magic!="PRESENT_DOMAINS_V2"||k!=0||cat.m.size()!=1||n!=cat.m[0].d)throw std::runtime_error("invalid AIS domains");
    std::set<int> seen;
    for(int j=0;j<n;++j){int v=0,count=0;if(!(in>>v>>count)||v<cat.ne||v>=cat.N||count<1||count>15||!seen.insert(v).second)throw std::runtime_error("invalid AIS domain coordinate");
        std::vector<unsigned char> d;std::set<int> unique;
        for(int i=0;i<count;++i){int x=0;if(!(in>>x)||x<1||x>15||!unique.insert(x).second||std::find(cat.allowed[v].begin(),cat.allowed[v].end(),x)==cat.allowed[v].end())throw std::runtime_error("AIS domain is not a support subset");d.push_back(static_cast<unsigned char>(x));}
        cat.allowed[v]=std::move(d);
    }
    if(in>>magic)throw std::runtime_error("extra AIS domains");
    cat.m[0].log_volume=0;for(int v:cat.m[0].used)cat.m[0].log_volume+=std::log(Real(cat.allowed[v].size()));
    refresh_moves(cat);
}
void ais_run(const Options& o,Catalog cat,Timer& timer){
    load_domains(cat,o);RNG rng(o.seed);
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"path_id\tlog_weight\tpattern\tendpoint\tinternal\ttrail_weight\n"<<std::setprecision(21);

    for(int path=0;path<o.paths;++path){timer.check();State s=random_state(cat,1,rng,0,o.endpoint);Level old=level(0);Real logw=0;
        for(int k=1;k<=o.steps;++k){timer.check();Level next=k==o.steps?level(16,true):level(16*Real(k)/(o.steps-1));
            logw+=density(cat,s,next,0)-density(cat,s,old,0);
            if(k<o.steps)sweep(cat,s,next,0,false,uint64_t(k),rng,timer);
            old=next;
        }
        const auto& c=s.c[0];out<<path<<'\t'<<logw<<"\t0\t"<<hex(c.x,0,cat.ne)<<'\t'<<hex(c.x,cat.ne,cat.N)<<'\t'<<weight(cat,c)<<'\n';
    }
    out.close();publish(o.out);
}

using Nibbles=std::array<unsigned char,16>;
Nibbles linear(const Nibbles&x){Nibbles y{};for(int i=0;i<64;++i)y[P(i)/4]^=static_cast<unsigned char>(((x[i/4]>>(i%4))&1)<<(P(i)%4));return y;}
Nibbles inverse_linear(const Nibbles&x){Nibbles y{};for(int i=0;i<64;++i)y[i/4]^=static_cast<unsigned char>(((x[P(i)/4]>>(P(i)%4))&1)<<(i%4));return y;}
std::string nibble_text(const Nibbles&x){std::string s;for(int c=15;c>=0;--c)s+="0123456789ABCDEF"[x[c]];return s;}

void seed_run(const Options& o,Timer& timer){
    constexpr int S[16]={12,5,6,11,9,0,10,13,3,14,15,8,4,7,1,2};int ddt[16][16]{};
    for(int a=0;a<16;++a)for(int x=0;x<16;++x)++ddt[a][S[x]^S[x^a]];
    RNG rng(o.seed);struct Hit{std::array<Nibbles,R+1> x{};int w=0;double first=0;};std::map<std::string,Hit> hits;
    try{for(uint64_t attempt=0;attempt<o.sweeps;++attempt){timer.check();bool reverse=(attempt&1)!=0;Hit h;
        std::vector<int> positions(16);std::iota(positions.begin(),positions.end(),0);std::shuffle(positions.begin(),positions.end(),rng);
        int count=1+draw(rng,4),common=1+draw(rng,15),active=0;auto& boundary=h.x[reverse?R:0];
        for(int j=0;j<count;++j)boundary[positions[j]]=static_cast<unsigned char>(attempt%3?common:1+draw(rng,15));
        bool retain=true;
        for(int step=0;step<R;++step){int r=reverse?R-1-step:step;Nibbles fixed=reverse?inverse_linear(h.x[r+1]):h.x[r], next{};
            for(int c=0;c<16;++c)if(fixed[c]){
                ++active;std::vector<int> choices;std::vector<Real> scores;

                Real bias=attempt%5==0?0.5L:4.0L;
                for(int z=1;z<16;++z){int n=reverse?ddt[z][fixed[c]]:ddt[fixed[c]][z];if(n){choices.push_back(z);scores.push_back(-bias*pop(unsigned(z))+std::log(Real(n)));}}
                int z=choices[size_t(choose(scores,rng))];next[c]=static_cast<unsigned char>(z);h.w+=(reverse?ddt[z][fixed[c]]:ddt[fixed[c]][z])==4?2:3;
            }
            if(active+R-step-1>o.active){retain=false;break;}
            h.x[reverse?r:r+1]=reverse?next:linear(next);
        }
        if(!retain)continue;
        std::string key;for(int r=0;r<R;++r)for(int c=0;c<16;++c)key+=h.x[r][c]?'1':'0';
        auto old=hits.find(key);h.first=timer.seconds();
        if(old!=hits.end()){if(h.w<old->second.w)old->second=h;continue;}
        if(hits.size()>=size_t(o.limit)){
            auto worst=std::max_element(hits.begin(),hits.end(),[](const auto&a,const auto&b){return a.second.w<b.second.w;});

            if(h.w>=worst->second.w&&attempt%17)continue;
            hits.erase(worst);
        }
        hits.emplace(key,h);
    }}catch(const Budget&){}
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);out<<"states\tfirst_seconds\n"<<std::setprecision(17);
    for(const auto& p:hits){for(int r=0;r<=R;++r){if(r)out<<',';out<<nibble_text(p.second.x[r]);}out<<'\t'<<p.second.first<<'\n';}
    out.close();publish(o.out);
}
struct BridgeState{std::array<unsigned char,CELLS>a{},b{};int distance=0;};

using Directions=std::array<std::array<Nibbles,R+1>,4>;
struct BridgeMove{struct Effect{int f=0;std::array<unsigned char,16>a{},b{};};std::vector<Effect>f;};
struct BridgeLevel{std::array<Real,17>lg{};Real lambda=0;bool hard=false;};
struct BridgeSystem{
    std::array<int,256>ddt{};std::array<bool,CELLS>baseline{};Nibbles final_state{};int center,width;
    explicit BridgeSystem(const Options&o):center(o.center),width(o.width){
        constexpr int S[16]={12,5,6,11,9,0,10,13,3,14,15,8,4,7,1,2};
        for(int a=0;a<16;++a)for(int x=0;x<16;++x)++ddt[16*a+(S[x]^S[x^a])];
    }
    BridgeState read(const std::string&path){
        std::ifstream in(path);std::string magic,s;
        if(!(in>>magic)||magic!="PRESENT_BRIDGE16_V2")throw std::runtime_error("bad bridge header");
        std::array<Nibbles,R+1>x{};
        for(auto&row:x){if(!(in>>s)||s.size()!=16)throw std::runtime_error("bad bridge state");
            for(int c=0;c<16;++c){auto p=std::string("0123456789ABCDEF").find(s[15-c]);if(p==std::string::npos)throw std::runtime_error("bad bridge nibble");row[c]=static_cast<unsigned char>(p);}}
        if(in>>s)throw std::runtime_error("extra bridge input");
        if(std::all_of(x[0].begin(),x[0].end(),[](auto v){return v==0;})||std::all_of(x[R].begin(),x[R].end(),[](auto v){return v==0;}))throw std::runtime_error("zero endpoints");
        final_state=x[R];BridgeState state;
        for(int r=0;r<R;++r){auto y=inverse_linear(x[r+1]);for(int c=0;c<16;++c){int f=16*r+c;state.a[f]=x[r][c];state.b[f]=y[c];baseline[f]=x[r][c]!=0;}}
        if(weight(state)<0)throw std::runtime_error("bridge start is not DDT legal");
        return state;
    }
    int weight(const BridgeState&s)const{
        int w=0;for(int f=0;f<CELLS;++f){int n=ddt[16*s.a[f]+s.b[f]];if(!n)return -1;if(n==16)continue;w+=n==4?2:3;}return w;
    }
    BridgeLevel level(Real t,bool hard=false)const{
        BridgeLevel l;l.hard=hard;l.lambda=hard?1:std::min(1.0L,t/8);l.lg.fill(-INF);
        Real alpha=std::exp(-t);for(int n:{0,2,4,16})l.lg[n]=hard?(n?std::log(Real(n)/16):-INF):std::log(alpha+(1-alpha)*n)-std::log(16.0L);
        return l;
    }
    Real bias(int distance,const BridgeLevel&l)const{Real d=Real(distance-center)/width;return -.5L*l.lambda*d*d;}
    Real density(const BridgeState&s,const BridgeLevel&l)const{
        Real v=bias(s.distance,l);for(int f=0;f<CELLS;++f)v+=l.lg[ddt[16*s.a[f]+s.b[f]]];return v;
    }
    BridgeMove move(const Directions&directions)const{
        std::map<int,BridgeMove::Effect> effects;
        for(int bit=0;bit<4;++bit){const auto&direction=directions[bit];
            for(int boundary:{0,R})if(std::any_of(direction[boundary].begin(),direction[boundary].end(),[](auto v){return v!=0;}))throw std::runtime_error("endpoint-changing move");
            for(int r=0;r<R;++r){auto y=inverse_linear(direction[r+1]);for(int c=0;c<16;++c){int f=16*r+c;
                if(direction[r][c]||y[c]){auto&e=effects[f];e.f=f;for(int delta=0;delta<16;++delta)if((delta>>bit)&1){e.a[delta]^=direction[r][c];e.b[delta]^=y[c];}}
            }}
        }
        BridgeMove m;for(const auto&entry:effects)m.f.push_back(entry.second);return m;
    }
    void update(BridgeState&s,const BridgeLevel&l,const BridgeMove&m,RNG&rng)const{
        std::vector<Real>scores(16);std::array<int,16>distance{};
        for(int delta=0;delta<16;++delta){int d=s.distance;
            for(const auto&e:m.f)if(e.a[delta])d+=int(((s.a[e.f]^e.a[delta])!=0)!=baseline[e.f])-int((s.a[e.f]!=0)!=baseline[e.f]);
            distance[delta]=d;Real value=bias(d,l);
            for(const auto&e:m.f){unsigned a=s.a[e.f]^e.a[delta],b=s.b[e.f]^e.b[delta];value+=l.lg[ddt[16*a+b]];if(!std::isfinite(value))break;}
            scores[delta]=value;
        }
        int delta=choose(scores,rng);for(const auto&e:m.f){s.a[e.f]^=e.a[delta];s.b[e.f]^=e.b[delta];}s.distance=distance[delta];
    }
    std::string pattern(const BridgeState&s)const{
        std::string p;for(int r=0;r<R;++r){if(r)p+=',';for(int c=0;c<16;++c)p+=(s.a[16*r+c]?'1':'0');}return p;
    }
    std::string states(const BridgeState&s)const{
        std::string text;for(int r=0;r<R;++r){if(r)text+=',';for(int c=15;c>=0;--c)text+="0123456789ABCDEF"[s.a[16*r+c]];}return text+','+nibble_text(final_state);
    }
};
void expand_run(const Options&o,Timer&timer){
    if(o.bridge.empty())throw std::runtime_error("expand requires a legal bridge witness");
    BridgeSystem sys(o);auto start=sys.read(o.bridge);RNG rng(o.seed);
    std::vector<BridgeMove>x_basis,y_basis,collective;
    for(int r=1;r<R;++r)for(int c=0;c<16;++c){Directions dx{},dy{};
        for(int bit=0;bit<4;++bit){dx[bit][r][c]=static_cast<unsigned char>(1u<<bit);dy[bit][r]=linear(dx[bit][r]);}
        x_basis.push_back(sys.move(dx));y_basis.push_back(sys.move(dy));
    }

    for(int lo=1;lo<R-1;++lo)for(int hi=lo+1;hi<R;++hi)for(int c=0;c<16;++c)for(int reverse=0;reverse<2;++reverse){
        timer.check();
        Directions d{};for(int bit=0;bit<4;++bit){Nibbles value{};value[c]=static_cast<unsigned char>(1u<<bit);
            for(int r=lo;r<=hi;++r){d[bit][r]=value;value=reverse?inverse_linear(value):linear(value);}}
        collective.push_back(sys.move(d));
    }
    std::vector<BridgeLevel>levels;for(Real t:{0.0L,.25L,.5L,1.0L,1.5L,2.0L,2.5L,3.0L,4.0L,5.0L,6.0L,8.0L,10.0L,12.0L,16.0L,20.0L,24.0L})levels.push_back(sys.level(t));
    levels.push_back(sys.level(24,true));std::vector<BridgeState>states(levels.size(),start);
    std::vector<int>order(INTERNAL);std::iota(order.begin(),order.end(),0);
    struct Hit{std::string states;int weight=0,distance=0;uint64_t n=0;double first=0,last=0;};
    std::map<std::string,Hit>buffer;std::map<std::pair<uint64_t,std::string>,std::string>kept;
    uint64_t rows=0,steps=0,legal=0,swaps=0,attempted=0;
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"pattern\tstates\ttrail_weight\tactivity_distance\tobservations\tfirst_seconds\tlast_seconds\n"<<std::setprecision(17);
    auto flush=[&](){for(const auto&e:buffer){const auto&h=e.second;out<<e.first<<'\t'<<h.states<<'\t'<<h.weight<<'\t'<<h.distance<<'\t'<<h.n<<'\t'<<h.first<<'\t'<<h.last<<'\n';++rows;}out.flush();buffer.clear();};
    auto collect=[&](const BridgeState&s){int w=sys.weight(s);if(w<0)return;++legal;if(s.distance==0)return;
        auto key=sys.pattern(s);auto it=buffer.find(key);if(it==buffer.end()){
            auto rank=std::make_pair(text_hash(key,o.seed),key);
            if(buffer.size()>=size_t(o.limit)){
                auto last=std::prev(kept.end());if(rank>=last->first)return;
                buffer.erase(last->second);kept.erase(last);
            }
            kept.emplace(rank,key);Hit h;h.states=sys.states(s);h.weight=w;h.distance=s.distance;h.first=timer.seconds();it=buffer.emplace(key,h).first;
        }
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
    flush();out.close();publish(o.out);
    std::ofstream diag(o.out+".expand.json");diag<<"{\"sweeps\":"<<steps<<",\"stream_rows\":"<<rows<<",\"legal_observations\":"<<legal<<",\"swap_attempts\":"<<attempted<<",\"swaps\":"<<swaps<<",\"umbrella_center\":"<<o.center<<",\"umbrella_width\":"<<o.width<<",\"exhaustive\":false,\"mass_estimated_here\":false}\n";
}
#include "ensemble.hpp"
#include "sparse_corridor.hpp"

int main(int argc,char**argv){
    std::signal(SIGINT,on_signal);std::signal(SIGTERM,on_signal);Options o;Clock::time_point start=Clock::now();std::clock_t cpu=std::clock();int result=0;
#ifdef __linux__
    const auto parent=getppid();if(parent<=1)return 2;prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()!=parent)return 2;
#endif
    try{o=parse(argc,argv);Timer timer(o.seconds);if(o.mode=="pattern")pattern_run(o,timer);else if(o.mode=="seed")seed_run(o,timer);else if(o.mode=="expand")expand_run(o,timer);else if(o.mode=="ensemble")present_ensemble::run(o,timer);else if(o.mode=="corridor")present_corridor::run(o,timer);else{Catalog cat=load(o.model);if(o.mode=="ais")ais_run(o,std::move(cat),timer);else search_run(o,cat,timer);}}
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
