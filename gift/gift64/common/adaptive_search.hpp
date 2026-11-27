#pragma once
#include "search_allocation.hpp"
namespace thermo {
inline constexpr const char *adaptive_revision = "ADAPTIVE_FEASIBLE_ACTION_UNION_V2";

inline J &adaptive_stats(J &p, const std::string &mode) {
    if (!p.contains("adaptive_actions")) p["adaptive_actions"] = J::object();
    auto &s = p["adaptive_actions"][mode];
    if (s.is_null()) s = J{{"submitted",0},{"receipts",0},{"cpu_seconds",0},
        {"evaluation_cpu_seconds",0},{"new_families",0},{"duplicates",0},
        {"screened_families",0},{"certified_families",0},{"window",J::array()}};
    return s;
}
inline void adaptive_event_reward(J &event) {
    std::map<std::string, double> endpoint_ratios;
    for (const auto &v : event.at("new").items()) {
        const auto &c = v.value();
        if (!c.at("rank_F").is_number()) continue;
        double d = number(event,"reference_F") - number(c,"rank_F");
        endpoint_ratios[c.at("endpoint").get<std::string>()] += std::exp2(std::min(0.,d));
    }
    double reward = 0;
    for (const auto &kv : endpoint_ratios) reward = std::max(reward,std::min(1.,kv.second));
    event["reward"] = reward;
}

inline double adaptive_priority(const J &p, const std::string &mode, double prior_F,
                                double job_seconds, double inflight_seconds) {
    if (!std::isfinite(prior_F)) return -INFINITY;
    double reward=0,cpu=0;
    if (p.contains("adaptive_actions") && p.at("adaptive_actions").contains(mode))
        for (const auto &e : p.at("adaptive_actions").at(mode).at("window")) {
            reward += number(e,"reward");
            cpu += number(e,"cpu_seconds") + number(e,"evaluation_cpu_seconds");
        }
    return -prior_F + std::log2(1+reward) - std::log2(job_seconds+cpu+inflight_seconds);
}
inline void adaptive_submit(std::map<std::string,J> &patterns, std::set<std::string> &dirty,
                            const J &job) {
    if (!job.contains("adaptive_action")) return;
    const auto &v=job.at("adaptive_action");
    auto &p=patterns.at(v.at("pid").get<std::string>());
    auto &s=adaptive_stats(p,v.at("mode"));
    increment(s,"submitted");
    if (job.contains("joint_members")) {
        require(count(p,"adaptive_joint_turn")==count(v,"joint_sequence"),"Stale joint group sequence");
        increment(p,"adaptive_joint_turn");
        for (const auto &member : job.at("joint_members")) {
            auto &q=patterns.at(member.at("pid").get<std::string>());
            increment(q,"joint_exposures");dirty.insert(q.at("id"));
        }
    }
    dirty.insert(p.at("id"));
}
inline void adaptive_receipt(std::map<std::string,J> &patterns, std::map<std::string,J> &cases,
                             std::set<std::string> &dirty, const J &job, const J &receipt,
                             const std::vector<std::string> &created, std::uint64_t duplicates,
                             unsigned window) {
    if (!job.contains("adaptive_action")) return;
    const auto &v=job.at("adaptive_action");
    auto &p=patterns.at(v.at("pid").get<std::string>());
    auto &s=adaptive_stats(p,v.at("mode"));
    double cpu=number(receipt,"cpu_seconds")+number(receipt,"cpp_thread_cpu_seconds");
    increment(s,"receipts");increment(s,"new_families",created.size());increment(s,"duplicates",duplicates);
    s["cpu_seconds"]=number(s,"cpu_seconds")+cpu;
    const auto diag=receipt.value("diagnostics",J::object());
    for(const auto *key:{"pattern_changes","mixed_pattern_hard_visits","observations"})
        increment(s,key,count(diag,key));
    if(job.contains("joint_members")) {
        s["last_joint_pattern_ids"]=J::array();
        for(const auto &member:job.at("joint_members"))s["last_joint_pattern_ids"].push_back(member.at("pid"));
    }
    J event{{"job",job.at("id")},{"cpu_seconds",cpu},{"evaluation_cpu_seconds",0},
        {"reference_F",v.at("reference_F")},{"reward",0},{"new",J::object()}};
    for (const auto &id:created) {
        auto &c=cases.at(id);

        event["new"][id]={{"endpoint",c.at("endpoint")},{"rank_F",J()},
                          {"screened",false},{"certified",false}};
        c["adaptive_source"]={{"pid",p.at("id")},{"mode",v.at("mode")},{"job",job.at("id")}};
    }
    s["window"].push_back(event);
    while (window && s["window"].size()>window) s["window"].erase(s["window"].begin());
    dirty.insert(p.at("id"));
}
inline void adaptive_feedback(std::map<std::string,J> &patterns, std::set<std::string> &dirty,
                              J &c, double rank_F, bool screened, bool certified, double cpu) {
    if (!c.contains("adaptive_source")) return;
    auto &src=c.at("adaptive_source");
    auto &p=patterns.at(src.at("pid").get<std::string>());
    auto &s=adaptive_stats(p,src.at("mode"));
    s["evaluation_cpu_seconds"]=number(s,"evaluation_cpu_seconds")+cpu;
    if(screened && !c.value("adaptive_screen_credited",false)) {
        c["adaptive_screen_credited"]=true;increment(s,"screened_families");
    }
    if(certified && !c.value("adaptive_exact_credited",false)) {
        c["adaptive_exact_credited"]=true;increment(s,"certified_families");
    }
    for (auto &event:s["window"]) if(event.at("job")==src.at("job")) {
        event["evaluation_cpu_seconds"]=number(event,"evaluation_cpu_seconds")+cpu;
        auto &v=event["new"].at(c.at("id").get<std::string>());
        v["rank_F"]=std::isfinite(rank_F)?J(rank_F):J();
        v["screened"]=screened;v["certified"]=certified;

        adaptive_event_reward(event);break;
    }
    dirty.insert(p.at("id"));
}
inline J adaptive_summary(const std::map<std::string,J> &patterns, const Options &a) {
    J totals=J::object();
    for (const auto &kv:patterns) if(kv.second.contains("adaptive_actions"))
        for (const auto &action:kv.second.at("adaptive_actions").items()) {
            auto &row=totals[action.key()];if(row.is_null())row=J::object();
            for(const auto *key:{"submitted","receipts","new_families","duplicates","screened_families","certified_families",
                                "pattern_changes","mixed_pattern_hard_visits","observations"})
                increment(row,key,count(action.value(),key));
            for(const auto *key:{"cpu_seconds","evaluation_cpu_seconds"})
                row[key]=number(row,key)+number(action.value(),key);
        }
    return {{"revision",adaptive_revision},{"enabled",a.adaptive_patterns},
            {"joint_pattern_limit",a.joint_patterns},{"recent_jobs",a.adaptive_window_jobs},
            {"actions",totals},{"rewards_are_probability_certificates",false},
            {"policy","mass prior times recent new-family yield per CPU; coverage retained; delayed AIS/exact feedback"}};
}
}
