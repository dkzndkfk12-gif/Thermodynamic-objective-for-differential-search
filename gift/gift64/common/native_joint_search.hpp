

void joint_search_run(const Options &o, const Catalog &cat, Timer &timer) {
    RNG rng(o.seed);
    std::vector<Real> ts={0,.125L,.25L,.5L,.75L,1,1.25L,1.5L,1.75L,2,2.5L,3,3.5L,4,5,6,8,10,12,16};
    std::vector<Level> levels;
    std::vector<State> states;
    for (auto t:ts) {
        levels.push_back(level(t));
        states.push_back(random_state(cat,o.copies,rng,-1,""));
        states.back().tag=int(states.size())-1;
    }
    bool hard_active=false;
    if (!o.bridge.empty()) {
        std::ifstream in(o.bridge);
        std::string magic, ep, internal, common_ep;
        int n=0,p=-1;
        if (!(in>>magic>>n) || magic!="THERMO_JOINT_WITNESSES_V1" || n!=int(cat.m.size()))
            throw std::runtime_error("invalid joint initialization header");
        std::vector<Copy> starts;
        std::set<int> seen;
        for(int i=0;i<n;++i) {
            if(!(in>>p>>ep>>internal) || p<0 || p>=n || !seen.insert(p).second)
                throw std::runtime_error("invalid joint initialization member");
            if(i==0) common_ep=ep;
            if(ep!=common_ep) throw std::runtime_error("joint starts must share endpoints");
            auto one=random_state(cat,1,rng,p,ep).c.front();
            auto values=internal=="-"?std::vector<unsigned char>{}:unhex(internal);
            if(values.size()!=cat.m[p].used.size()) throw std::runtime_error("joint coordinate count");
            for(size_t j=0;j<values.size();++j) {
                int v=cat.m[p].used[j];
                if(std::find(cat.allowed[v].begin(),cat.allowed[v].end(),values[j])==cat.allowed[v].end())
                    throw std::runtime_error("joint coordinate outside domain");
                one.x[v]=values[j];
            }
            refresh(cat,one);
            if(weight(cat,one)<0) throw std::runtime_error("illegal joint starting witness");
            starts.push_back(std::move(one));
        }
        if(in>>magic) throw std::runtime_error("extra joint initialization content");
        State cold=states.back();

        for(auto &copy:cold.c) copy=starts[draw(rng,int(starts.size()))];
        states.back()=cold;
        states.push_back(cold); levels.push_back(level(16,true)); hard_active=true;
    }

    struct MemberHit {int p=0;Real w=0;std::string x;double first=0,last=0;};
    static_assert(!std::numeric_limits<decltype(MemberHit{}.w)>::is_integer, "Joint witness weight must preserve fractions");
    struct EndpointHit {uint64_t hard=0,soft=0;std::map<int,MemberHit> members;};
    std::map<std::string,EndpointHit> buffer;
    uint64_t steps=0,observations=0,trimmed=0,rows=0,pattern_changes=0,mixed_hard_visits=0;
    std::vector<uint64_t> attempt(ts.size(),0),accepted(ts.size(),0),member_hits(cat.m.size(),0);
    auto trim=[&](size_t keep) {
        std::vector<std::string> keys;for(const auto &kv:buffer)keys.push_back(kv.first);
        std::sort(keys.begin(),keys.end(),[&](const auto &a,const auto &b){
            auto &x=buffer.at(a),&y=buffer.at(b);
            return std::make_tuple(x.hard,x.soft,a)>std::make_tuple(y.hard,y.soft,b);
        });
        std::set<std::string> selected;
        for(size_t i=0;i<keys.size() && i<(keep+1)/2;++i)selected.insert(keys[i]);
        std::sort(keys.begin(),keys.end(),[&](const auto&a,const auto&b){
            auto x=key_rank(a,o.seed),y=key_rank(b,o.seed);return x!=y?x<y:a<b;
        });
        for(const auto &k:keys){if(selected.size()>=keep)break;selected.insert(k);}
        for(auto it=buffer.begin();it!=buffer.end();) {
            if(!selected.count(it->first)){it=buffer.erase(it);++trimmed;}else ++it;
        }
    };
    auto collect=[&](const State &s,bool hard) {
        std::map<int,const Copy*> legal;
        for(const auto &copy:s.c) {
            const Real w=weight(cat,copy);if(w<0)continue;
            auto it=legal.find(copy.p);
            if(it==legal.end() || w<weight(cat,*it->second)) legal[copy.p]=&copy;
        }
        if(legal.empty())return;
        auto e=hex(s.c.front().x,0,cat.ne);
        if(!buffer.count(e) && buffer.size()>=8192)trim(4096);
        auto &h=buffer[e];if(hard)++h.hard;else ++h.soft;
        if(hard && legal.size()>1)++mixed_hard_visits;
        ++observations;
        for(auto item:legal) {
            const auto &copy=*item.second;const Real w=weight(cat,copy);
            ++member_hits.at(copy.p);
            auto it=h.members.find(copy.p);
            if(it==h.members.end()) {
                MemberHit v;v.p=copy.p;v.w=w;v.x=hex(copy.x,cat.ne,cat.N);
                v.first=timer.seconds();it=h.members.emplace(copy.p,std::move(v)).first;
            }
            if(w<it->second.w){it->second.w=w;it->second.x=hex(copy.x,cat.ne,cat.N);}
            it->second.last=timer.seconds();
        }
    };
    try {
        for(uint64_t k=0;k<o.sweeps;++k) {
            for(size_t lane=0;lane<states.size();++lane) {
                std::vector<int> before;for(auto &copy:states[lane].c)before.push_back(copy.p);
                sweep(cat,states[lane],levels[lane],-1,true,k,rng,timer);
                for(size_t c=0;c<before.size();++c)pattern_changes+=before[c]!=states[lane].c[c].p;
            }
            ++steps;
            if(k%4==0)for(size_t lane=states.size()-3;lane<states.size();++lane)
                collect(states[lane],levels[lane].hard);
            if(!hard_active && std::isfinite(density(cat,states.back(),level(16,true),-1))) {
                states.push_back(states.back());levels.push_back(level(16,true));hard_active=true;
            }
            for(size_t j=k%2;j+1<states.size();j+=2) {
                ++attempt[j];
                Real d=density(cat,states[j+1],levels[j],-1)+density(cat,states[j],levels[j+1],-1)
                      -density(cat,states[j],levels[j],-1)-density(cat,states[j+1],levels[j+1],-1);
                if(!std::isnan(d)&&accept(rng,d)){std::swap(states[j],states[j+1]);++accepted[j];}
            }
        }
    } catch(const Budget&) {}
    trim(size_t(o.limit));
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"endpoint\thard_visits\tsoft_visits\tpattern\tinternal\ttrail_weight\tfirst_seconds\tlast_seconds\n"
       <<std::setprecision(std::numeric_limits<Real>::max_digits10);
    for(const auto &kv:buffer)for(const auto &mv:kv.second.members) {
        const auto &h=mv.second;
        out<<kv.first<<'\t'<<kv.second.hard<<'\t'<<kv.second.soft<<'\t'<<h.p<<'\t'<<h.x<<'\t'
           <<h.w<<'\t'<<h.first<<'\t'<<h.last<<'\n';++rows;
    }
    out.close();publish(o.out);
    std::ofstream diag(o.out+".search.json");
    diag<<"{"<<collapsed_diagnostics()<<"\"joint_patterns\":"<<cat.m.size()<<",\"sweeps\":"<<steps
        <<",\"hard_lane_activated\":"<<(hard_active?"true":"false")
        <<",\"observations\":"<<observations<<",\"stream_rows\":"<<rows
        <<",\"archive_discarded_endpoints\":"<<trimmed<<",\"pattern_changes\":"<<pattern_changes
        <<",\"mixed_pattern_hard_visits\":"<<mixed_hard_visits
        <<",\"equilibrium_certified\":false,\"target\":\"sum_pattern_Z(e)^copies\",\"member_hits\":[";
    for(size_t i=0;i<member_hits.size();++i){if(i)diag<<',';diag<<member_hits[i];}
    diag<<"],\"swaps\":[";
    for(size_t i=0;i<attempt.size();++i){if(i)diag<<',';diag<<'['<<attempt[i]<<','<<accepted[i]<<']';}
    diag<<"]}\n";
}
