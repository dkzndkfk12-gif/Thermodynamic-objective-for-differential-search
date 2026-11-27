#pragma once
#include "search.hpp"
namespace present_native {
inline J new_domain(int n, int k) {
    return J{{"input_active", n},   {"cap", k},
             {"initial", false},    {"submitted", 0},
             {"finished", 0},       {"rows", 0},
             {"positive_rows", 0},  {"seconds", 0.0},
             {"best_F", nullptr},   {"improvements", 0},
             {"failure_streak", 0}, {"retry_after", 0},
             {"retry_wall", 0},     {"last_submitted", -1},
             {"stops", J::object()}};
}
inline bool permitted(const J &a, const J &d) {
    int low = a["corridor_caps"].empty() ? 1 : a["corridor_caps"][0].get<int>();
    return iv(d, "input_active") >= iv(a, "corridor_input_active") &&
           iv(d, "input_active") <= std::min(iv(a, "corridor_max_input_active"), 4 * iv(d, "cap")) &&
           iv(d, "cap") >= low && iv(d, "cap") <= iv(a, "corridor_max_cap");
}
inline std::string domain_key(const J &d) {
    return std::to_string(iv(d, "input_active")) + ":" + std::to_string(iv(d, "cap"));
}
inline void Campaign::expand_domains() {
    auto &s = m["corridor"]["adaptation"];
    if (iv(s, "finished") < iv(s, "next_expand"))
        return;
    J candidates = J::object();
    for (const auto &d : s["domains"]) {
        if (!permitted(a, d) || !iv(d, "finished"))
            continue;
        int n = iv(d, "input_active"), k = iv(d, "cap");
        for (int axis = 0; axis < 2; ++axis) {
            J nd = new_domain(n + (axis == 0), k + (axis == 1));
            auto key = domain_key(nd);
            if (!s["domains"].contains(key) && permitted(a, nd))
                candidates[key] = J::array({nd, axis == 0 ? "input" : "internal"});
        }
    }
    if (candidates.empty())
        return;
    std::string preferred = iv(s, "expansions") % 2 == 0 ? "input" : "internal";
    int maxcap = 0;
    for (int k : a["corridor_caps"])
        maxcap = std::max(k, maxcap);
    auto order = [&](const J &item) {
        auto d = item[0];
        int u = iv(d, "input_active") - iv(a, "corridor_input_active"),
            v = std::max(0LL, iv(d, "cap") - maxcap);
        return std::make_tuple(std::max(u, v), item[1] != preferred, u + v,
                               iv(d, "input_active") + iv(d, "cap"));
    };
    J best = *std::min_element(candidates.begin(), candidates.end(),
                               [&](const J &x, const J &y) { return order(x) < order(y); });
    s["domains"][domain_key(best[0])] = best[0];
    inci(s, "expansions");
    s["next_expand"] = iv(s, "finished") + iv(a, "corridor_adapt_every");
    s["events"].push_back(J{{"seconds", elapsed()},
                            {"axis", best[1]},
                            {"input_active", best[0]["input_active"]},
                            {"cap", best[0]["cap"]},
                            {"after_jobs", s["finished"]}});
    tail(s["events"], 32);
}
inline J Campaign::select_domain() {
    expand_domains();
    auto &s = m["corridor"]["adaptation"];
    long long turn = iv(m["corridor"], "jobs");
    std::map<std::string, int> busy;
    int risky = 0;
    for (auto *j : running("corridor")) {
        ++busy[domain_key(*j)];
        risky += truth(get(*j, "adaptive_probe"));
    }
    int probe_slots =
        std::max(1, int(std::ceil(num(r, "search_slots") * num(a, "corridor_expansion_share"))));
    long long failed = 0;
    for (const auto &d : s["domains"])
        if (iv(d, "cap") > 3)
            failed += std::max(0LL, iv(d, "finished") - iv(d, "productive_jobs"));
    probe_slots = std::max(1, int(std::ceil(probe_slots / (1 + failed / 16.))));
    int slots = std::max(
        1, int(std::ceil(num(r, "search_slots") / std::max<std::size_t>(3, a["corridor_caps"].size()))));
    J pool = J::array();
    for (auto it = s["domains"].begin(); it != s["domains"].end(); ++it) {
        const auto &d = it.value();
        if (!permitted(a, d))
            continue;
        bool probe = (!truth(get(d, "initial")) || iv(d, "cap") > 3) &&
                     (!iv(d, "positive_rows") || iv(d, "failure_streak") > 0);
        bool cooling = iv(d, "retry_after") > iv(s, "finished") && num(d, "retry_wall") > elapsed();
        if (cooling || busy[it.key()] >= (probe ? 1 : slots) || (probe && risky >= probe_slots))
            continue;
        pool.push_back(J{{"name", it.key()}, {"d", d}, {"probe", probe}});
    }
    if (pool.empty())
        return nullptr;
    auto breadth = [&](const J &item) {
        const auto &d = item["d"];
        auto name = item["name"].get<std::string>();
        return std::make_tuple(iv(d, "submitted") + busy[name], iv(d, "last_submitted"), name);
    };
    J fresh = J::array();
    for (const auto &item : pool)
        if (!iv(item["d"], "submitted"))
            fresh.push_back(item);
    J selected;
    if (!fresh.empty())
        selected = *std::min_element(fresh.begin(), fresh.end(),
                                     [&](auto &x, auto &y) { return breadth(x) < breadth(y); });
    else if (turn % 4 == 0 || turn % 4 == 1) {
        J p = pool;
        if (turn % 4 == 0) {
            J wider = J::array();
            for (const auto &item : pool)
                if (!truth(get(item["d"], "initial")))
                    wider.push_back(item);
            if (!wider.empty())
                p = wider;
        }
        selected =
            *std::min_element(p.begin(), p.end(), [&](auto &x, auto &y) { return breadth(x) < breadth(y); });
    } else {
        double best = INFINITY;
        for (const auto &item : pool)
            if (!get(item["d"], "best_F").is_null())
                best = std::min(best, num(item["d"], "best_F"));
        auto utility = [&](const J &item) {
            const auto &d = item["d"];
            double quality = !std::isfinite(best) || get(d, "best_F").is_null()
                                 ? 1
                                 : std::exp2(std::max(-12., best - num(d, "best_F")));
            double throughput = (1 + num(d, "novel_certificates")) /
                                (num(a, "corridor_probe_seconds") + num(d, "allocation_seconds"));
            return std::make_pair(quality * throughput / (1 + busy[item["name"]]), -iv(d, "last_submitted"));
        };
        selected = *std::max_element(pool.begin(), pool.end(),
                                     [&](auto &x, auto &y) { return utility(x) < utility(y); });
    }
    const auto &d = selected["d"];
    bool probe = selected["probe"],
         force = iv(d, "input_active") > iv(a, "corridor_input_active") && iv(d, "submitted") % 2 == 0;
    double seconds = num(a, "corridor_seconds");
    if (probe)
        seconds = std::min(seconds,
                           num(a, "corridor_probe_seconds") * std::exp2(std::min(3LL, iv(d, "submitted"))));
    int copies[] = {1, 2, 4};
    return J{{"domain", selected["name"]},
             {"cap", d["cap"]},
             {"input_active", d["input_active"]},
             {"adaptive_probe", probe},
             {"seconds", seconds},
             {"start_active", force ? d["input_active"] : J(0)},
             {"domain_turn", d["submitted"]},
             {"copies", copies[iv(d, "submitted") % 3]}};
}
inline J Campaign::corridor_job() {
    if (a["corridor_caps"].empty())
        return nullptr;
    J selected = select_domain();
    if (selected.is_null())
        return nullptr;
    auto &co = m["corridor"];
    long long turn = iv(selected, "domain_turn");
    J start;
    bool warm = turn % 2 == 1 || (truth(get(selected, "adaptive_probe")) && turn == 0);
    if (warm && !iv(selected, "start_active")) {
        if (old_start_pool.is_null()) {
            J ids = J::array();
            for (auto it = cases.begin(); it != cases.end(); ++it)
                ids.push_back(it.key());
            std::stable_sort(ids.begin(), ids.end(), [&](const J &x, const J &y) {
                return std::make_pair(num(cases.at(x.get<std::string>()), "weight"),
                                      num(cases.at(x.get<std::string>()), "first_retained_seconds")) <
                       std::make_pair(num(cases.at(y.get<std::string>()), "weight"),
                                      num(cases.at(y.get<std::string>()), "first_retained_seconds"));
            });
            old_start_pool = J::array();
            for (const auto &id : ids) {
                const auto &x = cases.at(id.get<std::string>())["input"];
                if (activity(x) <= iv(a, "corridor_max_input_active") && !has(old_start_pool, x) &&
                    old_start_pool.size() < 128)
                    old_start_pool.push_back(x);
            }
        }
        J certs = J::array();
        for (const auto &c : co["certificates"])
            certs.push_back(c);
        std::stable_sort(certs.begin(), certs.end(),
                         [](const J &x, const J &y) { return num(x["mass"], "F") < num(y["mass"], "F"); });
        J pool = J::array();
        for (const auto &c : certs)
            if (!has(pool, c["input"]))
                pool.push_back(c["input"]);
        for (const auto &x : old_start_pool)
            if (!has(pool, x))
                pool.push_back(x);
        J eligible = J::array();
        for (const auto &x : pool) {
            auto key = std::to_string(iv(selected, "cap")) + ":" + x.get<std::string>();
            if (activity(x) <= std::min(iv(selected, "input_active"), 4 * iv(selected, "cap")) &&
                elapsed() - num(get(co, "unfinished_rows"), key, -1e100) > 600)
                eligible.push_back(x);
        }
        if (!eligible.empty())
            start = eligible[(turn / 2) % std::min<std::size_t>(128, eligible.size())];
    }
    selected.update(J{{"kind", "corridor"},
                      {"initial", start},
                      {"keep", 16},
                      {"max_nodes", r["corridor_effective_max_nodes"]},
                      {"max_edges", r["corridor_effective_max_edges"]},
                      {"address_bytes", r["address_bytes"]}});
    return selected;
}
inline int Campaign::merge_records(const J &job, const J &records, const J &result,
                                   const std::string &origin) {
    auto &co = m["corridor"];
    J pending = J::object();
    int improvements = 0;
    for (auto record : records) {
        auto key = record["input"].get<std::string>() + "_" + record["output"].get<std::string>();
        J old = get(co["certificates"], key);
        bool improves = old.is_null() || probability(record["mass"]) > probability(old["mass"]);
        if (!old.is_null()) {
            auto p0 = probability(old["mass"]), p1 = probability(record["mass"]);
            int k0 = old["cap"], k1 = record["cap"];
            require(!((k0 == k1 && p0 != p1) || (k0 < k1 && p0 > p1) || (k0 > k1 && p0 < p1)),
                    "Nested corridor exact sums disagree");
            if (k0 >= k1)
                continue;
            J absorbed = J::array();
            for (const auto &c : cases) {
                if (c["endpoint"] != key)
                    continue;
                int cap = 0;
                auto pp = patterns.at(c["pid"].get<std::string>())["pattern"];
                for (std::size_t r = 1; r < pp.size(); ++r)
                    cap = std::max(cap, int(std::count(pp[r].get_ref<const std::string &>().begin(),
                                                       pp[r].get_ref<const std::string &>().end(), '1')));
                if (k0 < cap && cap <= k1)
                    absorbed.push_back(c["exact"].is_null()
                                           ? dyadic(zi(get(c, "partial_numerator")), 3 * iv(c, "active"))
                                           : c["exact"]);
            }
            require(p1 >= p0 + probability(sum_mass(absorbed)),
                    "Expanded corridor omitted certified outside mass");
        }
        record["first_seconds"] = num(job, "started") + num(record, "first_seconds");
        record["completed_seconds"] = elapsed();
        record["origin"] = J{{"kind", origin},
                             {"job", job["job_index"]},
                             {"seed", job["seed"]},
                             {"copies", get(job, "copies")},
                             {"initial_input", get(job, "initial")},
                             {"input_activity_limit", get(job, "input_active")},
                             {"forced_initial_activity", iv(job, "start_active")},
                             {"adaptive_probe", truth(get(job, "adaptive_probe"))},
                             {"job_cpu_seconds", num(result, "cpu_seconds")}};
        if (!old.is_null())
            record["first_seconds"] = std::min(num(record, "first_seconds"), num(old, "first_seconds"));
        co["certificates"][key] = record;
        pending[key] = record;
        improvements += improves;
    }
    if (co["certificates"].size() > 512) {
        std::set<std::string> protect;
        for (const auto &row : slice(unions(true), 10))
            protect.insert(row["endpoint"]);
        std::vector<std::string> order;
        for (auto it = co["certificates"].begin(); it != co["certificates"].end(); ++it) {
            order.push_back(it.key());
            if (get(get(it.value(), "origin"), "kind") == "endpoint_conditioned_transfer")
                protect.insert(it.key());
        }
        std::stable_sort(order.begin(), order.end(), [&](const auto &x, const auto &y) {
            return probability(co["certificates"][x]["mass"]) > probability(co["certificates"][y]["mass"]);
        });
        for (std::size_t i = 512; i < order.size(); ++i)
            if (!protect.count(order[i])) {
                co["certificates"].erase(order[i]);
                inci(co, "discarded");
            }
    }
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (co["certificates"].contains(it.key()) && equivalent(co["certificates"][it.key()], it.value()))
            add_witness(it.value()["states"], it.value()["origin"], num(it.value(), "first_seconds"));
    }
    if (!pending.empty())
        group_cache = nullptr;
    return improvements;
}
inline void Campaign::corridor_completed(const J &j, const J &result, int improvements) {
    auto &co = m["corridor"];
    auto &s = co["adaptation"];
    inci(s, "finished");
    int n = iv(j, "input_active", iv(a, "corridor_input_active")), k = iv(j, "cap");
    auto &d = def(s["domains"], std::to_string(n) + ":" + std::to_string(k), new_domain(n, k));
    const auto &diag = get(result, "diagnostics");
    long long rows = iv(diag, "completed_rows"),
              positive = std::max(0LL, rows - iv(diag, "proved_zero_rows"));
    std::string reason =
        get(diag, "stop").is_null()
            ? get(result, "reason").is_null() ? "UNKNOWN" : result["reason"].get<std::string>()
            : diag["stop"].get<std::string>();
    inci(d, "finished");
    inci(d, "rows", rows);
    inci(d, "positive_rows", positive);
    inci(d, "productive_jobs", positive > 0);
    double cpu = std::max(0., num(result, "cpu_seconds", num(result, "job_wall_seconds", num(j, "seconds"))));
    inc(d, "seconds", cpu);
    inc(d, "allocation_seconds", cpu);
    inci(d, "improvements", improvements);
    inci(d, "novel_certificates", improvements);
    if (truth(get(diag, "unfinished_input")) && Z(diag["unfinished_input"].get<std::string>(), 16) != 0) {
        auto &fails = def(co, "unfinished_rows", J::object());
        fails[std::to_string(k) + ":" + diag["unfinished_input"].get<std::string>()] = elapsed();
        if (fails.size() > 512) {
            std::vector<std::string> keys;
            for (auto it = fails.begin(); it != fails.end(); ++it)
                keys.push_back(it.key());
            std::stable_sort(keys.begin(), keys.end(), [&](const std::string &x, const std::string &y) {
                return fails[x].get<double>() < fails[y].get<double>();
            });
            for (std::size_t i = 0; i < keys.size() - 512; ++i)
                fails.erase(keys[i]);
        }
    }
    inci(d["stops"], reason);
    for (const auto &rec : result.value("corridor_records", J::array()))
        if (!get(rec["mass"], "F").is_null())
            d["best_F"] = get(d, "best_F").is_null() ? rec["mass"]["F"]
                                                     : J(std::min(num(d, "best_F"), num(rec["mass"], "F")));
    if (!positive) {
        inci(d, "failure_streak");
        d["retry_after"] =
            iv(s, "finished") + std::min(256LL, 4LL * (1LL << std::min(6LL, iv(d, "failure_streak"))));
        d["retry_wall"] = elapsed() + std::min(120., num(a, "corridor_probe_seconds") *
                                                         std::exp2(std::min(3LL, iv(d, "failure_streak"))));
    } else {
        d["failure_streak"] = 0;
        d["retry_after"] = 0;
        d["retry_wall"] = 0;
    }
}
inline int Campaign::pair_threads() const {
    int reserve = std::max(1, int(num(r, "exact_slots") * num(a, "pair_share")));
    return std::min(
        iv(a, "pair_threads") ? int(iv(a, "pair_threads")) : std::min(8, std::max(1, reserve / 2)), reserve);
}
inline void Campaign::clean_pair(const fs::path &p) {
    require(p.parent_path() == scratch && !fs::is_symlink(p), "Invalid pair scratch directory");
    for (auto file :
         {"checkpoint.bin", "checkpoint.bin.tmp", "forward.bin", "forward.bin.tmp", "backward.bin",
          "backward.bin.tmp", "progress.json", "progress.json.tmp", "worker.lock"}) {
        std::error_code e;
        fs::remove(p / file, e);
    }
    std::error_code e;
    fs::remove(p, e);
}
inline Records Campaign::pair_candidates() {
    Records gs;
    for (const auto &c : cases) {
        auto pp = patterns.at(c["pid"].get<std::string>())["pattern"];
        int cap = 0;
        for (std::size_t r = 1; r < pp.size(); ++r)
            cap = std::max(cap, int(std::count(pp[r].get_ref<const std::string &>().begin(),
                                               pp[r].get_ref<const std::string &>().end(), '1')));
        if (cap > iv(a, "corridor_max_cap"))
            continue;
        std::string e = c["endpoint"];
        const auto &old = get(gs, e);
        if (old.is_null() ||
            std::make_tuple(cap, num(c, "weight"), c["id"].get<std::string>()) <
                std::make_tuple(int(iv(old, "witness_cap")), num(old, "weight"), str(old["cid"])))
            gs[e] = J{{"endpoint", e},  {"witness_cap", cap},    {"weight", c["weight"]},
                      {"cid", c["id"]}, {"states", c["states"]}, {"F", num(c, "weight")}};
    }
    for (auto it = m["corridor"]["certificates"].begin(); it != m["corridor"]["certificates"].end(); ++it) {
        const auto &rec = it.value();
        if (!gs.contains(it.key())) {
            int cap = 0;
            for (int r = 1; r < model.R; ++r)
                cap = std::max(cap, activity(rec["states"][r]));
            gs[it.key()] =
                J{{"endpoint", it.key()}, {"witness_cap", cap},      {"weight", rec["witness_weight"]},
                  {"cid", nullptr},       {"states", rec["states"]}, {"F", rec["mass"]["F"]}};
        }
        gs[it.key()]["F"] = std::min(num(gs[it.key()], "F"), num(rec["mass"], "F"));
    }
    for (const auto &row : unions(true)) {
        auto e = row["endpoint"].get<std::string>();
        if (gs.contains(e))
            gs[e]["F"] = row["mass"]["F"];
    }
    return gs;
}
inline J Campaign::pair_job(int available) {
    if (a["corridor_caps"].empty())
        return nullptr;
    int nt = pair_threads(), reserved = std::max(nt, int(num(r, "exact_slots") * num(a, "pair_share"))),
        busycores = 0;
    std::set<std::string> busy;
    for (auto *j : running("pair")) {
        busy.insert(j->at("endpoint"));
        busycores += iv(*j, "cpu_cores");
    }
    if (available < nt || busycores + nt > reserved)
        return nullptr;
    auto &pm = m["pair_transfer"];
    for (auto it = pm["tasks"].begin(); it != pm["tasks"].end(); ++it) {
        auto &task = it.value();
        const auto &record = get(m["corridor"]["certificates"], task["endpoint"]);
        if (!truth(get(task, "done")) && !busy.count(task["endpoint"]) && !record.is_null() &&
            iv(record, "cap") >= iv(task, "cap")) {
            task["done"] = true;
            task["superseded_by_larger_exact_cap"] = record["cap"];
            flush();
            clean_pair(scratch / it.key());
        }
    }
    auto gs = pair_candidates();
    std::vector<std::string> order;
    for (auto it = gs.begin(); it != gs.end(); ++it)
        order.push_back(it.key());
    std::sort(order.begin(), order.end(), [&](const auto &x, const auto &y) {
        return std::make_pair(num(gs[x], "F"), x) < std::make_pair(num(gs[y], "F"), y);
    });
    bool coverage = iv(pm, "turn") % 4 == 3;
    std::map<std::string, double> spent;
    for (const auto &task : pm["tasks"])
        spent[task["endpoint"]] += num(task, "cpu_seconds");
    if (coverage)
        std::stable_sort(order.begin(), order.end(), [&](const auto &x, const auto &y) {
            return std::make_tuple(spent[x], num(gs[x], "F"), x) <
                   std::make_tuple(spent[y], num(gs[y], "F"), y);
        });
    std::size_t limit =
        coverage ? iv(a, "pair_pending")
                 : std::min<long long>(iv(a, "pair_pending"), std::max<long long>(16, 2 * busy.size() + 4));
    if (order.size() > limit)
        order.resize(limit);
    J best;
    std::tuple<double, long long, double, std::string> scorebest;
    for (const auto &e : order) {
        if (busy.count(e))
            continue;
        auto g = gs[e];
        const auto &rec = get(m["corridor"]["certificates"], e);
        int low = a["corridor_caps"][0],
            cap = std::max(iv(g, "witness_cap"),
                           rec.is_null() ? static_cast<long long>(low) : iv(rec, "cap") + 1);
        if (cap > iv(a, "corridor_max_cap"))
            continue;
        std::string id = digest(J::array({model.R, e, cap, "independent_endpoint_transfer_v1"}));
        const auto &task = get(pm["tasks"], id);
        if (!task.is_null() && (truth(get(task, "done")) || num(task, "retry_at") > elapsed()))
            continue;
        auto score = std::make_tuple(coverage ? spent[e] : num(task, "cpu_seconds"), iv(task, "last", -1),
                                     num(g, "F"), e);
        if (best.is_null() || score < scorebest) {
            int bits = 3 * activity(g["states"][0]) + 3 * cap * (model.R - 1);
            std::uint64_t table = r["table_bytes"], address = r["address_bytes"];
            best = J{
                {"kind", "pair"},
                {"pair_id", id},
                {"endpoint", e},
                {"states", g["states"]},
                {"cap", cap},
                {"seconds", std::min(num(a, "pair_max_seconds"),
                                     num(a, "pair_seconds") * std::exp2(std::min(4LL, iv(task, "stalls"))))},
                {"cpu_cores", nt},
                {"threads", nt},
                {"address_bytes", address * nt},
                {"table_bytes", table * nt},
                {"max_nodes", std::max<std::uint64_t>(128, table * nt /
                                                               (bits < 256   ? 96
                                                                : bits < 512 ? 192
                                                                             : 384))},
                {"work", (scratch / id).string()}};
            scorebest = score;
        }
    }
    return best;
}
inline void Campaign::pair_consume(const J &j, const J &result) {
    auto &pm = m["pair_transfer"];
    auto &task = pm["tasks"].at(j["pair_id"].get<std::string>());
    inc(task, "cpu_seconds",
        std::max(0., num(result, "cpu_seconds")) + std::max(0., num(result, "python_thread_cpu_seconds")));
    const auto &d = get(result, "pair_progress");
    const auto &old = get(task, "progress");
    bool progress = iv(d, "generation") > iv(old, "generation") &&
                    (get(d, "phase") != get(old, "phase") || get(d, "round") != get(old, "round") ||
                     iv(d, "completed_sources") > iv(old, "completed_sources"));
    task["stalls"] = progress ? 0 : iv(task, "stalls") + 1;
    task["progress"] = d;
    task["last_reason"] = get(result, "reason").is_null()
                              ? get(d, "reason").is_null() ? J("UNKNOWN") : d["reason"]
                              : result["reason"];
    task["cpu_time_has_lower_bound_jobs"] =
        truth(get(task, "cpu_time_has_lower_bound_jobs")) || truth(get(result, "cpu_time_is_lower_bound"));
    task["retry_at"] =
        elapsed() +
        (iv(task, "stalls") ? std::min(120., 5 * std::exp2(std::min(4LL, iv(task, "stalls")))) : 1);
    if (truth(get(result, "corridor_records"))) {
        J mergejob = j;
        mergejob["input_active"] = activity(j["states"][0]);
        mergejob["copies"] = 1;
        mergejob["domain"] = nullptr;
        J records = result["corridor_records"];
        for (auto &record : records) {
            record["scope_cpu_seconds"] = task["cpu_seconds"];
            record["scope_cpu_time_has_lower_bound_jobs"] = task["cpu_time_has_lower_bound_jobs"];
            record["scope_first_submitted_seconds"] = task["first"];
        }
        merge_records(mergejob, records, result, "endpoint_conditioned_transfer");
        task["done"] = true;
        task["completed_seconds"] = elapsed();
        inci(pm, "completed");
        flush();
        clean_pair(scratch / j["pair_id"].get<std::string>());
    } else
        require(get(result, "status") != "PAIR_DONE",
                "Positive witnessed endpoint received no exact certificate");
}
}
