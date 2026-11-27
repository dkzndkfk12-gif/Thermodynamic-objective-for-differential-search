#pragma once
#include "campaign.hpp"
namespace thermo {
inline J Campaign::adaptive_search_job(J job, const std::vector<J *> &witnessed, bool targeted) {
    if (!a.adaptive_patterns) return job;
    const auto copies = job.at("copies").get<unsigned>();
    const std::string single = "single_" + std::to_string(copies), joint = "joint_" + std::to_string(copies);
    std::map<std::pair<std::string,std::string>,double> inflight;
    for (const auto &kv : active) if (kv.second.job.contains("adaptive_action")) {
        const auto &v=kv.second.job.at("adaptive_action");
        inflight[{v.at("pid").get<std::string>(),v.at("mode").get<std::string>()}] += number(kv.second.job,"seconds");
    }
    auto best_case = [&](const std::string &pid) -> const J * {
        auto it=search_quality.find(pid);
        return it==search_quality.end() || it->second.empty() ? nullptr : &cases.at(it->second.begin()->second);
    };
    auto priority = [&](const J &p,const std::string &mode,double F) {
        double value=adaptive_priority(p,mode,F,a.search_seconds,inflight[{p.at("id"),mode}]);
        if(a.cost_priority && p.contains("mass_work") && p.at("mass_work").at("rate_F").is_number()){
            double logratio=F+std::log2(a.search_seconds)-number(p.at("mass_work"),"rate_F");
            value+=std::log2(1+std::exp2(std::min(0.,logratio)));
        }
        return value;
    };
    struct Choice {
        J *root = nullptr;
        const J *start = nullptr;
        std::string mode;
        double reference = INFINITY, score = -INFINITY;
        std::vector<const J *> members;
    } chosen;

    using GroupKey = std::tuple<std::string, std::string, std::string>;
    struct Group {
        std::vector<const J *> rows;
        std::map<std::string, std::size_t> index;
    };
    std::map<GroupKey, Group> groups;
    auto group_for = [&](const J &p, const J &start) -> const Group & {
        const auto &pat = p.at("pattern");
        GroupKey key{start.at("endpoint"), pat.front(), pat.back()};
        auto inserted = groups.try_emplace(key);
        auto &group = inserted.first->second;
        if (!inserted.second) return group;
        auto it = endpoint_cases.find(start.at("endpoint"));
        std::map<std::string, const J *> unique;
        if (it != endpoint_cases.end()) for (const auto &id : it->second) {
            const auto &c = cases.at(id);
            const std::string pid = c.at("pid");
            const auto &q = patterns.at(pid).at("pattern");
            if (q.front() != pat.front() || q.back() != pat.back() ||
                !std::isfinite(replica_quality(c))) continue;
            auto old = unique.find(pid);
            if (old == unique.end() ||
                std::make_tuple(replica_quality(c), c.at("id")) <
                std::make_tuple(replica_quality(*old->second), old->second->at("id"))) unique[pid] = &c;
        }
        for (const auto &kv : unique) group.rows.push_back(kv.second);
        std::sort(group.rows.begin(), group.rows.end(), [&](const J *x, const J *y) {
            return std::make_tuple(replica_quality(*x), x->at("id")) <
                   std::make_tuple(replica_quality(*y), y->at("id"));
        });
        for (std::size_t i = 0; i < group.rows.size(); ++i) group.index[group.rows[i]->at("pid")] = i;
        return group;
    };
    auto consider = [&](Choice candidate) {
        auto tie = [](const Choice &x) {
            return std::make_tuple(x.root->at("mass_search_last_turn"),
                                   x.root->at("mass_search_order"), x.root->at("id"), x.mode);
        };
        if (!chosen.root || candidate.score > chosen.score ||
            (candidate.score == chosen.score && tie(candidate) < tie(chosen))) chosen = std::move(candidate);
    };
    auto evaluate = [&](J *p) {
        auto *start = best_case(p->at("id"));
        if (!start) return;
        double reference = replica_quality(*start);
        if (!std::isfinite(reference)) return;
        consider({p, start, single, reference, priority(*p, single, reference), {}});
        if (!targeted || copies <= 1 || a.joint_patterns == 1) return;
        const auto &group = group_for(*p, *start);
        if (group.rows.size() < 2) return;
        const auto root_index = group.index.at(p->at("id"));
        const auto companions = group.rows.size() - 1;
        std::vector<const J *> members{start};
        const auto limit = a.joint_patterns ? std::min<std::size_t>(a.joint_patterns - 1, companions)
                                           : companions;
        const auto offset = count(*p, "adaptive_joint_turn") % companions;
        for (std::size_t i = 0; i < limit; ++i) {
            const auto k = (offset + i) % companions;
            members.push_back(group.rows[k + (k >= root_index)]);
        }
        double lowest = reference;
        for (auto *c : members) lowest = std::min(lowest, replica_quality(*c));
        double relative = 0;
        for (auto *c : members) relative += std::exp2(lowest - replica_quality(*c));
        const double jointF = lowest - std::log2(relative);
        consider({p, start, joint, jointF, priority(*p, joint, jointF), std::move(members)});
    };
    J *root=&patterns.at(job.at("pid").get<std::string>());
    if (targeted && copies>1) {
        auto pool=witnessed;
        if(a.replica_focus_patterns && pool.size()>a.replica_focus_patterns) {
            std::sort(pool.begin(),pool.end(),[&](const J*x,const J*y){
                const auto *cx=best_case(x->at("id")),*cy=best_case(y->at("id"));
                return std::make_tuple(cx?replica_quality(*cx):double(INFINITY),x->at("id")) <
                       std::make_tuple(cy?replica_quality(*cy):double(INFINITY),y->at("id"));
            });
            pool.resize(a.replica_focus_patterns);
        }
        for (auto *p : pool) evaluate(p);
    } else evaluate(root);
    if (!chosen.root) return job;
    root = chosen.root;
    const std::string pid=root->at("id");
    const J *start=chosen.start;
    const double reference=chosen.reference;
    job["pid"]=pid;job["pattern"]=root->at("pattern");job["states"]=start->at("states");
    auto &allocation=job["search_allocation"];
    if (targeted) allocation["next_cursor"]=count(m.at("mass_search_allocation").at("group_cursors"),
                                                 allocation.at("role").get<std::string>().c_str());
    const int n=root->at("active");
    allocation["bucket"]=std::find(a.counts.begin(),a.counts.end(),n)!=a.counts.end()
        ? std::to_string(n):"outside_initial_counts";
    allocation["first_pattern_search"]=count(*root,"searches")==0;
    allocation["priority_F"]=reference;
    const std::string mode=chosen.mode;
    if (!chosen.members.empty()) {
        job["joint_members"] = J::array();
        for (auto *c : chosen.members) job["joint_members"].push_back({{"pid",c->at("pid")},
            {"case_id",c->at("id")},{"pattern",patterns.at(c->at("pid")).at("pattern")},
            {"states",c->at("states")}});
        allocation["reason"] = "adaptive_joint_replica";
    }
    if(targeted && mode==single) allocation["reason"]="adaptive_single_replica";
    job["adaptive_action"]={{"pid",pid},{"mode",mode},{"reference_F",reference},
        {"joint_sequence",count(*root,"adaptive_joint_turn")},{"revision",adaptive_revision},
        {"selection_log2_score",chosen.score},{"bundle_patterns",chosen.members.empty()?1:chosen.members.size()}};
    return job;
}

inline J Campaign::adaptive_expansion_job() {
    std::map<std::pair<std::string,std::string>,double> inflight;
    for(const auto &kv:active)if(kv.second.job.contains("adaptive_action")) {
        const auto &v=kv.second.job.at("adaptive_action");
        inflight[{v.at("pid"),v.at("mode")}]+=number(kv.second.job,"seconds");
    }
    const J *chosen=nullptr,*group=nullptr;
    int center=0;double best=-INFINITY;
    for(const auto &u:partial_cache)for(const auto &id:u.at("component_ids")) {
        const auto &c=cases.at(id.get<std::string>());const std::string pid=c.at("pid");
        auto &p=patterns.at(pid);
        const double F=number(u.at("mass"),"F",INFINITY);
        for(int k:{0,4,8,16,24,32}) {
            std::string mode="expand_"+std::to_string(k);
            double value=adaptive_priority(p,mode,F,a.expansion_seconds,inflight[{pid,mode}]);
            if(value>best) {best=value;chosen=&c;group=&u;center=k;}
        }
    }
    if(!chosen)return nullptr;
    const auto ep=chosen->at("endpoint").get<std::string>();
    auto n=count(m["expansions"],ep.c_str());
    return {{"kind","expand"},{"cid",chosen->at("id")},{"states",chosen->at("states")},
        {"center",center},{"seconds",a.expansion_seconds},{"evaluation",false},
        {"expansion_allocation",{{"endpoint",ep},{"sequence",n},{"certified_union",group->at("mass")},
                                  {"has_unfinished_patterns",count(*group,"unfinished_patterns")>0}}},
        {"adaptive_action",{{"pid",chosen->at("pid")},{"mode","expand_"+std::to_string(center)},
                             {"reference_F",group->at("mass").at("F")},{"revision",adaptive_revision}}}};
}
}
