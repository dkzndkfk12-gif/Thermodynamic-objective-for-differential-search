

struct CollapsedCounters {
    std::uint64_t attempted=0,completed=0,skipped=0,endpoint_changes=0,internal_changes=0;
    double cpu_seconds=0;
};
inline CollapsedCounters &collapsed_counts(){static CollapsedCounters d;return d;}
inline std::string collapsed_diagnostics(){const auto &d=collapsed_counts();return
    "\"collapsed_attempted\":"+std::to_string(d.attempted)+",\"collapsed_completed\":"+std::to_string(d.completed)+
    ",\"collapsed_work_skips\":"+std::to_string(d.skipped)+",\"collapsed_endpoint_changes\":"+std::to_string(d.endpoint_changes)+
    ",\"collapsed_internal_changes\":"+std::to_string(d.internal_changes)+",\"collapsed_cpu_seconds\":"+std::to_string(d.cpu_seconds)+
    ",\"search_message_hits\":"+std::to_string(mass_messages::cache().hits)+",";}
inline void collapsed_update(const Catalog &cat,State &s,const Level &level,RNG &rng,Timer &timer) {
    using Ring=factor_sum::Log;using Fac=factor_sum::Factor<Ring>;
    auto &stats=collapsed_counts();++stats.attempted;
    struct Account {CollapsedCounters &d;std::clock_t start=std::clock();
        ~Account(){if(start>=0)d.cpu_seconds+=double(std::clock()-start)/CLOCKS_PER_SEC;}} account{stats};
    const int endpoint=draw(rng,cat.ne);
    struct Block {
        std::vector<int> vars;factor_sum::Domains domains;std::vector<Fac> factors;
        factor_sum::Plan plan;factor_sum::Result<Ring> result;
    };
    std::vector<Block> blocks(s.c.size());
    const auto cap=mass_messages::setting("THERMO_COLLAPSED_WORK",262144);
    std::uint64_t work=0;
    for(std::size_t j=0;j<blocks.size();++j) {
        auto &block=blocks[j];const auto &copy=s.c[j];const auto &m=cat.m[copy.p];
        block.vars={endpoint};std::set<int> neighbours;
        for(auto incidence:m.inc[endpoint])for(const auto *side:{&m.f[incidence.f].a,&m.f[incidence.f].b})
            for(auto term:*side)if(term.v>=cat.ne)neighbours.insert(term.v);
        std::vector<int> choices(neighbours.begin(),neighbours.end());std::shuffle(choices.begin(),choices.end(),rng);
        for(std::size_t k=0;k<std::min<std::size_t>(2,choices.size());++k)block.vars.push_back(choices[k]);
        for(int v:block.vars)block.domains.emplace_back(cat.allowed[v].begin(),cat.allowed[v].end());
        std::set<int> touched;for(int v:block.vars)for(auto incidence:m.inc[v])touched.insert(incidence.f);
        for(int id:touched) {
            const auto &f=m.f[id];unsigned a=copy.a[id],b=copy.b[id];std::vector<Term> aa,bb;
            for(unsigned side=0;side<2;++side)for(auto term:(side?f.b:f.a)) {
                const auto at=std::find(block.vars.begin(),block.vars.end(),term.v);
                if(at==block.vars.end())continue;
                (side?b:a)^=((copy.x[term.v]>>term.src)&1u)<<term.dst;
                term.v=int(at-block.vars.begin());(side?bb:aa).push_back(term);
            }
            std::set<int> scope;for(auto term:aa)scope.insert(term.v);for(auto term:bb)scope.insert(term.v);
            Fac factor;factor.scope.assign(scope.begin(),scope.end());
            std::ostringstream key;key<<"GIFT_BIT_COLLAPSED_V1/"<<a<<'/'<<b<<'/'
                <<std::setprecision(std::numeric_limits<Real>::max_digits10)<<level.t<<'/'<<level.hard<<'/'<<level.beta;
            for(const auto *side:{&aa,&bb}){key<<'/';for(auto t:*side)key<<t.v<<','<<t.src<<','<<t.dst<<';';}
            factor.semantic=key.str();
            factor.function=[&,aa,bb,a,b](const std::vector<int> &x) {
                unsigned av=a,bv=b;
                for(auto t:aa)av^=((x[t.v]>>t.src)&1u)<<t.dst;
                for(auto t:bb)bv^=((x[t.v]>>t.src)&1u)<<t.dst;
                return factor_log(cat,av,bv,level);
            };
            block.factors.push_back(std::move(factor));
        }
        Fac unit;unit.scope={0};unit.semantic="GIFT-endpoint-domain-unity";
        unit.function=[](const std::vector<int>&){return Real(0);};block.factors.push_back(std::move(unit));
        block.plan=factor_sum::plan(block.domains,block.factors,{0},[&](){timer.check();});
        work=factor_sum::add(work,factor_sum::add(block.plan.operations,
            factor_sum::mul(block.domains[0].size(),block.factors.size())));
    }
    if(!cap || work>cap){++stats.skipped;return;}
    for(auto &b:blocks)b.result=factor_sum::eliminate(b.domains,std::move(b.factors),b.plan,4*cap,[&](){timer.check();},true);
    std::vector<Real> scores(cat.allowed[endpoint].size(),0);
    for(const auto &b:blocks) {
        if(b.result.marginal.scope!=std::vector<int>{0} || b.result.marginal.data.size()!=scores.size())
            throw std::runtime_error("GIFT collapsed boundary mismatch");
        for(std::size_t k=0;k<scores.size();++k)scores[k]+=b.result.marginal.data[k];
    }
    const int selected=choose(scores,rng);std::vector<std::vector<int>> assignments;
    for(const auto &b:blocks) {
        std::vector<int> positions(b.vars.size());positions[0]=selected;
        for(auto i=b.result.trace.rbegin();i!=b.result.trace.rend();++i) {
            const auto n=b.domains[i->variable].size(),offset=factor_sum::index(i->scope,positions,b.domains)*n;
            positions[i->variable]=choose(std::vector<Real>(i->joint.begin()+offset,i->joint.begin()+offset+n),rng);
        }
        std::vector<int> values;for(std::size_t k=0;k<positions.size();++k)values.push_back(b.domains[k][positions[k]]);
        assignments.push_back(std::move(values));
    }

    for(std::size_t j=0;j<blocks.size();++j) {
        auto &copy=s.c[j];const auto &b=blocks[j];
        for(std::size_t k=0;k<b.vars.size();++k) {
            if(copy.x[b.vars[k]]!=assignments[j][k]){if(k==0 && j==0)++stats.endpoint_changes;else if(k)++stats.internal_changes;}
            copy.x[b.vars[k]]=static_cast<unsigned char>(assignments[j][k]);
        }
        refresh(cat,copy);
    }
    ++stats.completed;
}
