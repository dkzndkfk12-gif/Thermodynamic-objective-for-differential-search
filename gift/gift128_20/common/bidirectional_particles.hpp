#pragma once

namespace gift_particles {
using Trail=std::array<Nibbles,R+1>;
inline unsigned active(const Nibbles& x){return unsigned(std::count_if(x.begin(),x.end(),[](auto v){return v!=0;}));}
struct Step{Nibbles next{};Real weight=0,log_increment=-INF;bool valid=false;Real log_h=0;};
struct Kernel {
    std::array<std::array<int,16>,16> ddt{};
    struct Choice{Nibbles next;Real weight,cumulative,log_h;};
    Nibbles cached_input{};
    unsigned cached_cap=0,cached_future_cap=0;
    Real cached_beta=-1,cached_total=0;
    bool cached_reverse=false,cached=false;
    std::vector<Choice> choices;
    using PotentialKey=std::tuple<Nibbles,bool,unsigned,Real>;
    std::map<PotentialKey,Real> potentials;
    uint64_t lookahead_rows=0,lookahead_fallbacks=0,lookahead_dead_ends=0,lookahead_cache_hits=0;
    Kernel(){
        constexpr int s[16]={1,10,4,12,6,15,3,9,2,13,11,7,5,0,8,14};
        for(int a=0;a<16;++a)for(int x=0;x<16;++x)++ddt[a][s[x]^s[x^a]];
    }
    int count(int fixed,int variable,bool reverse)const {
        return reverse?ddt[variable][fixed]:ddt[fixed][variable];
    }
    uint64_t combinations(const Nibbles& fixed,bool reverse,uint64_t work)const {
        uint64_t n=1;
        for(auto v:fixed)if(v){
            unsigned size=0;for(int z=1;z<16;++z)size+=count(v,z,reverse)!=0;
            if(n>work/std::max(1u,size))return work+1;
            n*=size;
        }
        return n;
    }
    template<class Fn>
    void enumerate(const Nibbles& x,bool reverse,unsigned cap,Timer& timer,Fn&& fn)const {
        const auto fixed=reverse?inverse_linear(x):x;
        std::vector<int> cells;for(int c=0;c<NC;++c)if(fixed[c])cells.push_back(c);
        Nibbles out{};uint64_t nodes=0;
        auto visit=[&](auto&& self,size_t at,Real weight)->void{
            if((++nodes&255)==0)timer.check();
            if(at==cells.size()){
                const auto next=reverse?out:linear(out);
                if(active(next)<=cap)fn(next,weight);
                return;
            }
            int c=cells[at];
            for(int z=1;z<16;++z)if(int n=count(fixed[c],z,reverse)){
                out[c]=static_cast<unsigned char>(z);
                self(self,at+1,weight-std::log2(Real(n)/16));
            }
        };
        visit(visit,0,0);
    }
    Real continuation(const Nibbles& x,bool reverse,unsigned cap,Real beta,
                      uint64_t& remaining,uint64_t cache_limit,Timer& timer) {
        if(!cap)return 1;
        const PotentialKey key{x,reverse,cap,beta};
        auto it=potentials.find(key);
        if(it!=potentials.end()){++lookahead_cache_hits;return it->second;}
        const auto fixed=reverse?inverse_linear(x):x;
        const auto work=combinations(fixed,reverse,remaining);
        if(work>remaining){++lookahead_fallbacks;return 1;}
        remaining-=work;Real h=0;
        enumerate(x,reverse,cap,timer,[&](const Nibbles&,Real weight){h+=std::exp2(-beta*weight);});
        ++lookahead_rows;lookahead_dead_ends+=h==0;

        if(potentials.size()>=cache_limit)potentials.clear();
        if(cache_limit)potentials.emplace(key,h);
        return h;
    }

