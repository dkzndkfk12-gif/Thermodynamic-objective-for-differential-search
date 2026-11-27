#pragma once
#include "options.hpp"
namespace thermo {
inline void initialize_common_seeds(J &m,const std::vector<int> &counts,double elapsed) {
    if(!m.contains("skinny_common_value_seeds"))
        m["skinny_common_value_seeds"]={{"schema","SKINNY_COMMON_SEEDS_V1"},
            {"started_elapsed_seconds",elapsed},{"submitted",0},{"receipts",0},{"completed",0},
            {"incomplete",0},{"errors",0},{"interrupted",0},{"cpu_seconds",0.},
            {"new_patterns",0},{"new_families",0},{"duplicates",0},{"turn",0},
            {"next_endpoint_turn",0},{"by_active",J::object()},{"best",J()}};
    auto &s=m.at("skinny_common_value_seeds");
    require(s.at("schema")=="SKINNY_COMMON_SEEDS_V1" &&
        count(s,"receipts")==count(s,"completed")+count(s,"incomplete")+count(s,"errors") &&
        count(s,"receipts")+count(s,"interrupted")<=count(s,"submitted"),"Common seed accounting mismatch");
    for(int a:counts) if(!s["by_active"].contains(std::to_string(a)))
        s["by_active"][std::to_string(a)]={{"cursor",0},{"progress",J()},{"no_support",false},
            {"rows_completed",0},{"submissions",0},{"cpu_seconds",0.}};
}
inline J common_seed_job(const J &m,const Options &a,std::uint64_t campaign_seed) {
    const auto &s=m.at("skinny_common_value_seeds");
    const auto endpoint_turn=count(m.at("mass_search_allocation"),"endpoint_turn");
    if(endpoint_turn<count(s,"next_endpoint_turn"))return nullptr;
    const auto turn=count(s,"turn");
    for(std::size_t offset=0;offset<a.counts.size();++offset) {
        const auto i=(turn%a.counts.size()+offset)%a.counts.size();
        int active=a.counts[i];const auto &row=s.at("by_active").at(std::to_string(active));
        if(row.at("no_support").get<bool>())continue;

        return J{{"kind","seed"},{"seed_kind","common_value"},{"active",active},
            {"seconds",a.common_seed_seconds>0?a.common_seed_seconds:a.pattern_seconds},
            {"evaluation",false},{"keep",a.keep},{"common_seed_sequence",count(s,"submitted")},
            {"common_seed_turn",turn},{"common_seed_next_turn",sat_add(turn,offset+1)},
            {"common_seed_endpoint_turn",endpoint_turn},{"common_seed_every",a.common_seed_every},
            {"common_seed_campaign_seed",campaign_seed},{"common_seed_cursor",count(row,"cursor")},
            {"common_seed_progress",row.at("progress")}};
    }
    return nullptr;
}
inline void record_common_seed_submission(J &m,const J &job) {
    if(!is_common_seed(job))return;
    auto &s=m.at("skinny_common_value_seeds");
    auto &row=s.at("by_active").at(str(job.at("active")));
    const auto endpoint_turn=count(m.at("mass_search_allocation"),"endpoint_turn");
    require(count(job,"common_seed_sequence")==count(s,"submitted") &&
        count(job,"common_seed_turn")==count(s,"turn") &&
        count(job,"common_seed_cursor")==count(row,"cursor") &&
        job.at("common_seed_progress")==row.at("progress") && !row.at("no_support").get<bool>() &&
        count(job,"common_seed_endpoint_turn")==endpoint_turn && endpoint_turn>=count(s,"next_endpoint_turn"),
        "Stale common seed submission");
    increment(s,"submitted");increment(row,"submissions");
    s["next_endpoint_turn"]=sat_add(endpoint_turn,count(job,"common_seed_every"));
}
inline void record_common_seed_receipt(J &m,const J &job,const J &receipt,
    const J &new_patterns,std::uint64_t created,std::uint64_t duplicates) {
    if(!is_common_seed(job))return;
    auto &s=m.at("skinny_common_value_seeds");
    auto &row=s.at("by_active").at(str(job.at("active")));
    require(count(job,"common_seed_turn")==count(s,"turn") &&
        count(job,"common_seed_cursor")==count(row,"cursor"),"Common seed receipt cursor mismatch");
    const auto status=receipt.value("status",std::string());
    increment(s,"receipts");
    increment(s,status=="COMPLETE"?"completed":status=="ERROR"?"errors":"incomplete");
    const auto cpu=number(receipt,"cpu_seconds")+number(receipt,"cpp_thread_cpu_seconds");
    s["cpu_seconds"]=number(s,"cpu_seconds")+cpu;row["cpu_seconds"]=number(row,"cpu_seconds")+cpu;
    increment(s,"new_families",created);increment(s,"duplicates",duplicates);
    for(const auto &kv:new_patterns.items())increment(s,"new_patterns",kv.value().get<std::uint64_t>());
    if(receipt.contains("common_seed_progress"))row["progress"]=receipt.at("common_seed_progress");
    if(receipt.value("common_seed_no_support",false)) {
        require(status=="COMPLETE","Incomplete catalog cannot exclude a support count");
        row["no_support"]=true;row["progress"]=nullptr;
    } else if(status=="COMPLETE") {
        require(receipt.value("common_seed_row_complete",false),"Complete seed without completed transfer row");
        increment(row,"cursor");increment(row,"rows_completed");row["progress"]=nullptr;
    }
    if(receipt.contains("common_seed_supports"))row["supports"]=receipt.at("common_seed_supports");
    s["turn"]=job.at("common_seed_next_turn");
    if(status=="INCOMPLETE" || receipt.value("common_seed_no_support",false))
        s["next_endpoint_turn"]=count(m.at("mass_search_allocation"),"endpoint_turn");
    for(const auto &h:receipt.value("hits",J::array())) {
        const auto &cert=h.at("common_value_seed");
        if(s["best"].is_null() || compare_mass(cert.at("mass"),s["best"].at("mass"))>0)
            s["best"]={{"endpoint",h.at("endpoint")},{"input",h.at("input")},{"output",h.at("output")},
                {"mass",cert.at("mass")},{"pattern",h.at("pattern_record").at("pattern")},{"states",h.at("states")},
                {"certificate",cert},{"job",job.at("id")}};
    }
}
inline void record_common_seed_interruption(J &m,const J &job) {
    if(!is_common_seed(job))return;
    auto &s=m.at("skinny_common_value_seeds");
    increment(s,"interrupted");

    s["next_endpoint_turn"]=std::min(count(s,"next_endpoint_turn"),
        count(m.at("mass_search_allocation"),"endpoint_turn"));
}
inline J common_seed_summary(const J &m) {
    if(!m.contains("skinny_common_value_seeds"))return nullptr;
    J s=m.at("skinny_common_value_seeds");
    for(auto &row:s["by_active"]) {
        row["completed_round_layers"]=row["progress"].is_object()?row["progress"].at("prefix").size()-1:0;
        row.erase("progress");
    }
    s["scope"]="restricted common-value seed family; ranking bound only; overlaps ordinary exact sums";
    return s;
}
}
