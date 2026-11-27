
using DKey=std::tuple<double,double,double,std::string>;
enum Lane : unsigned { PLAN=0, FRONTIER=1, FIRST_SCREEN=2, AGAIN_SCREEN=3 };
struct Entry { std::array<DKey,2> key; unsigned mask; };
std::array<std::array<std::set<DKey>,2>,4> eval_order;
std::map<std::string,Entry> eval_entries;
std::set<std::string> eval_dirty,ep_dirty,role_dirty,pattern_order_dirty;
bool eval_built=false,ep_built=false,pattern_order_built=false;
std::uint64_t eval_reindexed=0,eval_visits=0,ep_visits=0,ep_reindexed=0;
using FairKey=std::tuple<std::uint64_t,std::uint64_t,std::string>;
using YieldKey=std::pair<double,FairKey>;
struct EpEntry { double F;std::array<FairKey,2> fair;std::array<YieldKey,2> yield; };
std::set<std::pair<double,std::string>> ep_mass;
std::array<std::set<FairKey>,2> ep_fair;
std::array<std::set<YieldKey>,2> ep_yield;
std::map<std::string,EpEntry> ep_entries;
using PatKey=std::tuple<std::uint64_t,int,std::string>;
std::set<PatKey> pat_order;
std::map<std::string,PatKey> pat_keys;
std::size_t fresh_patterns=0;
void mark_case(const std::string& id) { dc.insert(id);eval_dirty.insert(id); }
void invalidate_dispatch() {
    eval_built=ep_built=pattern_order_built=false;eval_entries.clear();ep_entries.clear();pat_keys.clear();
    for(auto& lane:eval_order)for(auto& order:lane)order.clear();
    ep_mass.clear();for(auto& x:ep_fair)x.clear();for(auto& x:ep_yield)x.clear();
    pat_order.clear();fresh_patterns=0;eval_dirty.clear();ep_dirty.clear();role_dirty.clear();pattern_order_dirty.clear();
}
void refresh_eval_order() {
    refresh_ranking();
    if(!eval_built){for(const auto& kv:cases)eval_dirty.insert(kv.first);eval_built=true;}
    for(const auto& id:eval_dirty){
        ++eval_reindexed;
        auto old=eval_entries.find(id);
        if(old!=eval_entries.end()){
            for(unsigned l=0;l<4;++l)if(old->second.mask&(1u<<l))
                for(unsigned k=0;k<2;++k)eval_order[l][k].erase(old->second.key[k]);
            eval_entries.erase(old);
        }
        const auto& c=cases.at(id);
        if(!c.at("exact").is_null() || covered(c))continue;
        unsigned mask=0;
        if(c.at("plan").is_null()) mask=1u<<PLAN;
        else {
            if(!c.at("frontier").empty())mask|=1u<<FRONTIER;
            const bool cheap=c.contains("root_bound")&&c.at("root_bound").contains("domain_volume")&&
               integer(c.at("root_bound").at("domain_volume"))*std::max<std::uint64_t>(1,count(c,"active"))<=Z(a.leaf_operations);
            if(!cheap){
                if(!count(c,"screen_attempts"))mask|=1u<<FIRST_SCREEN;
                if(count(c,"screen_attempts")<2 && number(c.at("screen"),"ess")<8)mask|=1u<<AGAIN_SCREEN;
            }
        }
        if(!mask)continue;
        const auto& r=rank_keys.at(id);double f=std::get<0>(r);
        Entry entry{{DKey{f,f,std::get<1>(r),id},DKey{mass_cost_score(c,f,a.exact_seconds),f,std::get<1>(r),id}},mask};
        for(unsigned l=0;l<4;++l)if(mask&(1u<<l))for(unsigned k=0;k<2;++k)eval_order[l][k].insert(entry.key[k]);
        eval_entries.emplace(id,std::move(entry));
    }
    eval_dirty.clear();
}
bool dispatch_available(const std::string& id){
    ++eval_visits;return number(cases.at(id),"next_at")<=elapsed()&&!admission_blocked(id);
}
void refresh_pattern_order(){
    if(!pattern_order_built){for(const auto& kv:patterns)pattern_order_dirty.insert(kv.first);pattern_order_built=true;}
    for(const auto& id:pattern_order_dirty){
        auto old=pat_keys.find(id);
        if(old!=pat_keys.end()){if(!std::get<0>(old->second))--fresh_patterns;pat_order.erase(old->second);}
        const auto& p=patterns.at(id);PatKey key{count(p,"searches"),p.at("active").get<int>(),id};
        if(!std::get<0>(key))++fresh_patterns;
        pat_order.insert(key);pat_keys[id]=std::move(key);
    }
    pattern_order_dirty.clear();
}
J* indexed_breadth(){
    refresh_pattern_order();std::set<std::string> busy;
    for(const auto* j:running("search"))busy.insert(j->at("pid"));
    for(const auto& k:pat_order)if(!busy.count(std::get<2>(k)))return &patterns.at(std::get<2>(k));
    return nullptr;
}
void update_ep_entry(const std::string& ep,double f,bool valid){
    auto old=ep_entries.find(ep);
    if(old!=ep_entries.end()){
        ep_mass.erase({old->second.F,ep});
        for(unsigned k=0;k<2;++k){ep_fair[k].erase(old->second.fair[k]);ep_yield[k].erase(old->second.yield[k]);}
        ep_entries.erase(old);
    }
    if(!valid)return;
    EpEntry e;e.F=f;const auto& dict=m.at("endpoint_roles");const J empty=J::object();
    const auto& role=dict.contains(ep)?dict.at(ep):empty;
    ep_mass.emplace(f,ep);
    for(unsigned k=0;k<2;++k){
        const char* name=k?"expand":"ensemble";const auto n=count(role,name);
        e.fair[k]={n,count(role,(std::string(name)+"_last").c_str()),ep};
        e.yield[k]={f+std::log2(1.+n),e.fair[k]};ep_fair[k].insert(e.fair[k]);ep_yield[k].insert(e.yield[k]);
    }
    ep_entries.emplace(ep,std::move(e));
}
void refresh_ep_order(){
    refresh_ranking();
    if(!ep_built){for(const auto& kv:endpoint_cases)ep_dirty.insert(kv.first);ep_built=true;}
    for(const auto& ep:ep_dirty){
        bool valid=true;const double f=dispatch_endpoint_score(ep,valid);update_ep_entry(ep,f,valid);
        ++ep_reindexed;role_dirty.erase(ep);
    }
    ep_dirty.clear();
    for(const auto& ep:role_dirty){auto old=ep_entries.find(ep);if(old!=ep_entries.end())update_ep_entry(ep,old->second.F,true);}
    role_dirty.clear();
}
std::string indexed_endpoint(const std::string& role,unsigned window,bool use_yield) {
    refresh_ep_order();const unsigned k=role=="expand";std::map<std::string,unsigned> busy;
    for(const auto* j:running(role))++busy[j->at("endpoint").get<std::string>()];
    auto available=[&](const std::string& ep){++ep_visits;auto b=busy.find(ep);return b==busy.end()||b->second<(k?1u:2u);};
    const bool fair=count(m.at("present_roles"),role.c_str())%3==2;
    if(!fair&&window){
        std::string best;unsigned seen=0;
        for(const auto& v:ep_mass){const auto& ep=v.second;if(!available(ep))continue;
            const auto& cur=ep_entries.at(ep);
            if(best.empty()||(use_yield?cur.yield[k]<ep_entries.at(best).yield[k]:cur.fair[k]<ep_entries.at(best).fair[k]))best=ep;
            if(++seen==window)break;
        }return best;
    }
    if(!fair&&use_yield){for(const auto& v:ep_yield[k])if(available(std::get<2>(v.second)))return std::get<2>(v.second);}
    else {for(const auto& v:ep_fair[k])if(available(std::get<2>(v)))return std::get<2>(v);}
    return {};
}
