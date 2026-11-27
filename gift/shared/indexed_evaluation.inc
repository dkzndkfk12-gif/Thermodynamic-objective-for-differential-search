inline J Campaign::indexed_evaluation_job(unsigned cap,std::uint64_t turn) {
    refresh_eval_order();const unsigned k=a.cost_priority&&turn%4==0;
    const unsigned auxiliary=std::max(1u,cap/8);
    auto first=[&](Lane lane,const char* kind)->const DKey*{
        for(const auto& key:eval_order[lane][k]){
            const auto& id=std::get<3>(key);
            if(dispatch_available(id)&&running(kind,id).empty())return &key;
        }return nullptr;
    };
    if(running("screen").size()<auxiliary){
        const DKey *x=turn%3!=0?first(FIRST_SCREEN,"screen"):nullptr,*y=turn%8==0?first(AGAIN_SCREEN,"screen"):nullptr;
        if(!x||(y&&*y<*x))x=y;
        if(x){const auto& c=cases.at(std::get<3>(*x));return {{"kind","screen"},{"cid",c.at("id")},{"states",c.at("states")},
            {"pattern",patterns.at(c.at("pid")).at("pattern")},{"paths",a.paths},{"steps",a.steps},{"seconds",a.screen_seconds},{"evaluation",true}};}
    }
    const DKey* plan=running("plan").size()<auxiliary?first(PLAN,"plan"):nullptr;
    auto planjob=[&](){const auto& c=cases.at(std::get<3>(*plan));return J{{"kind","plan"},{"cid",c.at("id")},{"states",c.at("states")},
        {"seconds",a.plan_seconds*std::exp2(std::min(4.,number(c,"plan_failures")))},{"leaf_operations",a.leaf_operations},
        {"max_frontier",a.chunk_frontier},{"evaluation",true}};};
    const auto& front=eval_order[FRONTIER][k];auto it=front.begin();
    while(it!=front.end()&&!dispatch_available(std::get<3>(*it)))++it;
    if(plan&&(it==front.end()||turn%8==1))return planjob();
    auto exactjob=[&](J* c)->J{
        auto id = c->at("id").get<std::string>();
        auto busy = running("exact", id);
        std::set<std::string> taken;
        for(auto *job:busy)taken.insert(job->at("box_id"));

        std::string key;
        std::pair<int, int> best{-1, 0};
        for (auto it = (*c)["frontier"].begin(); it != (*c)["frontier"].end(); ++it) {
            if (taken.count(it.key()))
                continue;
            int mass = 0;
            for (auto &b : it.value()) {
                Z mask(b[1].get<std::string>(), 16);
                mass += mpz_popcount(mask.get_mpz_t());
            }
            auto k = std::make_pair(int(it.value().size()), -mass);
            if (key.empty() || k > best) {
                key = it.key();
                best = k;
            }
        }
        if (key.empty())
            return J();
        auto attempts = count((*c)["attempts"], key.c_str());
        J j{{"kind", "exact"},
            {"cid", id},
            {"states", (*c)["states"]},
            {"box_id", key},
            {"box", (*c)["frontier"][key]},
            {"seconds", exact_seconds_for(*c, key)},
            {"leaf_operations",
             std::max<std::uint64_t>(1024, a.leaf_operations >> std::min<std::uint64_t>(attempts, 12))},
            {"max_frontier", a.chunk_frontier},

            {"evaluation", true}};

        const auto used=used_cores(true);
        const bool costly=number(c->at("plan"),"operation_score")>a.leaf_operations||attempts>0;
        if(costly&&used<cap&&!c->value("unsplittable",J::object()).contains(key)){
            bool splitting=false;for(const auto& x:active)if(x.second.job.value("partition_target",0)>1){splitting=true;break;}
            if(!splitting){
                std::size_t available=0;
                for(const auto& entry:front){
                    const auto& other=std::get<3>(entry);if(!dispatch_available(other))continue;
                    const auto n=cases.at(other).at("frontier").size();available+=n-std::min(n,running("exact",other).size());
                    if(available>=cap-used)break;
                }
                if(available<cap-used){j["partition_target"]=std::min<std::size_t>(a.chunk_frontier,cap-used-available+1);}
            }
        }
        return j;
    };
    std::vector<std::string> focus;
    for(;it!=front.end()&&focus.size()<a.focus;++it)if(dispatch_available(std::get<3>(*it)))focus.push_back(std::get<3>(*it));
    m["focus"]=focus;
    std::stable_sort(focus.begin(),focus.end(),[&](const auto& x,const auto& y){
        return std::make_pair(running("exact",x).size(),rank_keys.at(x))<std::make_pair(running("exact",y).size(),rank_keys.at(y));});
    for(const auto& id:focus){auto j=exactjob(&cases.at(id));if(!j.is_null())return j;}
    for(;it!=front.end();++it){const auto& id=std::get<3>(*it);if(!dispatch_available(id))continue;
        auto j=exactjob(&cases.at(id));if(!j.is_null())return j;}
    return plan?planjob():J();
}
