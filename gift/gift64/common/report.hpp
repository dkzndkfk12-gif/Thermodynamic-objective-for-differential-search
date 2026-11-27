#pragma once
#include "present_policy.hpp"
namespace thermo {
inline std::string probability(const J &mass) {
    if (mass.is_null() || !mass.is_object() || !mass.contains("F") || mass["F"].is_null()) return "pending";
    std::ostringstream text;
    text << "2^-" << std::fixed << std::setprecision(9) << mass.at("F").get<double>();
    return text.str();
}
inline void Campaign::publish(bool terminal) {
    const double publish_started=now();
    refresh_unions(true);
    refresh_ranking();
    J best = partial_cache.empty() ? J() : partial_cache.front();
    J status{{"schema","THERMO_CPP_STATUS_V1"},{"protocol",protocol},
        {"status",terminal ? "FINISHED" : stage},{"reason",reason},{"elapsed_seconds",elapsed()},
        {"best_certified_lower_bound",best},{"certified_lower_bound_best",best},
        {"errors",m["errors"]},{"session_error",m.value("session_error",J())}};
    auto export_rows = [&](const std::vector<J> &rows) {
        J output = J::array();
        for (auto g : rows) {
            g["components"] = J::array();
            for (auto &id : g["component_ids"]) {
                auto &c = cases.at(id.get<std::string>());
                g["components"].push_back(
                    J{{"id", id},
                      {"pattern", patterns.at(c["pid"])["pattern"]},
                      {"states", c["states"]},
                      {"representative_mass", raw_mass(c)},
                      {"closed_subproblem_mass", dyadic(integer(c["partial_numerator"]),exponent(c))},
                      {"symmetry", c["symmetry"]},
                      {"bounds", bounds(c)},
                      {"complete", !c["exact"].is_null()},
                      {"witness_numerator", c["witness_numerator"]},
                      {"denominator_power", exponent(c)},
                      {"screen", c["screen"]},
                      {"mass_work",c.value("mass_work",J())},
                      {"common_value_seed", c.value("common_value_seed", J())},
                      {"origins", c["origins"]},
                      {"first_retained_seconds", c["first_retained_seconds"]},
                      {"exact_completed_seconds", c.value("exact_completed_seconds", J())},
                      {"exact_cpu_seconds", c["exact_cpu_seconds"]},
                      {"exact_job_wall_seconds", c["exact_job_wall_seconds"]},
                      {"exact_peak_rss_kib", c["exact_peak_rss_kib"]},
                      {"solved_nodes", c["solved_nodes"]}});
            }
            output.push_back(g);
            if (output.size() == 10)
                break;
        }
        return output;
    };
    auto done = export_rows(complete_cache), progress = export_rows(partial_cache);
    J results{
        {"schema", "THERMO_CPP_RESULTS_V1"},
        {"protocol", protocol},
        
        {"status", status},
        {"exact_unions", done},
        {"certified_partial_unions", progress},
        {"common_value_seed_best_separate_certificate", common_seed_summary(m).is_object()
            ? common_seed_summary(m).at("best") : J()},
        {"scope", "Exact DDT-product sum of computed disjoint trail regions at the stated endpoints; a lower "
                  "bound for the unrestricted differential"},
        {"keys",
         "independent-round-key Markov/DDT model; does not assert this value for each fixed master key"},
        {"archive_exhaustive", false}};
    save_json(a.out / "STATUS.json", status);
    save_json(a.out / "RESULTS.json", results);
    std::ostringstream text;
    text << "elapsed=" << std::fixed << std::setprecision(1) << elapsed()/60 << " min | best="
         << probability(best.is_null() ? J() : best.at("mass"));
    if (!best.is_null()) text << " | " << best.at("input").get<std::string>() << " -> " << best.at("output").get<std::string>();
    if (terminal) text << " | " << reason;
    text << '\n';
    write_atomic(a.out / "REPORT_KO.md",text.str());
    std::cout << text.str() << std::flush;
    if (had_error && m.value("session_error",J()).is_object())
        std::cerr << "ERROR: " << m.at("session_error").at("reason").get<std::string>() << std::endl;
    m["last_report_wall_seconds"]=now()-publish_started;
    last_publish=now();
}
}
