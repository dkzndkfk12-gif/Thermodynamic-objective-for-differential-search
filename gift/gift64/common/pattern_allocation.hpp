#pragma once
#include "options.hpp"
namespace thermo {
inline constexpr const char *pattern_allocation_schema = "PATTERN_GENERATION_ROUND_ROBIN_V1";

inline J empty_pattern_count_stats() {
    return J{{"submitted", 0}, {"completed", 0}, {"receipts", 0}, {"incomplete", 0},
             {"errors", 0}, {"interrupted_without_receipt", 0}, {"cpu_seconds", 0.},
             {"returned_patterns", 0}, {"new_unique_patterns", 0}};
}
inline J retained_pattern_counts(const std::map<std::string, J> &patterns) {
    J result = J::object();
    for (const auto &kv : patterns)
        increment(result, str(kv.second.at("active")));
    return result;
}
inline void initialize_pattern_allocation(J &m, const std::vector<int> &counts,
                                         const std::map<std::string, J> &patterns,
                                         const std::map<std::string, J> &jobs,
                                         double elapsed, bool resumed) {
    require(!counts.empty(), "Pattern generation needs at least one active-count level");
    if (!m.contains("pattern_generation_allocation")) {

        J rows = J::object();
        for (int n : counts)
            rows[std::to_string(n)] = empty_pattern_count_stats();
        std::uint64_t pending = 0;
        for (const auto &kv : jobs)
            pending += kv.second.at("kind") == "pattern";
        m["pattern_generation_allocation"] =
            J{{"schema", pattern_allocation_schema}, {"active_counts", counts},
              {"sequence", 0}, {"tracking_started_elapsed_seconds", elapsed},
              {"legacy_history_unavailable", resumed},
              {"legacy_completed_generator_jobs", count(m["completed"], "pattern")},
              {"legacy_generator_cpu_seconds", number(m["cpu"], "pattern")},
              {"baseline_retained_patterns_by_active", retained_pattern_counts(patterns)},
              {"legacy_pending_at_upgrade", pending}, {"legacy_receipts_after_upgrade", 0},
              {"legacy_cpu_seconds_after_upgrade", 0.},
              {"legacy_interrupted_after_upgrade", 0}, {"by_active", rows}};
    }
    const auto &allocation = m.at("pattern_generation_allocation");
    require(allocation.at("schema") == pattern_allocation_schema &&
                allocation.at("active_counts") == J(counts),
            "Pattern allocation schema or active-count scope changed");
    std::uint64_t submitted = 0;
    for (int n : counts) {
        const auto &r = allocation.at("by_active").at(std::to_string(n));
        submitted = sat_add(submitted, count(r, "submitted"));
        require(count(r, "receipts") == count(r, "completed") + count(r, "incomplete") + count(r, "errors") &&
                    count(r, "receipts") + count(r, "interrupted_without_receipt") <= count(r, "submitted"),
                "Inconsistent pattern generation completion counters");
    }
    require(submitted == count(allocation, "sequence"), "Pattern round-robin sequence differs from submissions");
}
inline int next_pattern_active(const J &m, const std::vector<int> &counts) {
    const auto &allocation = m.at("pattern_generation_allocation");
    return counts.at(count(allocation, "sequence") % counts.size());
}
inline void tag_pattern_submission(J &job, const J &m, const std::vector<int> &counts) {
    if (job.at("kind") != "pattern")
        return;
    require(job.at("active") == next_pattern_active(m, counts), "Stale pattern generation selection");
    job["pattern_generation_sequence"] = count(m.at("pattern_generation_allocation"), "sequence");
}
inline void record_pattern_submission(J &m, const J &job) {
    if (job.at("kind") != "pattern")
        return;
    auto &allocation = m.at("pattern_generation_allocation");
    require(job.at("pattern_generation_sequence") == allocation.at("sequence"),
            "Pattern submission sequence changed before commit");
    increment(allocation, "sequence");
    increment(allocation["by_active"].at(str(job.at("active"))), "submitted");
}
inline void record_pattern_receipt(J &m, const J &job, const J &receipt, std::uint64_t new_patterns) {
    if (job.at("kind") != "pattern")
        return;
    auto &allocation = m.at("pattern_generation_allocation");
    double cpu = number(receipt, "cpu_seconds") + number(receipt, "cpp_thread_cpu_seconds");
    if (!job.contains("pattern_generation_sequence")) {
        increment(allocation, "legacy_receipts_after_upgrade");
        allocation["legacy_cpu_seconds_after_upgrade"] = number(allocation, "legacy_cpu_seconds_after_upgrade") + cpu;
        return;
    }
    require(job.at("pattern_generation_sequence").get<std::uint64_t>() < count(allocation, "sequence"),
            "Uncommitted pattern generation receipt");
    auto &row = allocation["by_active"].at(str(job.at("active")));
    increment(row, "receipts");
    const auto status = receipt.value("status", std::string());
    increment(row, status == "COMPLETE" ? "completed" : status == "ERROR" ? "errors" : "incomplete");
    row["cpu_seconds"] = number(row, "cpu_seconds") + cpu;
    increment(row, "returned_patterns", receipt.value("patterns", J::array()).size());
    increment(row, "new_unique_patterns", new_patterns);
}
inline void record_pattern_interruption(J &m, const J &job) {
    if (job.at("kind") != "pattern")
        return;
    auto &allocation = m.at("pattern_generation_allocation");
    if (job.contains("pattern_generation_sequence"))
        increment(allocation["by_active"].at(str(job.at("active"))), "interrupted_without_receipt");
    else
        increment(allocation, "legacy_interrupted_after_upgrade");
}
inline J pattern_allocation_status(const J &m, const std::vector<int> &counts,
                                   const std::map<std::string, J> &patterns,
                                   const std::map<std::string, J> &jobs) {
    J result = m.at("pattern_generation_allocation"), rows = J::array(), pending = J::object();
    const auto retained = retained_pattern_counts(patterns);
    std::uint64_t legacy_pending = 0, lo = UINT64_MAX, hi = 0;
    for (const auto &kv : jobs) {
        const auto &j = kv.second;
        if (j.at("kind") == "pattern") {
            if (j.contains("pattern_generation_sequence"))
                increment(pending, str(j.at("active")));
            else
                ++legacy_pending;
        }
    }
    for (int n : counts) {
        auto key = std::to_string(n);
        J row = result.at("by_active").at(key);
        row["active"] = n;
        row["pending"] = count(pending, key.c_str());
        row["retained_unique_patterns_all_origins"] = count(retained, key.c_str());
        lo = std::min(lo, count(row, "submitted"));
        hi = std::max(hi, count(row, "submitted"));
        rows.push_back(row);
    }
    result.erase("by_active");
    result["rows"] = rows;
    result["next_active"] = next_pattern_active(m, counts);
    result["submitted_count_spread"] = hi - lo;
    result["legacy_pending"] = legacy_pending;
    result["retained_unique_patterns_by_active_all_origins"] = retained;
    result["scope"] = "pattern generator jobs only; excludes seed/corridor/endpoint search/AIS/exact/expansion";
    result["completed_definition"] = "valid COMPLETE receipts; empty/duplicate output still completes a job";
    result["cpu_definition"] = "receipted native process CPU plus C++ preparation/verification thread CPU; in-flight and lost CPU unavailable";
    result["new_patterns_definition"] = "previously unretained unique IDs first added by that target-count generator; duplicates excluded";
    result["fairness"] = "one turn per committed pattern submission; submission counts differ by at most one; completion/CPU/yield need not be equal";
    return result;
}
}