    Step draw(const Nibbles& x,unsigned cap,Real beta,uint64_t work,RNG& rng,Timer& timer,
              bool reverse=false,unsigned future_cap=0,uint64_t lookahead_work=0) {
        const auto fixed=reverse?inverse_linear(x):x;
        if(combinations(fixed,reverse,work)>work){
            Nibbles out{};Real weight=0;
            for(int c=0;c<NC;++c)if(fixed[c]){
                int k=::draw(rng,16),z=1;
                for(;z<16;++z){k-=count(fixed[c],z,reverse);if(k<0)break;}
                if(z==16)throw std::runtime_error("Particle DDT row/column is not normalized");
                out[c]=static_cast<unsigned char>(z);weight-=std::log2(Real(count(fixed[c],z,reverse))/16);
            }
            auto next=reverse?out:linear(out);bool valid=active(next)<=cap;
            return {next,weight,valid?-(beta-1)*weight*std::log(Real(2)):-INF,valid};
        }
        auto sample=[&](){
            if(!(cached_total>0))return Step{};
            Real target=unit(rng)*cached_total;
            auto it=std::upper_bound(choices.begin(),choices.end(),target,[](Real v,const Choice& q){return v<q.cumulative;});
            if(it==choices.end())it=std::prev(choices.end());

            return Step{it->next,it->weight,std::log(cached_total)-it->log_h,true,it->log_h};
        };
        if(cached&&x==cached_input&&cap==cached_cap&&beta==cached_beta&&
           reverse==cached_reverse&&future_cap==cached_future_cap)return sample();
        cached=false;choices.clear();cached_total=0;
        std::vector<std::pair<Nibbles,Real>> row;
        enumerate(x,reverse,cap,timer,[&](const Nibbles& next,Real weight){row.emplace_back(next,weight);});
        uint64_t remaining=lookahead_work;
        for(const auto& [next,weight]:row){
            const Real h=lookahead_work?continuation(next,reverse,future_cap,beta,remaining,work,timer):1;
            if(h==0)continue;
            cached_total+=std::exp2(-beta*weight)*h;
            choices.push_back({next,weight,cached_total,std::log(h)});
        }
        cached=true;cached_input=x;cached_cap=cap;cached_beta=beta;
        cached_reverse=reverse;cached_future_cap=future_cap;
        return sample();
    }
    Real audit(const Trail& x)const {
        Real w=0;
        for(int r=0;r<R;++r){auto b=inverse_linear(x[r+1]);for(int c=0;c<NC;++c){
            int n=ddt[x[r][c]][b[c]];
            if(!n)throw std::runtime_error("Illegal particle witness");
            w-=std::log2(Real(n)/16);
        }}
        return w;
    }
};
struct Particle{Trail x{};Real weight=0;unsigned used=0;Real log_h=0;};
struct Hit{Particle p;double first=0;};
inline std::string activity_pattern(const Particle& p){
    std::string pattern;pattern.reserve(size_t(R)*NC);
    for(int r=0;r<R;++r)for(auto v:p.x[r])pattern+=v?'1':'0';
    return pattern;
}
inline std::string endpoint_pattern_key(const Particle& p){
    return activity_pattern(p)+"|"+nibble_text(p.x[0])+"|"+nibble_text(p.x[R]);
}
inline void retain(std::map<std::string,Hit>& hits,const Particle& p,double first){
    auto key=endpoint_pattern_key(p);auto it=hits.find(key);
    if(it==hits.end())hits.emplace(key,Hit{p,first});
    else if(p.weight<it->second.p.weight)it->second=Hit{p,first};
}
inline void run(const Options& o,Timer& timer){
    if(o.center<1||o.center>NC||o.width<1||o.width>NC||o.paths<2)
        throw std::runtime_error("Invalid legal-particle initializer settings");
    Kernel kernel;RNG rng(o.seed);std::map<std::string,Hit> hits;
    uint64_t populations=0,extinct=0,terminal=0,forward=0,interior=0;
    std::map<int,uint64_t> anchors,deaths;
    const uint64_t anchor_phase=mix(o.seed^UINT64_C(0x243f6a8885a308d3))%(R-1);
    const uint64_t beta_phase=mix(o.seed^UINT64_C(0x13198a2e03707344))%4;
    try{for(uint64_t attempt=0;attempt<o.sweeps;++attempt){
        timer.check();

        const int cut=o.seed_bidirectional&&(attempt%2)?1+int((anchor_phase+attempt/2)%(R-1)):0;

        const auto beta_index=o.seed_bidirectional?
            (beta_phase+attempt/2+attempt/(2*(R-1)))%4:attempt%4;
        const Real beta=std::array<Real,4>{1,1.5L,2,3}[beta_index];
        Nibbles initial{};std::vector<int> indices(NC);std::iota(indices.begin(),indices.end(),0);
        std::shuffle(indices.begin(),indices.end(),rng);
        const int k=1+::draw(rng,cut?o.center:o.width);
        for(int j=0;j<k;++j)initial[indices[j]]=static_cast<unsigned char>(1+::draw(rng,15));
        std::vector<Particle> particles(size_t(o.paths)),next(size_t(o.paths));
        for(auto& p:particles)p.x[cut]=initial;
        bool dead=false;int left=cut,right=cut,depth=0;
        while(left>0||right<R){
            const bool reverse=left>0&&(right==R||((depth+attempt)%2==0));
            const int from=reverse?left:right,to=reverse?left-1:right+1;
            const unsigned cap=to==0?unsigned(o.width):to==R?NC:unsigned(o.center);
            const int subsequent=reverse?to-1:to+1;
            const unsigned future_cap=subsequent<0||subsequent>R?0:
                subsequent==0?unsigned(o.width):subsequent==R?NC:unsigned(o.center);
            std::vector<Real> logs(size_t(o.paths),-INF);Real top=-INF;
            for(size_t i=0;i<particles.size();++i){
                timer.check();next[i]=particles[i];auto& p=next[i];
                Step step=kernel.draw(p.x[from],cap,beta,o.seed_work,rng,timer,
                                      reverse,future_cap,o.seed_lookahead_work);
                if(!step.valid)continue;
                p.x[to]=step.next;p.weight+=step.weight;
                p.used+=active(p.x[reverse?to:from]);
                if(p.used+unsigned(R-depth-1)>unsigned(o.active))continue;

                logs[i]=step.log_increment+step.log_h-p.log_h;p.log_h=step.log_h;
                top=std::max(top,logs[i]);
            }
            if(!std::isfinite(top)){dead=true;++deaths[depth];break;}
            std::vector<Real> prob(logs.size());Real sum=0;
            for(size_t i=0;i<logs.size();++i){prob[i]=std::exp(logs[i]-top);sum+=prob[i];}
            const Real offset=unit(rng)*sum/particles.size();Real cdf=prob[0];size_t at=0;
            for(size_t i=0;i<particles.size();++i){
                Real target=offset+i*sum/particles.size();
                while(at+1<prob.size()&&target>=cdf)cdf+=prob[++at];
                particles[i]=next[at];
            }
            if(reverse)--left;else ++right;
            ++depth;
        }
        ++populations;++anchors[cut];forward+=cut==0;interior+=cut!=0;
        if(dead){++extinct;continue;}
        for(auto& p:particles){
            if(!active(p.x[0])||!active(p.x[R]))throw std::runtime_error("Zero particle endpoint");
            const Real audited=kernel.audit(p.x);
            if(std::abs(audited-p.weight)>1e-9L)throw std::runtime_error("Particle witness weight mismatch");
            unsigned used=0;for(int r=0;r<R;++r)used+=active(p.x[r]);
            if(used!=p.used||used>unsigned(o.active))throw std::runtime_error("Particle activity accounting mismatch");
            retain(hits,p,timer.seconds());++terminal;
        }
        while(hits.size()>size_t(o.limit)){

            auto worst=hits.begin();
            for(auto it=hits.begin();it!=hits.end();++it)
                if(attempt%2?it->second.p.weight>worst->second.p.weight:
                   text_hash(it->first,o.seed)>text_hash(worst->first,o.seed))worst=it;
            hits.erase(worst);
        }
    }}catch(const Budget&){}
    std::ofstream out(o.out+".part");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"states\tfirst_seconds\n"<<std::setprecision(17);
    Real best=INF;
    for(const auto& [key,h]:hits){
        best=std::min(best,h.p.weight);
        for(int r=0;r<=R;++r){if(r)out<<',';out<<nibble_text(h.p.x[r]);}
        out<<'\t'<<h.first<<'\n';
    }
    out.close();publish(o.out);
    std::ofstream diag(o.out+".seed.json");diag.exceptions(std::ios::badbit|std::ios::failbit);
    diag<<std::setprecision(17)<<"{\"kind\":\"bidirectional_lookahead_initializer\",\"populations\":"<<populations
        <<",\"extinct\":"<<extinct<<",\"terminal_particles\":"<<terminal<<",\"cap\":"<<o.center
        <<",\"patterns\":"<<hits.size()<<",\"forward_populations\":"<<forward<<",\"interior_populations\":"<<interior
        <<",\"lookahead_rows\":"<<kernel.lookahead_rows<<",\"lookahead_fallbacks\":"<<kernel.lookahead_fallbacks
        <<",\"lookahead_dead_ends\":"<<kernel.lookahead_dead_ends<<",\"lookahead_cache_hits\":"<<kernel.lookahead_cache_hits
        <<",\"best_witness_weight\":";
    if(std::isfinite(best))diag<<best;else diag<<"null";
    auto counts=[&](const char* name,const auto& values){
        diag<<",\""<<name<<"\":{";bool first=true;
        for(const auto& [key,n]:values){if(!first)diag<<',';first=false;diag<<'"'<<key<<"\":"<<n;}diag<<'}';
    };
    counts("anchor_population_counts",anchors);counts("extinction_depth_counts",deaths);
    diag<<",\"probability_estimate\":false}";
}
}
