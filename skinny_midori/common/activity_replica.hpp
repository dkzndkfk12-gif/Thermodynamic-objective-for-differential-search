#pragma once
#include "model.hpp"
#include <random>

namespace thermo {
namespace activity_walk {
using Cut=std::vector<int>;
using Trail=std::vector<Cut>;
using Replicas=std::vector<Trail>;
using Direction=Trail;
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
            for(int i=0;i<16;++i) {
                auto l=log_factor(c.count(c.kind(i),x[r][i],b[i]),c.Q,alpha);
                if(!std::isfinite(l))return -INFINITY;
                logp+=l;
            }
        }
    }
    return logp;
}
inline Direction direction(const Cipher &c,std::mt19937_64 &rng,int boundary=-1) {
    Direction d(c.R+1,Cut(16));

    int pivot=boundary>=0?boundary:1+int(rng()%(c.R-1));
    int cell=int(rng()%16);d[pivot][cell]=1;
    bool collective=(rng()&1)!=0;
    if(collective) {
        int lo=boundary==0?0:1,hi=boundary==c.R?c.R:c.R-1;
        int start=lo+int(rng()%(pivot-lo+1));
        int end=pivot+int(rng()%(hi-pivot+1));
        for(int r=pivot;r<end;++r)d[r+1]=c.forward(d[r]);
        for(int r=pivot;r>start;--r)d[r-1]=c.inverse(d[r]);
    }
    return d;
}
inline std::array<Direction,2> private_directions(const Cipher &c,std::mt19937_64 &rng) {
    auto a=direction(c,rng),b=direction(c,rng);
    while(b==a)b=direction(c,rng);
    return {std::move(a),std::move(b)};
}
struct LocalFactor {
    int kind,a,b;unsigned ac,bc;
};
class Orbit {
    const Cipher &c;
    const Trail &x;
    Direction common,d1,d2;
    unsigned shift;
    double alpha;
    std::vector<LocalFactor> factors;
    std::vector<double> factor_log;
    static int change(unsigned mask,unsigned u,unsigned v,unsigned w,unsigned shift) {
        return int(((mask&1?u:0)^(mask&2?v:0)^(mask&4?w:0))<<shift);
    }
  public:
    std::vector<double> logweights,marginal;
    Orbit(const Cipher &cc,const Trail &xx,Direction dc,Direction a,Direction b,unsigned sh,double al)
        :c(cc),x(xx),common(std::move(dc)),d1(std::move(a)),d2(std::move(b)),shift(sh),alpha(al) {
        factor_log.resize(c.Q+1);
        for(int n=0;n<=c.Q;++n)factor_log[n]=log_factor(n,c.Q,alpha);
        if(common.empty())common=Direction(c.R+1,Cut(16));
        for(int r=0;r<c.R;++r) {
            auto y=c.inverse(x[r+1]),yc=c.inverse(common[r+1]),ya=c.inverse(d1[r+1]),yb=c.inverse(d2[r+1]);
            for(int i=0;i<16;++i) {
                unsigned ac=unsigned(common[r][i])|(unsigned(d1[r][i])<<1)|(unsigned(d2[r][i])<<2);
                unsigned bc=unsigned(yc[i])|(unsigned(ya[i])<<1)|(unsigned(yb[i])<<2);
                require(ac<8 && bc<8,"Non-binary XOR-orbit coefficient");
                if(ac || bc)factors.push_back({c.kind(i),x[r][i],y[i],ac,bc});
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
                for(int i=0;i<16;++i)nz|=(x[r][i]^(common[r][i]?int(u<<shift):0))!=0;
                valid&=nz;
            }
            if(!valid)continue;
            for(unsigned v=0;v<16;++v)for(unsigned w=0;w<16;++w) {
                unsigned index=u*256+v*16+w;
                if(!(index%64))dl.check();
                double l=0;
                for(const auto &f:factors) {
                    auto a=f.a^change(f.ac,u,v,w,shift),b=f.b^change(f.bc,u,v,w,shift);
                    auto lf=factor_log[c.count(f.kind,a,b)];
                    if(!std::isfinite(lf)){l=-INFINITY;break;}
                    l+=lf;
                }
                logweights[index]=l;
            }
            auto first=logweights.begin()+u*256;
            double high=*std::max_element(first,first+256);
            if(std::isfinite(high)) {
                double sum=0;for(auto i=first;i!=first+256;++i)sum+=std::exp(*i-high);
                marginal[u]=high+std::log(sum);
            }
        }
    }
    Trail draw(unsigned u,std::mt19937_64 &rng) const {
        auto begin=logweights.begin()+u*256;
        std::vector<double> weights(begin,begin+256);
        unsigned vw=draw_log(weights,rng),v=vw/16,w=vw%16;
        auto y=x;
        for(int r=0;r<=c.R;++r)for(int i=0;i<16;++i) {
            unsigned mask=unsigned(common[r][i])|(unsigned(d1[r][i])<<1)|(unsigned(d2[r][i])<<2);
            y[r][i]^=change(mask,u,v,w,shift);
        }
        return y;
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
