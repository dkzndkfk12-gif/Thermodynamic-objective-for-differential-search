#pragma once
#include "options.hpp"
namespace thermo {
inline bool is_binary_seed(const J &job) {
    return job.at("kind") == "seed" && job.value("seed_kind", std::string()) == "binary";
}
inline void initialize_binary_seeds(J &m, double elapsed) {
    if (!m.contains("midori_binary_seeds"))
        m["midori_binary_seeds"] = {{"schema", "MIDORI64_BINARY_STARTS_V1"},
            {"started_elapsed_seconds", elapsed}, {"submitted", 0}, {"receipts", 0},
            {"completed", 0}, {"incomplete", 0}, {"errors", 0}, {"interrupted", 0},
            {"cpu_seconds", 0.}, {"new_patterns", 0}, {"new_families", 0}, {"duplicates", 0},
            {"next_endpoint_turn", 0}, {"returned_by_active", J::object()},
            {"new_patterns_by_active", J::object()}};
    const auto &s = m.at("midori_binary_seeds");
    require(s.at("schema") == "MIDORI64_BINARY_STARTS_V1" &&
        count(s, "receipts") == count(s, "completed") + count(s, "incomplete") + count(s, "errors") &&
        count(s, "receipts") + count(s, "interrupted") <= count(s, "submitted"),
        "Inconsistent binary seed accounting");
}
inline void record_binary_seed_submission(J &m, const J &job) {
    if (!is_binary_seed(job)) return;
    auto &s = m.at("midori_binary_seeds");
    const auto turn = count(m.at("mass_search_allocation"), "endpoint_turn");
    require(count(job, "binary_seed_sequence") == count(s, "submitted") &&
        count(job, "binary_seed_endpoint_turn") == turn && turn >= count(s, "next_endpoint_turn"),
        "Stale binary seed submission");
    increment(s, "submitted");
    s["next_endpoint_turn"] = sat_add(turn, sat_mul(3, job.at("counts").size()));
}
inline void record_binary_seed_receipt(J &m, const J &job, const J &receipt,
    const J &new_patterns_by_active, std::uint64_t created, std::uint64_t duplicates) {
    if (!is_binary_seed(job)) return;
    auto &s = m.at("midori_binary_seeds");
    increment(s, "receipts");
    const auto status = receipt.value("status", std::string());
    increment(s, status == "COMPLETE" ? "completed" : status == "ERROR" ? "errors" : "incomplete");
    s["cpu_seconds"] = number(s, "cpu_seconds") + number(receipt, "cpu_seconds") + number(receipt, "cpp_thread_cpu_seconds");
    increment(s, "new_families", created); increment(s, "duplicates", duplicates);
    for (const auto &h : receipt.value("hits", J::array()))
        increment(s["returned_by_active"], str(h.at("pattern_record").at("active")));
    for (const auto &kv : new_patterns_by_active.items()) {
        increment(s["new_patterns_by_active"], kv.key(), kv.value().get<std::uint64_t>());
        increment(s, "new_patterns", kv.value().get<std::uint64_t>());
    }
}
inline void record_binary_seed_interruption(J &m, const J &job) {
    if (!is_binary_seed(job)) return;
    auto &s = m.at("midori_binary_seeds");
    increment(s, "interrupted");

    s["next_endpoint_turn"] = std::min(count(s, "next_endpoint_turn"),
        count(m.at("mass_search_allocation"), "endpoint_turn"));
}
}
