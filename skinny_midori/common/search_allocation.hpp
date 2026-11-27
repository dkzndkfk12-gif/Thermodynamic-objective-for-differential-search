#pragma once
#include "options.hpp"
namespace thermo {
inline constexpr const char *mass_search_schema = "BALANCED_MASS_REPLICA_SEARCH_V1";

inline void register_search_pattern(J &m, J &p, std::set<std::string> &dirty) {
    if (p.contains("mass_search_order"))
        return;
    auto &s = m.at("mass_search_allocation");
    p["mass_search_order"] = count(s, "next_registration");
    increment(s, "next_registration");
    p["mass_search_last_turn"] = -1;
    p["mass_search_counts"] = J{{"1", 0}, {"2", 0}, {"4", 0}};
    p["mass_search_legacy_submissions"] = count(p, "searches");
    dirty.insert(p.at("id").get<std::string>());
}
inline void initialize_mass_search_allocation(J &m, std::map<std::string, J> &patterns,
                                             std::set<std::string> &dirty, double elapsed,
                                             bool resumed, bool enabled) {
    if (!enabled)
        return;
    if (!m.contains("mass_search_allocation"))
        m["mass_search_allocation"] =
            J{{"schema", mass_search_schema}, {"endpoint_turn", 0}, {"lane_turn", 0},
              {"next_registration", 0}, {"group_cursors", J::object()},
              {"dispatches", J::object()}, {"receipts", J::object()},
              {"cpu_seconds", J::object()}, {"started_elapsed_seconds", elapsed},
              {"legacy_history_unavailable", resumed}};
    require(m.at("mass_search_allocation").at("schema") == mass_search_schema,
            "Unknown mass-search allocation schema");

    for (auto &kv : patterns)
        register_search_pattern(m, kv.second, dirty);
}
inline void validate_mass_search_submission(const J &m, const J &j) {
    if (j.contains("search_allocation")) {
        const auto &s = m.at("mass_search_allocation"), &v = j.at("search_allocation");
        require(v.at("schema") == mass_search_schema && v.at("turn") == s.at("endpoint_turn"),
                "Stale endpoint-search allocation");
    }
    if (j.contains("expansion_allocation")) {
        const auto &v = j.at("expansion_allocation");
        require(count(v, "sequence") == count(m.at("expansions"), v.at("endpoint").get<std::string>().c_str()),
                "Stale pattern-expansion allocation");
    }
}
inline void record_mass_search_submission(J &m, std::map<std::string, J> &patterns,
                                         std::set<std::string> &dirty, const J &j) {
    if (!m.contains("mass_search_allocation"))
        return;
    auto &s = m.at("mass_search_allocation");
    const std::string kind = j.at("kind");
    if (kind == "pattern" || kind == "search" || kind == "expand" || kind == "seed" || kind=="activity")
        increment(s, "lane_turn");
    if (j.contains("expansion_allocation")) {
        const auto &v = j.at("expansion_allocation");
        increment(m["expansions"], v.at("endpoint").get<std::string>());
        increment(s["dispatches"], "expansion");
        if (v.at("has_unfinished_patterns").get<bool>())
            increment(s["dispatches"], "expansion_including_partial");
    }
    if (!j.contains("search_allocation"))
        return;
    const auto &v = j.at("search_allocation");
    auto &p = patterns.at(j.at("pid").get<std::string>());
    increment(p["mass_search_counts"], str(j.at("copies")));
    p["mass_search_last_turn"] = count(s, "endpoint_turn");
    dirty.insert(p.at("id").get<std::string>());
    increment(s, "endpoint_turn");
    s["group_cursors"][v.at("role").get<std::string>()] = v.at("next_cursor");
    increment(s["dispatches"], v.at("role").get<std::string>());
    increment(s["dispatches"], v.at("reason").get<std::string>());
    if (j.at("states").is_null())
        increment(s["dispatches"], "without_saved_witness");
}
inline void record_mass_search_receipt(J &m, const J &j, const J &r) {
    if (!j.contains("search_allocation") && !j.contains("expansion_allocation"))
        return;
    auto &s = m.at("mass_search_allocation");
    std::string role = j.contains("search_allocation")
                           ? j.at("search_allocation").at("reason").get<std::string>()
                           : "expansion";
    increment(s["receipts"], role);
    s["cpu_seconds"][role] = number(s["cpu_seconds"], role.c_str()) +
                              number(r, "cpu_seconds") + number(r, "cpp_thread_cpu_seconds");
}
inline J mass_search_status(const J &m) {
    if (!m.contains("mass_search_allocation"))
        return J{{"enabled", false}, {"policy", "PRESENT separate joint-pattern/corridor search unchanged"}};
    auto result = m.at("mass_search_allocation");
    result["enabled"] = true;
    result["scope"] = "new committed allocations since this revision; historical total searches retained separately";
    result["expansion_basis"] = "top four certified endpoint unions including positive unfinished exact partial sums";
    result["replica_cycle"] = J::array({1, 2, 4});
    result["factory_calls_advance_counters"] = false;
    return result;
}
}
