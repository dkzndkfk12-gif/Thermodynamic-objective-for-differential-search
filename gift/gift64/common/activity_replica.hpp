#pragma once
#include "model.hpp"
#include <random>

namespace thermo {
namespace activity_walk {
using Cut=std::vector<int>;
using Trail=std::vector<Cut>;
using Replicas=std::vector<Trail>;

struct Direction {
    std::array<Trail,4> basis;
    bool operator==(const Direction &o) const { return basis==o.basis; }
};
inline Direction zero_direction(const Cipher &c) {
    Direction d;for(auto &b:d.basis)b=Trail(c.R+1,Cut(c.N));return d;
}
inline unsigned effect(const Direction &d,int r,int cell,unsigned amplitude) {
    unsigned value=0;
    for(unsigned bit=0;bit<4;++bit)if(amplitude&(1u<<bit))value^=d.basis[bit][r][cell];
    return value;
}
inline double log_add(double a,double b) {
    if(!std::isfinite(a))return b;
    if(!std::isfinite(b))return a;
    if(a<b)std::swap(a,b);
    return a+std::log1p(std::exp(b-a));
}
inline unsigned draw_log(const std::vector<double> &weights,std::mt19937_64 &rng) {
    auto hi=*std::max_element(weights.begin(),weights.end());
    require(std::isfinite(hi),"Empty activity-changing heat bath");
    double total=0;for(auto w:weights)total+=std::exp(w-hi);
    double u=std::generate_canonical<double,53>(rng)*total;
    unsigned last=0;
    for(unsigned i=0;i<weights.size();++i)if(std::isfinite(weights[i])) {
        last=i;u-=std::exp(weights[i]-hi);if(u<0)return i;
    }
    return last;
}
inline double log_factor(int count,int q,double alpha) {
    auto n=alpha+(1-alpha)*count;
    return n>0?std::log(n)-std::log(double(q)):-INFINITY;
}
inline bool nonzero(const Cut &x) {return std::any_of(x.begin(),x.end(),[](int v){return v!=0;});}
inline double log_density(const Cipher &c,const Replicas &rep,double alpha) {
    double logp=0;
    for(const auto &x:rep) {
        require(int(x.size())==c.R+1,"Activity replica round mismatch");
        require(x.front()==rep.front().front() && x.back()==rep.front().back(),"Activity replicas do not share endpoints");
        if(!nonzero(x.front()) || !nonzero(x.back()))return -INFINITY;
        for(int r=0;r<c.R;++r) {
            auto b=c.inverse(x[r+1]);
            for(int i=0;i<c.N;++i) {
                auto l=log_factor(c.count(c.kind(i),x[r][i],b[i]),c.Q,alpha);
                if(!std::isfinite(l))return -INFINITY;
                logp+=l;
            }
        }
    }
    return logp;
}
inline Direction direction(const Cipher &c,std::mt19937_64 &rng,int boundary=-1) {
    require(c.present && c.B==4,"GIFT activity kernel requires 4-bit bit-permutation cipher");
    auto d=zero_direction(c);

    int pivot=boundary>=0?boundary:1+int(rng()%(c.R-1));
    int cell=int(rng()%c.N);
    int start=pivot,end=pivot;
    if(rng()&1) {
        int lo=boundary==0?0:1,hi=boundary==c.R?c.R:c.R-1;
        start=lo+int(rng()%(pivot-lo+1));end=pivot+int(rng()%(hi-pivot+1));
    }
    for(unsigned bit=0;bit<4;++bit) {
        auto &b=d.basis[bit];b[pivot][cell]=int(1u<<bit);
        for(int r=pivot;r<end;++r)b[r+1]=c.forward(b[r]);
        for(int r=pivot;r>start;--r)b[r-1]=c.inverse(b[r]);
    }
    return d;
}
inline std::array<Direction,2> private_directions(const Cipher &c,std::mt19937_64 &rng) {
    auto a=direction(c,rng),b=direction(c,rng);
    while(b==a)b=direction(c,rng);
    return {std::move(a),std::move(b)};
}
struct LocalFactor {
    int kind=0,a=0,b=0;
    std::array<std::array<unsigned,16>,3> da{},db{};
};
class Orbit {
    const Cipher &c;
    const Trail &x;
    std::array<Direction,3> dirs;
    std::vector<LocalFactor> factors;
    std::vector<double> factor_log;
  public:
    std::vector<double> logweights,marginal;
    Orbit(const Cipher &cc,const Trail &xx,Direction dc,Direction a,Direction b,unsigned sh,double al)
        :c(cc),x(xx),dirs{std::move(dc),std::move(a),std::move(b)} {
        require(sh==0,"GIFT has 4-bit cells");
        if(dirs[0].basis[0].empty())dirs[0]=zero_direction(c);
        factor_log.resize(c.Q+1);
        for(int n=0;n<=c.Q;++n)factor_log[n]=log_factor(n,c.Q,al);
        for(int r=0;r<c.R;++r) {
            auto y=c.inverse(x[r+1]);
            std::array<std::array<Cut,4>,3> inverse;
            for(unsigned d=0;d<3;++d)for(unsigned k=0;k<4;++k)
                inverse[d][k]=c.inverse(dirs[d].basis[k][r+1]);
            for(int i=0;i<c.N;++i) {
                LocalFactor f;f.kind=c.kind(i);f.a=x[r][i];f.b=y[i];bool affected=false;
                for(unsigned d=0;d<3;++d)for(unsigned v=0;v<16;++v) {
                    f.da[d][v]=effect(dirs[d],r,i,v);
                    for(unsigned k=0;k<4;++k)if(v&(1u<<k))f.db[d][v]^=inverse[d][k][i];
                    affected|=f.da[d][v]||f.db[d][v];
                }
                if(affected)factors.push_back(std::move(f));
            }
        }
    }
    void sum(bool shared,const Deadline &dl) {
        unsigned extent=shared?16:1;
        logweights.assign(extent*256,-INFINITY);marginal.assign(extent,-INFINITY);
        for(unsigned u=0;u<extent;++u) {
            bool valid=true;
            for(int r:{0,c.R}) {
                bool nz=false;
                for(int i=0;i<c.N;++i)nz|=(x[r][i]^effect(dirs[0],r,i,u))!=0;
                valid&=nz;
            }
            if(!valid)continue;
            for(unsigned v=0;v<16;++v)for(unsigned w=0;w<16;++w) {
                unsigned index=u*256+v*16+w;if(!(index%64))dl.check();
                double l=0;
                for(const auto &f:factors) {
                    auto a=f.a^f.da[0][u]^f.da[1][v]^f.da[2][w];
                    auto b=f.b^f.db[0][u]^f.db[1][v]^f.db[2][w];
                    auto lf=factor_log[c.count(f.kind,a,b)];
                    if(!std::isfinite(lf)){l=-INFINITY;break;}l+=lf;
                }
                logweights[index]=l;
            }
            auto first=logweights.begin()+u*256;
            double high=*std::max_element(first,first+256);
            if(std::isfinite(high)) {
                double total=0;for(auto i=first;i!=first+256;++i)total+=std::exp(*i-high);
                marginal[u]=high+std::log(total);
            }
        }
    }
    Trail point(unsigned u,unsigned v,unsigned w) const {
        auto y=x;
        for(int r=0;r<=c.R;++r)for(int i=0;i<c.N;++i)
            y[r][i]^=effect(dirs[0],r,i,u)^effect(dirs[1],r,i,v)^effect(dirs[2],r,i,w);
        return y;
    }
    Trail draw(unsigned u,std::mt19937_64 &rng) const {
        auto begin=logweights.begin()+u*256;
        unsigned vw=draw_log(std::vector<double>(begin,begin+256),rng);
        return point(u,vw/16,vw%16);
    }
};

inline bool step(const Cipher &c,Replicas &rep,double alpha,std::mt19937_64 &rng,
                 bool shared,const Deadline &dl) {
    unsigned shift=c.B==8 && (rng()&1)?4:0;
    if(!shared) {
        auto j=unsigned(rng()%rep.size());auto dirs=private_directions(c,rng);
        Orbit o(c,rep[j],{},std::move(dirs[0]),std::move(dirs[1]),shift,alpha);
        o.sum(false,dl);auto next=o.draw(0,rng);bool changed=next!=rep[j];
        rep[j]=std::move(next);return changed;
    }
    auto dc=direction(c,rng,(rng()&1)?0:c.R);
    std::vector<std::unique_ptr<Orbit>> orbits;
    std::vector<double> weights(16,0);
    for(const auto &x:rep) {
        auto dirs=private_directions(c,rng);
        auto o=std::make_unique<Orbit>(c,x,dc,std::move(dirs[0]),std::move(dirs[1]),shift,alpha);
        o->sum(true,dl);
        for(unsigned u=0;u<16;++u)weights[u]+=o->marginal[u];
        orbits.push_back(std::move(o));
    }
    unsigned u=draw_log(weights,rng);Replicas next;
    for(auto &o:orbits)next.push_back(o->draw(u,rng));
    bool changed=next!=rep;rep=std::move(next);return changed;
}
inline J run(const Cipher &c,const J &job,const Deadline &dl) {
    const double started=now();
    auto initial=job.at("states").get<std::vector<std::string>>();
    Trail x;for(const auto &s:initial)x.push_back(c.parse(s));
    unsigned copies=job.at("copies"),levels=job.value("activity_levels",12u);
    require((copies==2 || copies==4) && levels>=3 && levels<=64,"Invalid activity replica dimensions");
    std::mt19937_64 rng(job.at("seed").get<std::uint64_t>());
    Replicas start(copies,x);
    if(job.contains("activity_resume")) {
        auto r=job.at("activity_resume").get<std::vector<std::vector<std::string>>>();
        require(r.size()==copies,"Activity continuation copy mismatch");start.clear();
        for(const auto &ss:r){Trail t;for(const auto &s:ss)t.push_back(c.parse(s));start.push_back(std::move(t));}
    }
    require(std::isfinite(log_density(c,start,0)),"Activity search requires a legal starting ensemble");
    std::vector<Replicas> chains(levels,start);
    std::vector<double> alpha(levels);
    for(unsigned l=0;l+1<levels;++l)alpha[l]=std::exp2(-16.0*l/(levels-2));
    alpha.back()=0;
    std::uint64_t attempts=0,changed=0,shared_attempts=0,shared_changed=0,swap_attempts=0,swaps=0;
    std::uint64_t seen=0,pattern_changes=0,endpoint_changes=0;
    const unsigned keep=job.at("keep");

    std::map<std::string,J> archive;
    std::map<std::string,double> visited;
    std::vector<std::string> previous_pattern(copies);
    std::string previous_endpoint;
    auto collect=[&]() {
        for(unsigned replica=0;replica<copies;++replica) {
            const auto &t=chains.back()[replica];
            std::vector<std::string> states;Pattern p;
            for(int r=0;r<=c.R;++r){states.push_back(c.encode(t[r]));if(r<c.R){std::string a;for(auto v:t[r])a+=v?'1':'0';p.push_back(a);}}
            auto ep=states.front()+"_"+states.back();auto ps=digest(p);auto id=digest(J::array({ep,p}));
            if(!previous_pattern[replica].empty() && previous_pattern[replica]!=ps)++pattern_changes;
            if(!previous_endpoint.empty() && previous_endpoint!=ep)++endpoint_changes;
            previous_pattern[replica]=ps;previous_endpoint=ep;
            double weight=-log_density(c,Replicas{t},0)/std::log(2.0);
            J h{{"states",states},{"weight",weight},{"first_seconds",now()-started}};
            auto old=archive.find(id);
            if(old!=archive.end()){if(weight<old->second.at("weight").get<double>()){
                h["first_seconds"]=old->second.at("first_seconds");old->second=std::move(h);}continue;}
            if(!visited.emplace(id,h.at("first_seconds").get<double>()).second)continue;
            ++seen;
            if(archive.size()<keep)archive[id]=std::move(h);
            else {
                std::uniform_int_distribution<std::uint64_t> choose(0,seen-1);
                auto v=choose(rng);
                if(v<keep){auto i=archive.begin();std::advance(i,v);archive.erase(i);archive[id]=std::move(h);}
            }
        }
    };
    collect();std::string why;
    try {
        for(std::uint64_t sweep=0;;++sweep) {
            for(unsigned l=0;l<levels;++l) {
                dl.check();bool shared=(sweep%3==0);
                bool change=step(c,chains[l],alpha[l],rng,shared,dl);
                ++attempts;changed+=change;if(shared){++shared_attempts;shared_changed+=change;}
            }
            for(unsigned l=sweep%2;l+1<levels;l+=2) {
                dl.check();++swap_attempts;
                double ratio=log_density(c,chains[l],alpha[l+1])+log_density(c,chains[l+1],alpha[l])
                            -log_density(c,chains[l],alpha[l])-log_density(c,chains[l+1],alpha[l+1]);
                double u=std::generate_canonical<double,53>(rng);
                if(std::log(u)<ratio){std::swap(chains[l],chains[l+1]);++swaps;}
            }
            collect();
        }
    } catch(const Cancelled &e) {why=e.what();}

    collect();
    J states=J::array(),continuation=J::array();
    std::set<std::string> emitted;
    for(auto &kv:archive){states.push_back(kv.second);emitted.insert(kv.first);}
    for(const auto &t:chains.back()) {
        std::vector<std::string> ss;for(const auto &cut:t)ss.push_back(c.encode(cut));continuation.push_back(ss);
        auto id=digest(J::array({ss.front()+"_"+ss.back(),c.activity(ss)}));
        if(emitted.insert(id).second)states.push_back({{"states",ss},{"weight",-log_density(c,{t},0)/std::log(2.0)},
                                                       {"first_seconds",visited.at(id)}});
    }
    require(std::isfinite(log_density(c,chains.back(),0)),"Illegal cold activity continuation");
    return J{{"status","COMPLETE"},{"reason",why},{"activity_raw_hits",states},
        {"activity_resume",continuation},{"activity_diagnostics",{{"updates",attempts},{"changed",changed},
        {"shared_updates",shared_attempts},{"shared_changed",shared_changed},{"swap_attempts",swap_attempts},
        {"swaps",swaps},{"observed_pattern_changes",pattern_changes},{"observed_endpoint_changes",endpoint_changes},
        {"distinct_endpoint_patterns",seen},{"retained",states.size()},
        {"target","product of raw DDT/Q over replicas; endpoint marginal Z(e)^m"},
        {"stationarity_claimed",false},{"probability_certificate",false}}}};
}
}
}
