#pragma once
#include "model.hpp"

namespace craft {
struct ThermalLevel {
    double alpha;
    std::array<double,17> log_factor{};
    explicit ThermalLevel(double a):alpha(a) { for(int n=0;n<=16;++n){double p=(a+(1-a)*n)/16;log_factor[n]=p>0?std::log(p):-INFINITY;} }
};
using Copies = std::vector<Values>;
inline double factor_log(const Factor& f,const Values& x,const ThermalLevel& level){int n=f.count(x);return f.weighted?level.log_factor[n]:(n?0:-INFINITY);}
inline double log_density(const Model& m,const Copies& copies,const ThermalLevel& level) {
    double q=0;
    for(const auto& x:copies) { if(!m.endpoints_nonzero(x))return -INFINITY;for(const auto& f:m.factors){double a=factor_log(f,x,level);if(!std::isfinite(a))return -INFINITY;q+=a;} }return q;
}
inline std::vector<int> touched_factors(const Model& m,const std::vector<int>& vars) {
    std::vector<int> fs;for(int v:vars)fs.insert(fs.end(),m.incident[v].begin(),m.incident[v].end());std::sort(fs.begin(),fs.end());fs.erase(std::unique(fs.begin(),fs.end()),fs.end());return fs;
}
inline std::vector<int> domain_values(uint16_t mask) { std::vector<int> x;for(int v=0;v<16;++v)if(mask&(1u<<v))x.push_back(v);return x; }
struct Conditional {
    std::vector<int> variables,factors;
    std::vector<std::vector<int>> domains;
    std::vector<double> logweights;
    Conditional(const Model& m,const Values& state,std::vector<int> vars,const ThermalLevel& level,const Domains* pattern=nullptr):variables(std::move(vars)),factors(touched_factors(m,variables)) {
        size_t n=1;for(int v:variables){domains.push_back(domain_values(m.domains[v]));n*=domains.back().size();}
        logweights.resize(n);Values x=state;

        std::vector<size_t> shared_positions;size_t shared_size=1;
        for(size_t j=0;j<variables.size();++j)if(variables[j]<m.ne){shared_positions.push_back(j);shared_size*=domains[j].size();}
        std::vector<bool> endpoint_ok(shared_size);
        for(size_t code=0;code<shared_size;++code){size_t z=code;for(auto j:shared_positions){x[variables[j]]=uint8_t(domains[j][z%domains[j].size()]);z/=domains[j].size();}endpoint_ok[code]=m.endpoints_nonzero(x);}
        for(size_t code=0;code<n;++code) {
            size_t z=code,shared_code=0,multiplier=1;for(size_t j=0;j<variables.size();++j){size_t digit=z%domains[j].size();x[variables[j]]=uint8_t(domains[j][digit]);z/=domains[j].size();if(variables[j]<m.ne){shared_code+=digit*multiplier;multiplier*=domains[j].size();}}
            double q=endpoint_ok[shared_code]?0:-INFINITY;
            if(std::isfinite(q))for(int f:factors){q+=factor_log(m.factors[f],x,level);if(!std::isfinite(q))break;}
            if(pattern)for(int v:variables)if(!((*pattern)[v]&(1u<<x[v])))q+=level.alpha>0?std::log(level.alpha):-INFINITY;
            logweights[code]=q;
        }
    }
    void apply(Values& x,size_t code) const { for(size_t j=0;j<variables.size();++j){x[variables[j]]=uint8_t(domains[j][code%domains[j].size()]);code/=domains[j].size();} }
};

inline std::vector<double> endpoint_marginal(const std::vector<Conditional>& c,size_t nend) {
    std::vector<double> joint(nend,0);
    for(const auto& t:c) {
        std::vector<double> marginal(nend,-INFINITY);
        for(size_t code=0;code<t.logweights.size();++code)marginal[code%nend]=logadd(marginal[code%nend],t.logweights[code]);
        for(size_t e=0;e<nend;++e)joint[e]+=marginal[e];
    }return joint;
}
inline bool collapsed_endpoint(const Model& m,Copies& copies,const ThermalLevel& level,RNG& rng,const std::vector<Domains>* patterns=nullptr) {
    int e=draw(rng,m.ne);std::vector<int> neighbours;
    for(int f:m.incident[e])for(int v:m.factors[f].scope)if(v>=m.ne)neighbours.push_back(v);
    std::sort(neighbours.begin(),neighbours.end());neighbours.erase(std::unique(neighbours.begin(),neighbours.end()),neighbours.end());
    std::vector<Conditional> conditionals;
    for(const auto& x:copies) {
        auto available=neighbours;std::shuffle(available.begin(),available.end(),rng);std::vector<int> vars{e};

        int n=std::min<int>(int(available.size()),draw(rng,4)==0?2:1);
        for(int j=0;j<n;++j)vars.push_back(available[j]);
        conditionals.emplace_back(m,x,std::move(vars),level,patterns?&patterns->at(conditionals.size()):nullptr);
    }
    size_t ne=domain_values(m.domains[e]).size();auto weights=endpoint_marginal(conditionals,ne);int chosen=draw_log(weights,rng);bool changed=false;
    for(size_t j=0;j<copies.size();++j) {
        const auto& t=conditionals[j];std::vector<double> w;
        for(size_t code=chosen;code<t.logweights.size();code+=ne)w.push_back(t.logweights[code]);
        size_t index=size_t(chosen)+ne*size_t(draw_log(w,rng));auto old=copies[j][e];t.apply(copies[j],index);changed|=old!=copies[j][e];
    }return changed;
}
inline void internal_update(const Model& m,Copies& copies,const ThermalLevel& level,RNG& rng,const std::vector<Domains>* patterns=nullptr) {
    if(m.nv==m.ne)return;
    int j=draw(rng,int(copies.size()));std::vector<int> vars;
    if(draw(rng,4)==0 && !m.factors.empty()){
        std::vector<int> available;for(int v:m.factors[draw(rng,int(m.factors.size()))].scope)if(v>=m.ne)available.push_back(v);
        std::shuffle(available.begin(),available.end(),rng);int n=std::min<int>(2,int(available.size()));vars.assign(available.begin(),available.begin()+n);
    }
    if(vars.empty())vars.push_back(m.ne+draw(rng,m.nv-m.ne));
    Conditional c(m,copies[j],std::move(vars),level,patterns?&patterns->at(size_t(j)):nullptr);c.apply(copies[j],size_t(draw_log(c.logweights,rng)));
}

inline Values linear_direction(const Model& m,RNG& rng,int boundary=-1){
    require(m.physical,"collective direction needs full physical coordinates");
    int pivot=boundary>=0?boundary:1+draw(rng,m.rounds-1);Trail t(m.rounds+1);t[pivot][draw(rng,16)]=1;
    int lo=boundary==0?0:1,hi=boundary==m.rounds?m.rounds:m.rounds-1;
    int start=lo+draw(rng,pivot-lo+1),end=pivot+draw(rng,hi-pivot+1);
    for(int r=pivot;r<end;++r)t[r+1]=linear(t[r]);
    for(int r=pivot;r>start;--r)t[r-1]=inverse_linear(t[r]);
    Values d(m.nv);for(int v=0;v<m.nv;++v){auto [r,c]=m.variable_cells[v];d[v]=t[r][c];}return d;
}
struct OrbitConditional {
    Values common,private1,private2;
    std::vector<double> logweights;
    OrbitConditional(const Model& m,const Values& state,const ThermalLevel& level,Values u,Values v,Values w)
        :common(std::move(u)),private1(std::move(v)),private2(std::move(w)),logweights(4096){
        struct Local{int a,b;unsigned ac,bc;};std::vector<Local> factors;
        for(const auto& f:m.factors){
            unsigned ac=evaluate(f.a,common)|(unsigned(evaluate(f.a,private1))<<1)|(unsigned(evaluate(f.a,private2))<<2);
            unsigned bc=evaluate(f.b,common)|(unsigned(evaluate(f.b,private1))<<1)|(unsigned(evaluate(f.b,private2))<<2);
            if(ac||bc)factors.push_back({evaluate(f.a,state),evaluate(f.b,state),ac,bc});
        }
        Values endpoints=state;
        for(unsigned a=0;a<16;++a){
            for(int i=0;i<m.ne;++i)endpoints[i]=uint8_t(state[i]^(common[i]?a:0));
            bool ok=m.endpoints_nonzero(endpoints);
            for(unsigned b=0;b<16;++b)for(unsigned c=0;c<16;++c){
                std::array<unsigned,8> delta{0,a,b,a^b,c,a^c,b^c,a^b^c};double q=ok?0:-INFINITY;
                if(ok)for(const auto& f:factors){q+=level.log_factor[ddt().n[f.a^delta[f.ac]][f.b^delta[f.bc]]];if(!std::isfinite(q))break;}
                logweights[a+16*b+256*c]=q;
            }
        }
    }
    void apply(Values& x,size_t code) const {unsigned a=unsigned(code&15),b=unsigned((code>>4)&15),c=unsigned((code>>8)&15);for(size_t i=0;i<x.size();++i)x[i]^=uint8_t((common[i]?a:0)^(private1[i]?b:0)^(private2[i]?c:0));}
};
inline bool collective_endpoint(const Model& m,Copies& copies,const ThermalLevel& level,RNG& rng){
    int boundary=draw(rng,2)?0:m.rounds;auto common=linear_direction(m,rng,boundary);
    std::vector<OrbitConditional> blocks;std::vector<double> joint(16,0);
    for(const auto& x:copies){
        auto v=linear_direction(m,rng),w=linear_direction(m,rng);while(w==v)w=linear_direction(m,rng);
        blocks.emplace_back(m,x,level,common,std::move(v),std::move(w));std::vector<double> marginal(16,-INFINITY);
        for(size_t code=0;code<4096;++code)marginal[code&15]=logadd(marginal[code&15],blocks.back().logweights[code]);
        for(int a=0;a<16;++a)joint[a]+=marginal[a];
    }
    int a=draw_log(joint,rng);bool changed=false;
    for(size_t j=0;j<copies.size();++j){std::vector<double> weights;for(size_t code=size_t(a);code<4096;code+=16)weights.push_back(blocks[j].logweights[code]);
        auto old=copies[j];blocks[j].apply(copies[j],size_t(a)+16*size_t(draw_log(weights,rng)));changed|=!std::equal(old.begin(),old.begin()+m.ne,copies[j].begin());}
    return changed;
}
class ReplicaSearch {
public:
    std::shared_ptr<const Model> model;
    bool fixed_endpoints=false;
    std::vector<ThermalLevel> levels;
    std::vector<Copies> lanes;
    uint64_t steps=0,endpoint_updates=0,endpoint_changes=0,swap_attempts=0,swap_accepts=0;
    uint64_t cold_endpoint_changes=0,cold_swap_attempts=0,cold_swap_accepts=0;
    uint64_t collective_updates=0;
    ReplicaSearch(std::shared_ptr<const Model> m,const Values& witness,int copies,RNG& rng,bool fixed=false):model(std::move(m)),fixed_endpoints(fixed) {
        require(copies>=1 && model->legal(witness),"replica requires legal initialization");
        for(double a:{1.0,0.25,0.04,0.004,0.0001,0.0})levels.emplace_back(a);
        lanes.assign(levels.size(),Copies(copies,witness));

        if(model->physical)for(size_t l=0;l+1<lanes.size();++l)for(auto& x:lanes[l])for(int v=model->ne;v<model->nv;++v){auto d=domain_values(model->domains[v]);x[v]=uint8_t(d[draw(rng,int(d.size()))]);}
    }
    void step(RNG& rng) {
        Values previous_cold(lanes.back()[0].begin(),lanes.back()[0].begin()+model->ne);
        for(size_t l=0;l<lanes.size();++l) {
            internal_update(*model,lanes[l],levels[l],rng);
            if(fixed_endpoints){
                if(model->physical&&model->rounds>1&&draw(rng,8)==0){
                    auto v=linear_direction(*model,rng),w=linear_direction(*model,rng);
                    auto& x=lanes[l][size_t(draw(rng,int(lanes[l].size())))];
                    OrbitConditional orbit(*model,x,levels[l],Values(model->nv,0),std::move(v),std::move(w));
                    orbit.apply(x,size_t(draw_log(orbit.logweights,rng)));++collective_updates;
                }
                continue;
            }
            ++endpoint_updates;
            if(model->physical&&model->rounds>1&&draw(rng,8)==0){endpoint_changes+=collective_endpoint(*model,lanes[l],levels[l],rng);++collective_updates;}
            else endpoint_changes+=collapsed_endpoint(*model,lanes[l],levels[l],rng);
        }
        for(size_t l=size_t(steps%2);l+1<lanes.size();l+=2) {
            ++swap_attempts;
            if(l+2==lanes.size())++cold_swap_attempts;
            double before=log_density(*model,lanes[l],levels[l])+log_density(*model,lanes[l+1],levels[l+1]);
            double after=log_density(*model,lanes[l],levels[l+1])+log_density(*model,lanes[l+1],levels[l]);
            double ratio=after-before;if(ratio>=0||std::log(unit(rng))<ratio){std::swap(lanes[l],lanes[l+1]);++swap_accepts;if(l+2==lanes.size())++cold_swap_accepts;}
        }
        if(!std::equal(previous_cold.begin(),previous_cold.end(),lanes.back()[0].begin()))++cold_endpoint_changes;
        ++steps;
    }
    const Copies& cold() const { return lanes.back(); }
};
}
