

struct CollapsedCounters {std::uint64_t attempted=0,completed=0,skipped=0,endpoint_changes=0,internal_changes=0;double cpu_seconds=0;};
inline CollapsedCounters& collapsed_counts(){static CollapsedCounters c;return c;}
inline std::string collapsed_diagnostics(){auto&d=collapsed_counts();return
    "\"collapsed_attempted\":"+std::to_string(d.attempted)+",\"collapsed_completed\":"+std::to_string(d.completed)+
    ",\"collapsed_work_skips\":"+std::to_string(d.skipped)+",\"collapsed_endpoint_changes\":"+std::to_string(d.endpoint_changes)+
    ",\"collapsed_internal_changes\":"+std::to_string(d.internal_changes)+",\"collapsed_cpu_seconds\":"+std::to_string(d.cpu_seconds)+",\"search_message_hits\":"+std::to_string(mass_messages::cache().hits)+",";}
#ifdef THERMO_COLLAPSED_AUDIT
template<class Blocks> void audit_collapsed(const Catalog&,const State&,const Level&,int,const Blocks&);
#endif
inline void collapsed_update(const Catalog&cat,State&s,const Level&l,RNG&g,Timer&timer) {
    using Ring=factor_sum::Log;using Fac=factor_sum::Factor<Ring>;
    auto&stats=collapsed_counts();++stats.attempted;
    struct CpuReceipt {CollapsedCounters&c;std::clock_t start=std::clock();~CpuReceipt(){if(start>=0)c.cpu_seconds+=double(std::clock()-start)/CLOCKS_PER_SEC;}} receipt{stats};
    const int u=draw(g,cat.ne);
    struct Block {std::vector<int> vars;factor_sum::Domains domains;std::vector<Fac> factors;factor_sum::Plan plan;factor_sum::Result<Ring> result;};
    std::vector<Block> blocks(s.c.size());

    for(std::size_t j=0;j<blocks.size();++j){auto&b=blocks[j];const auto&copy=s.c[j];const auto&m=cat.m[copy.p];b.vars={u};
        std::set<int> neighbours;for(auto inc:m.inc[u]){const auto&f=m.f[inc.f];for(int v:f.a)if(v>=cat.ne)neighbours.insert(v);for(int v:f.b)if(v>=cat.ne)neighbours.insert(v);}
        std::vector<int> available(neighbours.begin(),neighbours.end());std::shuffle(available.begin(),available.end(),g);
        int count=std::min<int>(MS_Q==16?2:1,available.size());for(int i=0;i<count;++i)b.vars.push_back(available[i]);
        for(int v:b.vars)b.domains.emplace_back(cat.allowed[v].begin(),cat.allowed[v].end());
    }
    const auto cap=mass_messages::setting("THERMO_COLLAPSED_WORK",262144);

    auto prepare=[&](){std::uint64_t total=0;
        for(std::size_t j=0;j<blocks.size();++j){auto&b=blocks[j];const auto&copy=s.c[j];const auto&m=cat.m[copy.p];b.factors.clear();
            std::set<int> touched;for(int v:b.vars)for(auto inc:m.inc[v])touched.insert(inc.f);
            for(int id:touched){const auto&f=m.f[id];std::vector<int> aa,bb;unsigned a=copy.a[id],z=copy.b[id];
                for(std::size_t k=0;k<b.vars.size();++k){int v=b.vars[k];
                    if(std::find(f.a.begin(),f.a.end(),v)!=f.a.end()){aa.push_back(int(k));a^=copy.x[v];}
                    if(std::find(f.b.begin(),f.b.end(),v)!=f.b.end()){bb.push_back(int(k));z^=copy.x[v];}}
                std::set<int> sc(aa.begin(),aa.end());sc.insert(bb.begin(),bb.end());Fac t;t.scope.assign(sc.begin(),sc.end());
                std::ostringstream key;key<<"COLLAPSED_FACTOR_V1/"<<MS_CIPHER<<'/'<<f.cell<<'/'<<a<<'/'<<z<<'/'<<factor_sum::ints(aa)<<'/'<<factor_sum::ints(bb)<<'/'
                    <<std::setprecision(std::numeric_limits<Real>::max_digits10)<<l.t<<'/'<<l.hard;
                t.semantic=key.str();const int cell=f.cell;
                t.function=[&,aa,bb,a,z,cell](const std::vector<int>&x){unsigned av=a,bv=z;for(int v:aa)av^=x[v];for(int v:bb)bv^=x[v];return ms_log(cat,cell,av,bv,l);};b.factors.push_back(std::move(t));}
            Fac unit;unit.scope={0};unit.semantic="endpoint-domain-unity";unit.function=[](const std::vector<int>&){return Real(0);};b.factors.push_back(std::move(unit));
            b.plan=factor_sum::plan(b.domains,b.factors,{0},[&](){timer.check();});total=factor_sum::add(total,factor_sum::add(b.plan.operations,factor_sum::mul(b.domains[0].size(),b.factors.size())));
        }return total;};
    auto work=prepare();
    if(work>cap && MS_Q==256){for(std::size_t j=0;j<blocks.size();++j){auto&b=blocks[j];const auto&copy=s.c[j];
            for(std::size_t k=1;k<b.vars.size();++k){int mask=15<<(4*draw(g,2)),fixed=copy.x[b.vars[k]]&~mask;
                auto&d=b.domains[k];d.erase(std::remove_if(d.begin(),d.end(),[&](int v){return (v&~mask)!=fixed;}),d.end());}}
        work=prepare();}
    if(!cap||work>cap){++stats.skipped;return;}

    for(auto&b:blocks)b.result=factor_sum::eliminate(b.domains,std::move(b.factors),b.plan,4*cap,[&](){timer.check();},true);
#ifdef THERMO_COLLAPSED_AUDIT
    audit_collapsed(cat,s,l,u,blocks);
#endif
    std::vector<Real> scores(cat.allowed[u].size(),0);
    for(auto&b:blocks){if(b.result.marginal.scope!=std::vector<int>{0}||b.result.marginal.data.size()!=scores.size())throw std::runtime_error("collapsed boundary mismatch");
        for(std::size_t k=0;k<scores.size();++k)scores[k]+=b.result.marginal.data[k];}
    const int endpoint_position=choose(scores,g);
    std::vector<std::vector<int>> assignments;
    for(auto&b:blocks){std::vector<int> pos(b.vars.size());pos[0]=endpoint_position;
        for(auto it=b.result.trace.rbegin();it!=b.result.trace.rend();++it){const auto&tr=*it;auto n=b.domains[tr.variable].size();auto offset=factor_sum::index(tr.scope,pos,b.domains)*n;
            std::vector<Real> conditional(tr.joint.begin()+offset,tr.joint.begin()+offset+n);pos[tr.variable]=choose(conditional,g);}
        std::vector<int> values;for(std::size_t k=0;k<pos.size();++k)values.push_back(b.domains[k][pos[k]]);assignments.push_back(std::move(values));}

    for(std::size_t j=0;j<blocks.size();++j){auto&copy=s.c[j];auto&b=blocks[j];for(std::size_t k=0;k<b.vars.size();++k){if(copy.x[b.vars[k]]!=assignments[j][k]){if(k==0&&j==0)++stats.endpoint_changes;else if(k)++stats.internal_changes;}copy.x[b.vars[k]]=static_cast<unsigned char>(assignments[j][k]);}refresh(cat,copy);}
    ++stats.completed;
}
