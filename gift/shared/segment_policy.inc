

inline bool segment_turn_due(const J& meta,double share,const char* counter="search_counter") {
    const auto n=count(meta,counter);
    return share>0 && std::floor((n+1)*share)>std::floor(n*share);
}
inline J Campaign::segment_job() {

    if(cases.empty())return nullptr;
    struct Seed { J states; unsigned input,cap; };
    std::vector<Seed> available;
    auto cursor=cases.upper_bound(m.value("segment_seed_cursor",std::string()));
    if(cursor==cases.end())cursor=cases.begin();
    const auto scan_limit=std::min<std::size_t>(cases.size(),std::max<std::size_t>(1,std::size_t(slots)*a.keep));
    for(std::size_t visited=0;visited<scan_limit&&available.size()<a.keep;++visited){
        const auto& c=cursor->second;m["segment_seed_cursor"]=cursor->first;
        const auto ss=c.at("states").get<std::vector<std::string>>();
        if(ss.size()!=std::size_t(cipher.R+1))throw std::runtime_error("retained segment witness length mismatch");
        unsigned input=input_activity(ss.front()),cap=0;
        for(int r=1;r<cipher.R;++r)cap=std::max(cap,input_activity(ss[r]));
        if(input>0&&(a.mass_conditions||(input<=a.max_input&&cap<=a.max_cap)))available.push_back({ss,input,std::max(1u,cap)});
        if(++cursor==cases.end())cursor=cases.begin();
    }

    if(available.empty())return nullptr;
    if(a.mass_conditions){
        std::set<unsigned> taken;
        for(const auto* j:running("seed"))if(j->value("segment_graph",false))taken.insert(j->at("segment_slot").get<unsigned>());
        unsigned slot=0;while(taken.count(slot))++slot;if(slot>=slots)return nullptr;
        const unsigned cells=cipher.cfg.value("cells",16);
        const unsigned copies=std::array<unsigned,3>{1,2,4}[count(m,"segment_submitted")%3];
        J seeds=J::array();for(const auto& s:available)seeds.push_back(s.states);
        auto& domain=m["segment_domains"]["full_legal"];if(domain.is_null())domain=J::object();
        domain["input_active"]=cells;domain["cap"]=cells;
        const auto last=(m.contains("segment_slots")?m.at("segment_slots").value(std::to_string(slot),J::object()):J::object());
        const auto observed=count(last,"reservation_revision")==2?count(last,"peak_accounted_bytes",Resources::minimum):Resources::minimum;
        const auto per_node=std::uint64_t(832)+4*std::uint64_t(cipher.R+1)*(48+((4*cells*cipher.R+64)/64)*8);
        auto hint=std::max({Resources::minimum,sat_mul(seeds.size()*(cipher.R+1),per_node),observed});
        return {{"kind","seed"},{"segment_graph",true},{"mass_conditions",true},
            {"mass_block_bits",a.mass_block_bits},{"mass_bias_levels",a.mass_bias_levels},{"mass_bias_strength",a.mass_bias_strength},
            {"segment_domain","full_legal"},{"segment_slot",slot},{"cap",cells},{"input_active",cells},
            {"active",cells*cipher.R},{"copies",copies},{"row_work",a.segment_work},{"initial_trails",seeds},
            {"segment_cache",(scratch/"segments"/(std::to_string(slot)+"_m"+std::to_string(copies)+"_full.bin")).string()},
            {"segment_memory_hint",hint},{"seconds",a.pattern_seconds},{"keep",std::max(a.keep,2u*unsigned(cipher.R))},{"evaluation",false}};
    }
    auto& ds=m["segment_domains"];
    if(ds.is_null())ds=J::object();
    for(unsigned k:a.caps){auto key=domain_key(a.input_cap,k);if(!ds.contains(key))ds[key]={{"input_active",a.input_cap},{"cap",k}};}
    if(ds.empty())for(unsigned k=std::min(2u,a.max_cap);k<=std::min(4u,a.max_cap);++k)ds[domain_key(a.input_cap,k)]={{"input_active",a.input_cap},{"cap",k}};
    auto finished=count(m,"segment_finished");
    if(finished>=count(m,"segment_next_expansion",a.adapt_every)){
        std::map<std::tuple<unsigned,unsigned,unsigned>,J> next;
        for(const auto& d:ds){unsigned n=d.at("input_active"),k=d.at("cap");
            for(auto p:{std::make_pair(n+1,k),std::make_pair(n,k+1)})
                if(p.first<=a.max_input&&p.second<=a.max_cap&&!ds.contains(domain_key(p.first,p.second)))
                    next[{p.first+p.second,p.first,p.second}]={{"input_active",p.first},{"cap",p.second}};
        }
        if(!next.empty()){auto d=next.begin()->second;ds[domain_key(d.at("input_active"),d.at("cap"))]=d;}
        m["segment_next_expansion"]=finished+a.adapt_every;
    }
    auto supported=[&](const J& d){
        unsigned n=d.at("input_active"),k=d.at("cap");
        if(n>a.max_input||k>a.max_cap)return false;
        for(const auto& seed:available)if(seed.input<=n&&seed.cap<=k)return true;
        return false;
    };

    bool any=false;for(const auto& d:ds)any=any||supported(d);
    if(!any){const auto& s=available.front();ds[domain_key(s.input,s.cap)]={{"input_active",s.input},{"cap",s.cap}};}
    std::set<unsigned> taken;std::map<std::string,unsigned> busy;
    for(const auto* j:running("seed"))if(j->value("segment_graph",false)){taken.insert(j->at("segment_slot").get<unsigned>());++busy[j->at("segment_domain").get<std::string>()];}
    unsigned slot=0;while(taken.count(slot))++slot;
    if(slot>=slots)return nullptr;
    const auto turn=count(m,"segment_submitted");auto chosen=ds.end();
    for(auto it=ds.begin();it!=ds.end();++it){
        if(!supported(it.value()))continue;
        auto cost=[&](const auto& q){return std::make_tuple(count(q.value(),"submitted"),number(q.value(),"cpu_seconds"),q.key());};
        if(chosen==ds.end()||cost(it)<cost(chosen))chosen=it;
    }

    if(turn%4!=0){double best=INFINITY;
        for(auto it=ds.begin();it!=ds.end();++it){if(!supported(it.value())||!count(it.value(),"new_families"))continue;
            double f=number(it.value(),"best_graph_F",INFINITY);
            double score=f+std::log2(a.pattern_seconds+number(it.value(),"cpu_seconds")+busy[it.key()]*a.pattern_seconds)-std::log2(double(count(it.value(),"new_families")));
            if(score<best){best=score;chosen=it;}
        }
    }
    if(chosen==ds.end())return nullptr;
    unsigned input=chosen.value().at("input_active"),cap=chosen.value().at("cap");
    J seeds=J::array();
    for(const auto& seed:available)if(seed.input<=input&&seed.cap<=cap)seeds.push_back(seed.states);
    require(!seeds.empty(),"segment job needs a retained legal full-length witness");
    auto last=(m.contains("segment_slots")?m.at("segment_slots").value(std::to_string(slot),J::object()):J::object());
    const auto nodes=std::uint64_t(seeds.size())*(cipher.R+1);
    const auto cells=unsigned(cipher.cfg.value("cells",16));
    const auto per_node=std::uint64_t(832)+4*std::uint64_t(cipher.R+1)*(48+((4*cells*cipher.R+64)/64)*8);

    const auto observed=count(last,"reservation_revision")==2?count(last,"peak_accounted_bytes",Resources::minimum):Resources::minimum;
    const auto hint=std::max({Resources::minimum,sat_mul(nodes,per_node),observed});
    return {{"kind","seed"},{"segment_graph",true},{"segment_domain",chosen.key()},{"segment_slot",slot},
        {"cap",cap},{"input_active",input},{"active",input+(cipher.R-1)*cap},
        {"copies",std::array<unsigned,3>{1,2,4}[count(chosen.value(),"submitted")%3]},
        {"row_work",a.segment_work},{"initial_trails",seeds},
        {"segment_cache",(scratch/"segments"/(std::to_string(slot)+".bin")).string()},
        {"segment_memory_hint",hint},
        {"seconds",a.pattern_seconds},{"keep",std::max(a.keep,2u*unsigned(cipher.R))},{"evaluation",false}};
}
