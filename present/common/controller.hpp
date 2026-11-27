#pragma once
#include "transfer.hpp"
namespace present_native {
inline bool Campaign::submit(Pool &pool, J job) {
    if (used() + iv(job, "cpu_cores", 1) > iv(r, "workers"))
        return false;
    double deadline = num(a, "wall_hours") * 3600, remaining = deadline - elapsed();
    if (remaining <= 0)
        return false;
    inci(m, "counter");
    std::string hash = digest(J::array({protocol["seed"], protocol["schema"], m["counter"], job["kind"]}));
    job["seed"] = std::stoull(hash.substr(0, 16), nullptr, 16);
    job["started"] = elapsed();
    job["submitted_monotonic"] = now();
    job["job_index"] = m["counter"];
    def(job, "keep", a["keep_per_job"]);
    if (job["kind"] == "exact") {
        auto &c = cases.at(job["cid"].get<std::string>());
        if (get(c, "exact_first_submitted_seconds").is_null()) {
            c["exact_first_submitted_seconds"] = job["started"];
            dc.insert(c["id"]);
        }
    }
    job["seconds"] = std::min(num(job, "seconds"), remaining);
    job["deadline_monotonic"] = started + deadline - base;
    auto f = pool.submit([this, job] { return present_native::execute(model, root, job); });
    active.push_back(Active{job, std::move(f)});
    committed(job);
    return true;
}
inline void Campaign::consume(const J &job, const J &result) {
    std::string kind = job["kind"];
    inci(m["completed"], kind);
    double cpu = num(result, "cpu_seconds") + num(result, "python_thread_cpu_seconds");
    inc(m["cpu"], kind, cpu);
    const auto &d = get(result, "diagnostics");
    if (kind == "ensemble" || kind == "search" || kind == "expand")
        for (auto field : {"diagnostics", "phase_diagnostics"}) {
            auto &target = def(m["joint_search"][field], kind, J::object());
            inci(target, "completed_jobs");
            if (kind == "ensemble") {
                for (auto k : {"sweeps",
                               "hard_observations",
                               "soft_observations",
                               "hard_move_attempts",
                               "hard_moves_changed",
                               "hard_pattern_changes",
                               "hard_endpoint_changes",
                               "hard_pattern_transitions",
                               "hard_endpoint_transitions",
                               "recombination_attempts",
                               "recombinations",
                               "selected_families",
                               "block_attempts",
                               "block_moves",
                               "block_pattern_changes",
                               "boundary_block_attempts",
                               "boundary_block_moves",
                               "boundary_block_pattern_changes",
                               "boundary_block_endpoint_changes",
                               "collective_attempts",
                               "collective_moves",
                               "collective_pattern_changes"})
                    inci(target, k, iv(d, k));
                const auto &pairs = get(d, "swaps");
                long long attempts = 0, accepts = 0;
                for (const auto &pair : pairs) {
                    attempts += pair[0].get<long long>();
                    accepts += pair[1].get<long long>();
                }
                inci(target, "swap_attempts", attempts);
                inci(target, "swap_accepts", accepts);
                if (!pairs.empty()) {
                    int index = iv(d, "target_lane", pairs.size()) - 1;
                    require(index >= 0 && index < int(pairs.size()), "Invalid replica swap receipt");
                    inci(target, "hard_link_attempts", pairs[index][0]);
                    inci(target, "hard_link_accepts", pairs[index][1]);
                }
            } else if (kind == "search") {
                inci(target, "hard_lane_jobs", truth(get(d, "hard_lane_activated")));
                inci(target, "observations", iv(d, "observations"));
            } else
                for (auto k : {"legal_observations", "swaps", "swap_attempts"})
                    inci(target, k, iv(d, k));
        }
    if (result["status"] == "ERROR") {
        m["errors"].push_back(J{{"kind", kind}, {"cid", get(job, "cid")}, {"reason", get(result, "reason")}});
        tail(m["errors"], 12);
        throw std::runtime_error(str(get(result, "reason")));
    }
    if (kind == "pair")
        pair_consume(job, result);
    else if (kind == "corridor") {
        auto &diag = m["corridor"]["diagnostics"];
        for (auto k : {"completed_rows", "proved_zero_rows", "proposals", "accepted_inputs", "cache_hits",
                       "swap_attempts", "swaps"})
            inci(diag, k, iv(d, k));
        for (auto k : {"peak_nodes", "peak_edges"})
            diag[k] = std::max(iv(diag, k), iv(d, k));
        std::string reason = get(d, "stop").is_null()
                                 ? get(result, "reason").is_null() ? "UNKNOWN" : str(result["reason"])
                                 : str(d["stop"]);
        inci(def(diag, "job_stops", J::object()), reason);
        auto &by = def(def(diag, "by_cap", J::object()), str(job["cap"]),
                       J{{"jobs", 0}, {"completed_rows", 0}, {"stops", J::object()}});
        inci(by, "jobs");
        inci(by, "completed_rows", iv(d, "completed_rows"));
        inci(by["stops"], reason);
        int improves = merge_records(job, result.value("corridor_records", J::array()), result);
        corridor_completed(job, result, improves);
    } else if (kind == "pattern") {
        for (const auto &p : result.value("patterns", J::array()))
            add_pattern(p);
    } else if (kind == "search" || kind == "expand" || kind == "seed" || kind == "ensemble") {
        J origin = {{"kind", kind},
                    {"seed", job["seed"]},
                    {"job_index", job["job_index"]},
                    {"replicas", get(job, "copies")},
                    {"pattern_id", get(job, "pid")},
                    {"allocation", get(job, "allocation")},
                    {"warm_started", truth(get(job, "states")) || truth(get(job, "initial_trails"))},
                    {"starting_endpoint", get(job, "endpoint")},
                    {"active_cap", kind == "ensemble" ? get(job, "active") : J()},
                    {"generated_legal_chain", kind == "seed"}};
        for (const auto &hit : result.value("hits", J::array()))
            add_witness(hit["states"], origin, num(job, "started") + num(hit, "first_seconds"));
        inci(m, "archive_discarded_entries", iv(d, "archive_discarded_entries"));
    } else {
        auto cid = job["cid"].get<std::string>();
        auto &c = cases.at(cid);
        dc.insert(cid);
        if (kind == "exact") {
            inc(c, "exact_completed_job_cpu_seconds", cpu);
            inc(c, "exact_sum_job_wall_seconds", num(result, "job_wall_seconds"));
            c["exact_peak_rss_kib"] = std::max(iv(c, "exact_peak_rss_kib"), iv(result, "peak_rss_kib"));
        }
        if (kind == "screen") {
            J current = select_fields(
                  result, {"status", "F", "ess", "positive", "reason", "paths", "max_weight_fraction"}),
              old = get(c, "screen");
            if (!old.is_null() && get(old, "status") == "COMPLETE" && get(current, "status") == "COMPLETE") {
                double n0 = num(old, "paths", num(a, "screen_paths")),
                       n1 = num(current, "paths", num(a, "screen_paths")), top = INFINITY;
                if (!get(old, "F").is_null())
                    top = std::min(top, num(old, "F"));
                if (!get(current, "F").is_null())
                    top = std::min(top, num(current, "F"));
                if (!std::isfinite(top))
                    top = 0;
                double a0 = get(old, "F").is_null() ? 0 : n0 * std::exp2(top - num(old, "F")),
                       a1 = get(current, "F").is_null() ? 0 : n1 * std::exp2(top - num(current, "F"));
                if (a0 + a1 > 0) {
                    current["F"] = top - std::log2((a0 + a1) / (n0 + n1));
                    current["ess"] = (a0 + a1) * (a0 + a1) /
                                     (a0 * a0 / std::max(num(old, "ess", 1), 1e-300) +
                                      a1 * a1 / std::max(num(current, "ess", 1), 1e-300));
                    current["max_weight_fraction"] = std::max(a0 * num(old, "max_weight_fraction", 1),
                                                              a1 * num(current, "max_weight_fraction", 1)) /
                                                     (a0 + a1);
                }
                current["paths"] = n0 + n1;
                current["positive"] = iv(old, "positive") + iv(current, "positive");
            }
            if (get(current, "status") == "COMPLETE" || old.is_null())
                c["screen"] = current;
        } else if (kind == "plan") {
            if (result["status"] == "PLAN") {
                c["plan"] =
                    select_fields(result, {"operation_score", "estimated_table_bytes", "initial_volume",
                                           "cell_bytes", "branch", "components", "fits_leaf_budget"});
                c["root_volume"] = result["initial_volume"];
                c["frontier"] = J::object();
                c["frontier"][digest(J::array())] = J::array();
                c["last_resource_reason"] = nullptr;
                speculative.erase(cid);
                const auto &proof = get(result, "unit_exact");
                if (!proof.is_null()) {
                    require(c["root_volume"] == "1" && zi(proof["numerator"]) == zi(c["witness_numerator"]),
                            "Invalid singleton certificate");
                    c["partial_numerator"] = proof["numerator"];
                    c["closed_volume"] = "1";
                    c["frontier"] = J::object();
                    c["solved_nodes"] = 1;
                    c["zero_nodes"] = 0;
                    c["last_chunk_reason"] = "SINGLE_ASSIGNMENT";
                    c["exact"] = dyadic(zi(c["partial_numerator"]), 3 * iv(c, "active"));
                    c["exact_completed_seconds"] = elapsed();
                    c["exact_first_submitted_seconds"] = job["started"];
                    inc(c, "exact_completed_job_cpu_seconds", cpu);
                    inc(c, "exact_sum_job_wall_seconds", num(result, "job_wall_seconds"));
                    c["plan"]["single_assignment_proof"] = true;
                    inci(m, "singleton_certifications");
                }
            } else
                c["last_resource_reason"] =
                    get(result, "reason").is_null() ? J("PLAN_INCOMPLETE") : result["reason"];
        } else if (result["status"] == "EXACT_PROGRESS" || result["status"] == "EXACT_DONE") {
            auto key = job["box_id"].get<std::string>();
            require(get(c["frontier"], key) == job["box"], "Duplicate/stale adaptive result");
            c["frontier"].erase(key);
            for (const auto &box : result["frontier"]) {
                auto child = digest(box);
                require(!c["frontier"].contains(child), "Duplicate adaptive box");
                c["frontier"][child] = box;
            }
            c["partial_numerator"] = Z(zi(c["partial_numerator"]) + zi(result["numerator"])).get_str();
            c["closed_volume"] = Z(zi(c["closed_volume"]) + zi(result["closed_volume"])).get_str();
            inci(c, "solved_nodes", iv(result, "solved_nodes"));
            inci(c, "zero_nodes", iv(result, "zero_nodes"));
            inci(c, "cache_hits", iv(result, "component_cache_hits"));
            c["last_chunk_reason"] = get(result, "reason");
            c["last_resource_reason"] =
                get(result, "reason") == "LEAF_MEMORY_LIMIT" ? get(result, "reason") : J();
            std::vector<std::string> stale;
            for (auto it = c["chunk_attempts"].begin(); it != c["chunk_attempts"].end(); ++it)
                if (!c["frontier"].contains(it.key()))
                    stale.push_back(it.key());
            for (const auto &x : stale)
                c["chunk_attempts"].erase(x);
            if (c["frontier"].empty()) {
                require(c["closed_volume"] == c["root_volume"], "Family partition not closed");
                require(zi(c["partial_numerator"]) >= zi(c["witness_numerator"]),
                        "Exact family omitted its witness");
                c["exact"] = dyadic(zi(c["partial_numerator"]), 3 * iv(c, "active"));
                c["exact_completed_seconds"] = elapsed();
            }
        } else {
            c["last_resource_reason"] =
                get(result, "reason").is_null() ? J("EXACT_INCOMPLETE") : result["reason"];
            c["next_exact_at"] = elapsed() + 1;
        }
    }
}
inline int Campaign::execute() {
    flush();
    publish();
    double last_save = now(), last_report = now();
    Pool pool(iv(r, "workers"));
    try {
        for (;;) {
            for (auto it = active.begin(); it != active.end();) {
                if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto job = it->job;
                    auto result = it->future.get();
                    it = active.erase(it);
                    consume(job, result);
                } else
                    ++it;
            }
            if (stopping || fs::exists(out / "STOP")) {
                stop_reason = "USER_STOP";
                break;
            }
            if (elapsed() >= num(a, "wall_hours") * 3600) {
                stop_reason = "TIME_BUDGET";
                break;
            }
            int neval = eval_used();
            while (neval < iv(r, "exact_slots") && used() < iv(r, "workers")) {
                J j = pair_job(std::min<int>(iv(r, "workers") - used(), iv(r, "exact_slots") - eval_used()));
                if (j.is_null())
                    j = exact_job();
                if (j.is_null() || !submit(pool, j))
                    break;
                neval += iv(j, "cpu_cores", 1);
            }
            int ns = used() - neval;
            while (used() < iv(r, "workers") &&
                   (ns < iv(r, "search_slots") || used() < iv(r, "workers") - 1)) {
                J j = search_job();
                if (j.is_null())
                    break;
                if (ns >= iv(r, "search_slots")) {
                    j["seconds"] = std::min(10., num(j, "seconds"));
                    j["borrowed_evaluation_slot"] = true;
                }
                if (!submit(pool, j))
                    break;
                ++ns;
            }
            if (now() - last_save >= 3) {
                flush();
                last_save = now();
            }
            if (now() - last_report >= 20) {
                publish();
                last_report = now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (const std::exception &e) {
        stop_reason = "ERROR: " + std::string(e.what());
    }
    stop_all();
    pool.join();
    for (auto &task : active)
        try {
            J result = task.future.get();
            if (result["status"] != "INCOMPLETE" || task.job["kind"] == "pair")
                consume(task.job, result);
        } catch (const std::exception &e) {
            stop_reason = "ERROR: " + std::string(e.what());
            m["errors"].push_back(J{{"kind", task.job["kind"]}, {"reason", e.what()}});
            tail(m["errors"], 12);
        }
    active.clear();
    flush(true);
    publish(true);
    return stop_reason.rfind("ERROR:", 0) == 0 ? 1 : 0;
}
}
