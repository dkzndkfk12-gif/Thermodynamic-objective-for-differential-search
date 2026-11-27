#pragma once
#include "campaign.hpp"
namespace present_native {
inline void Campaign::initialize() {
    J &co = def(m, "corridor",
                J{{"version", "collapsed_transfer_v3"},
                  {"jobs", 0},
                  {"certificates", J::object()},
                  {"diagnostics", J::object()},
                  {"history", J::array()},
                  {"discarded", 0}});
    require(has(J::array({"collapsed_transfer_v1", "collapsed_transfer_v2", "collapsed_transfer_v3"}),
                co["version"]),
            "Unsupported transfer ledger version");
    co["version"] = "collapsed_transfer_v3";
    def(co,"unfinished_rows",J::object());
    auto cc = config("corridor_search");
    if (co["history"].empty() || !equivalent(co["history"].back()["configuration"], cc))
        co["history"].push_back(J{{"seconds", elapsed()}, {"configuration", cc}});
    auto &ad = def(co, "adaptation",
                   J{{"version", "adaptive_activity_v2"},
                     {"finished", 0},
                     {"expansions", 0},
                     {"next_expand", 0},
                     {"domains", J::object()},
                     {"events", J::array()}});
    require(ad["version"] == "adaptive_activity_v1" || ad["version"] == "adaptive_activity_v2",
            "Unknown adaptive activity ledger version");
    if (ad["version"] != "adaptive_activity_v2")
        for (auto &d : ad["domains"]) {
            d["allocation_seconds"] = 0;
            d["novel_certificates"] = 0;
        }
    ad["version"] = "adaptive_activity_v2";
    for (int k : a["corridor_caps"]) {
        std::string name = std::to_string(iv(a, "corridor_input_active")) + ":" + std::to_string(k);
        auto &d = def(ad["domains"], name,
                      J{{"input_active", a["corridor_input_active"]},
                        {"cap", k},
                        {"initial", true},
                        {"submitted", 0},
                        {"finished", 0},
                        {"rows", 0},
                        {"positive_rows", 0},
                        {"seconds", 0.0},
                        {"best_F", nullptr},
                        {"improvements", 0},
                        {"failure_streak", 0},
                        {"retry_after", 0},
                        {"retry_wall", 0},
                        {"last_submitted", -1},
                        {"stops", J::object()}});
        d["initial"] = true;
    }
    ad["next_expand"] = std::max(iv(ad, "next_expand"), iv(ad, "finished") + iv(a, "corridor_adapt_every"));
    long long legacy_searches = 0;
    for (const auto &p : patterns)
        legacy_searches += iv(p, "searches");
    auto &sa = def(m, "search_allocation",
                   J{{"version", "balanced_pattern_search_v1"},
                     {"endpoint_turn", 0},
                     {"generation_turn", 0},
                     {"lane_turn", 0},
                     {"next_registration", 0},
                     {"group_cursors", J::object()},
                     {"dispatches", J::object()},
                     {"generation_by_active", J::object()},
                     {"started_at_seconds", elapsed()},
                     {"legacy_searches", legacy_searches}});
    require(sa["version"] == "balanced_pattern_search_v1", "Unknown search allocation state");
    std::vector<std::string> pids;
    for (auto it = patterns.begin(); it != patterns.end(); ++it)
        pids.push_back(it.key());
    std::sort(pids.begin(), pids.end());
    for (const auto &id : pids)
        register_pattern(patterns[id]);
    auto &js = def(m, "joint_search",
                   J{{"version", "present_adaptive_transfer_v4"},
                     {"turn", 0},
                     {"jobs", J::object()},
                     {"endpoint_jobs", J::object()},
                     {"diagnostics", J::object()},
                     {"phase_history", J::array()},
                     {"since_seconds", elapsed()}});
    require(has(J::array({"present_joint_patterns_v1", "present_blocked_joint_v2",
                          "present_collapsed_transfer_v3", "present_adaptive_transfer_v4"}),
                js["version"]),
            "Unknown PRESENT joint search version");
    js["version"] = "present_adaptive_transfer_v4";
    J conf = {{"active_cap", a["ensemble_max_active"]},
              {"source_hash", protocol["source_hash"]},
              {"ensemble_seconds", a["ensemble_seconds"]}};
    if (js["phase_history"].empty() || !equivalent(js["phase_history"].back()["configuration"], conf)) {
        js["phase_history"].push_back(J{{"seconds", elapsed()}, {"configuration", conf}});
        js["phase_diagnostics"] = J::object();
    }
    def(js, "phase_diagnostics", J::object());
    auto ec = config("evaluation_schedule");
    auto &ex =
        def(m, "exact_schedule", J{{"configuration", ec}, {"yields", 0}, {"phase_history", J::array()}});
    bool phase = ex["phase_history"].empty() || !equivalent(ex["phase_history"].back()["configuration"], ec);
    if (phase)
        ex["phase_history"].push_back(J{{"seconds", elapsed()}, {"configuration", ec}});
    ex["configuration"] = ec;
    m["focus"] = J::array();
    for (auto &c : cases) {
        if (phase || !c.contains("fair_phase_start_cpu")) {
            c["fair_phase_start_cpu"] = get(c, "exact_completed_job_cpu_seconds").is_null()
                                            ? J(0)
                                            : c["exact_completed_job_cpu_seconds"];
            dc.insert(c["id"]);
        }
        if (!c.contains("fair_slice_start_cpu")) {
            c["fair_slice_start_cpu"] = get(c, "exact_completed_job_cpu_seconds").is_null()
                                            ? J(0)
                                            : c["exact_completed_job_cpu_seconds"];
            dc.insert(c["id"]);
        }
    }
    auto &pm = def(m, "pair_transfer",
                   J{{"version", "independent_endpoint_transfer_v1"},
                     {"tasks", J::object()},
                     {"turn", 0},
                     {"history", J::array()},
                     {"completed", 0}});
    require(pm["version"] == "independent_endpoint_transfer_v1", "Unknown endpoint-conditioned exact ledger");
    auto pc = config("endpoint_exact");
    if (pm["history"].empty() || !equivalent(pm["history"].back()["configuration"], pc))
        pm["history"].push_back(J{{"seconds", elapsed()}, {"configuration", pc}});
    auto id = def(pm, "scratch_id", digest(J::array({out.string(), protocol["seed"], model.R})).substr(0, 24))
                  .get<std::string>();
    require(id.size() == 24 && id.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Invalid scratch identity");
    scratch = fs::temp_directory_path() / "present_exact_work" / id / digest(out.string()).substr(0, 12);
    fs::create_directories(scratch);
    for (auto it = pm["tasks"].begin(); it != pm["tasks"].end(); ++it) {
        require(it.key().size() == 64 && it.key().find_first_not_of("0123456789abcdef") == std::string::npos,
                "Invalid exact task identity");
        require(!fs::is_symlink(scratch / it.key()), "Exact work directory must not be a symlink");
        if (truth(get(it.value(), "done")))
            clean_pair(scratch / it.key());
    }
}
inline const Records &Campaign::groups() {
    if (!group_cache.is_null() && now() - group_clock < 2)
        return group_cache;
    Records gs;
    for (const auto &c : cases) {
        auto e = c["endpoint"].get<std::string>();
        auto &g = def(gs, e,
                      J{{"endpoint", e},
                        {"cases", J::array()},
                        {"eligible", J::array()},
                        {"F", INFINITY},
                        {"first", INFINITY}});
        g["cases"].push_back(c["id"]);
        g["first"] = std::min(num(g, "first"), num(c, "first_retained_seconds"));
        if (iv(c, "active") <= iv(a, "ensemble_max_active"))
            g["eligible"].push_back(c["id"]);
    }
    for (auto it = gs.begin(); it != gs.end(); ++it) {
        std::vector<double> fs;
        for (const auto &id : it.value()["cases"]) {
            const auto &c = cases.at(id.get<std::string>());
            if (covered(c))
                continue;
            double f;
            if (!get(get(c, "exact"), "F").is_null())
                f = num(c["exact"], "F");
            else {
                Z n = zi(get(c, "partial_numerator"));
                f = std::min(num(c, "weight"), n > 0 ? 3 * iv(c, "active") - thermo::log2z(n) : INFINITY);
            }
            fs.push_back(f);
        }
        if (m["corridor"]["certificates"].contains(it.key()))
            fs.push_back(num(m["corridor"]["certificates"][it.key()]["mass"], "F"));
        if (!fs.empty()) {
            double basef = *std::min_element(fs.begin(), fs.end());
            long double sum = 0;
            for (double f : fs)
                sum += std::exp2(basef - f);
            it.value()["F"] = basef - std::log2(double(sum));
        }
    }
    group_cache = std::move(gs);
    group_clock = now();
    return group_cache;
}
inline J Campaign::select_group(const J &pool, const std::string &role) {
    if (pool.empty())
        return nullptr;
    J ordered = pool;
    std::stable_sort(ordered.begin(), ordered.end(), [](const J &x, const J &y) {
        return std::make_tuple(num(x, "F"), num(x, "first"), x["endpoint"].get<std::string>()) <
               std::make_tuple(num(y, "F"), num(y, "first"), y["endpoint"].get<std::string>());
    });
    if (iv(m["joint_search"]["jobs"], role) % 3 != 2)
        ordered = slice(ordered, 32);
    auto score = [&](const J &g) {
        const auto &s = get(m["joint_search"]["endpoint_jobs"], g["endpoint"]);
        return std::make_tuple(iv(s, role), iv(s, role + "_last", -1), num(g, "first"),
                               g["endpoint"].get<std::string>());
    };
    return *std::min_element(ordered.begin(), ordered.end(),
                             [&](const J &x, const J &y) { return score(x) < score(y); });
}
inline J Campaign::breadth_job() {
    std::set<std::string> busy;
    for (auto *j : running("search"))
        busy.insert(j->at("pid"));
    J pool = J::array(), fresh = J::array();
    for (const auto &p : patterns)
        if (!busy.count(p["id"]) && (has(a["counts"], p["active"]) || truth(get(p, "states")))) {
            pool.push_back(p["id"]);
            if (!iv(p, "searches"))
                fresh.push_back(p["id"]);
        }
    if (pool.empty())
        return nullptr;
    bool first = !fresh.empty();
    if (first)
        pool = fresh;
    std::vector<std::string> labels;
    for (int n : a["counts"])
        labels.push_back(std::to_string(n));
    labels.push_back("outside_initial_counts");
    int cursor = iv(m["search_allocation"]["group_cursors"], "breadth");
    J group = J::array();
    std::string bucket;
    for (int off = 0; off < int(labels.size()); ++off) {
        int ix = (cursor + off) % labels.size();
        for (const auto &id : pool) {
            const auto &p = patterns.at(id.get<std::string>());
            auto b = has(a["counts"], p["active"]) ? str(p["active"]) : "outside_initial_counts";
            if (b == labels[ix])
                group.push_back(id);
        }
        if (!group.empty()) {
            cursor = (ix + 1) % labels.size();
            bucket = labels[ix];
            break;
        }
    }
    require(!group.empty(), "Empty endpoint-search group");
    auto score = [&](const J &id) {
        const auto &p = patterns.at(id.get<std::string>());
        return std::make_tuple(iv(p, "searches"), iv(p, "allocation_last_turn"), iv(p, "allocation_order"));
    };
    J id = *std::min_element(group.begin(), group.end(),
                             [&](const J &x, const J &y) { return score(x) < score(y); });
    const auto &p = patterns.at(id.get<std::string>());
    return J{{"kind", "search"},
             {"pid", id},
             {"pattern", p["pattern"]},
             {"states", p["states"]},
             {"copies", 1},
             {"seconds", std::min(60., num(a, "search_seconds"))},
             {"allocation", J{{"version", "balanced_pattern_search_v1"},
                              {"role", "breadth"},
                              {"requested_copies", 1},
                              {"bucket", bucket},
                              {"next_cursor", cursor},
                              {"first_pattern_search", !iv(p, "searches")},
                              {"reason", first ? "first_pattern_search" : "breadth_revisit"}}}};
}
inline double known_F(const J &c) {
    if (!get(get(c, "exact"), "F").is_null())
        return num(c["exact"], "F");
    Z n = zi(get(c, "partial_numerator"));
    return std::min(num(c, "weight"), n > 0 ? 3 * iv(c, "active") - thermo::log2z(n) : INFINITY);
}
inline J Campaign::ensemble_job() {
    std::map<std::string, int> busy;
    for (auto *j : running("ensemble"))
        ++busy[j->at("endpoint")];
    J pool = J::array();
    for (const auto &g : groups())
        if (!g["eligible"].empty() && busy[g["endpoint"]] < 2)
            pool.push_back(g);
    J selected = select_group(pool, "ensemble");
    if (selected.is_null())
        return nullptr;
    auto ids = selected["eligible"];
    std::stable_sort(ids.begin(), ids.end(), [&](const J &x, const J &y) {
        const auto &c = cases.at(x.get<std::string>());
        const auto &d = cases.at(y.get<std::string>());
        return std::make_tuple(known_F(c), num(c, "first_retained_seconds"), x.get<std::string>()) <
               std::make_tuple(known_F(d), num(d, "first_retained_seconds"), y.get<std::string>());
    });
    J chosen = slice(ids, 8);
    if (ids.size() > chosen.size()) {
        int rest = int(ids.size() - chosen.size()),
            count = iv(get(m["joint_search"]["endpoint_jobs"], selected["endpoint"]), "ensemble"),
            offset = 8 * count % rest;
        int take = std::min(rest, 16 - int(chosen.size()));
        for (int n = 0; n < take; ++n)
            chosen.push_back(ids[8 + (offset + n) % rest]);
    }
    J trails = J::array();
    for (const auto &id : chosen)
        trails.push_back(cases.at(id.get<std::string>())["states"]);
    int copies[] = {1, 2, 4};
    return J{{"kind", "ensemble"},
             {"endpoint", selected["endpoint"]},
             {"initial_trails", trails},
             {"copies", copies[iv(m["joint_search"]["jobs"], "ensemble") % 3]},
             {"active", a["ensemble_max_active"]},
             {"seconds", a["ensemble_seconds"]},
             {"keep", std::min(128LL, std::max(64LL, iv(a, "keep_per_job")))},
             {"allocation", J{{"version", "present_adaptive_transfer_v4"},
                              {"role", "ensemble"},
                              {"fixed_activity_pattern", false}}}};
}
inline J Campaign::expansion_job() {
    std::set<std::string> busy;
    for (auto *j : running("expand"))
        busy.insert(j->at("endpoint"));
    J pool = J::array();
    for (const auto &g : groups())
        if (!busy.count(g["endpoint"]))
            pool.push_back(g);
    J s = select_group(pool, "expand");
    if (s.is_null())
        return nullptr;
    int count = iv(get(m["joint_search"]["endpoint_jobs"], s["endpoint"]), "expand");
    auto ids = s["cases"];
    std::stable_sort(ids.begin(), ids.end(), [&](const J &x, const J &y) {
        const auto &c = cases.at(x.get<std::string>());
        const auto &d = cases.at(y.get<std::string>());
        return std::make_pair(known_F(c), num(c, "first_retained_seconds")) <
               std::make_pair(known_F(d), num(d, "first_retained_seconds"));
    });
    const auto &c = cases.at(ids[count % std::min<std::size_t>(ids.size(), 32)].get<std::string>());
    int centers[] = {0, 4, 8, 16, 24, 32};
    return J{{"kind", "expand"},
             {"endpoint", s["endpoint"]},
             {"cid", c["id"]},
             {"states", c["states"]},
             {"center", centers[count % 6]},
             {"seconds", std::min(60., num(a, "expansion_seconds"))}};
}
inline J Campaign::search_job() {
    long long turn = iv(m["joint_search"], "turn");
    if (!a["corridor_caps"].empty() && turn % 5 != 4) {
        J j = corridor_job();
        if (!j.is_null())
            return j;
    }
    bool seed = false;
    for (const auto &g : groups())
        if (!g["eligible"].empty())
            seed = true;
    int limit = std::max(1LL, std::min(2LL, iv(r, "search_slots") / 8));
    if ((!seed || turn % 24 == 23) && running("seed").size() < std::size_t(limit))
        return J{{"kind", "seed"},
                 {"active", a["ensemble_max_active"]},
                 {"seconds", std::min(30., num(a, "pattern_seconds"))}};
    int untried = 0;
    for (const auto &p : patterns)
        untried += !iv(p, "searches");
    if ((!seed || turn % 8 == 2) && untried < std::max(32LL, 4 * iv(r, "search_slots")) &&
        patterns.size() < std::size_t(iv(a, "max_patterns")) && running("pattern").empty())
        return J{{"kind", "pattern"},
                 {"active", a["counts"][iv(m["search_allocation"], "generation_turn") % a["counts"].size()]},
                 {"seconds", a["pattern_seconds"]},
                 {"keep", 8}};
    const char *roles[] = {"ensemble", "ensemble", "breadth",  "ensemble",
                           "expand",   "ensemble", "ensemble", "ensemble"};
    for (std::string role : {roles[turn % 8], "ensemble", "breadth"}) {
        J j = role == "ensemble" ? ensemble_job() : role == "breadth" ? breadth_job() : expansion_job();
        if (!j.is_null())
            return j;
    }
    return corridor_job();
}
inline J Campaign::evaluation_order(J pending) {
    if (pending.empty())
        return pending;
    std::set<std::string> es;
    for (const auto &id : pending)
        es.insert(cases.at(id.get<std::string>())["endpoint"]);
    const auto &gs = groups();
    J pool = J::array();
    for (const auto &g : gs)
        if (es.count(g["endpoint"]))
            pool.push_back(g);
    J selected = select_group(pool, "evaluation");
    std::string primary = selected.is_null() ? "" : selected["endpoint"].get<std::string>();
    auto score = [&](const J &id) {
        const auto &c = cases.at(id.get<std::string>());
        auto e = c["endpoint"].get<std::string>();
        return std::make_tuple(e != primary, gs.contains(e) ? num(gs[e], "F") : num(c, "weight"),
                               static_cast<long long>(num(c, "exact_completed_job_cpu_seconds") /
                                                      num(a, "exact_slice_cpu_seconds")),
                               std::log2(1 + num(get(c, "plan"), "operation_score")), priority(c),
                               num(c, "first_retained_seconds"));
    };
    std::stable_sort(pending.begin(), pending.end(),
                     [&](const J &x, const J &y) { return score(x) < score(y); });
    return pending;
}
inline bool Campaign::eligible(J &c, double t) {
    if (!get(c, "exact").is_null() || covered(c))
        return false;
    def(c, "fair_slice_start_cpu", num(c, "exact_completed_job_cpu_seconds"));
    if (num(c, "exact_completed_job_cpu_seconds") - num(c, "fair_slice_start_cpu") >=
        num(a, "exact_slice_cpu_seconds")) {
        c["fair_slice_start_cpu"] = num(c, "exact_completed_job_cpu_seconds");
        c["fair_resume_at"] = t + num(a, "exact_cooldown_seconds");
        inci(m["exact_schedule"], "yields");
        dc.insert(c["id"]);
    }
    return std::max(num(c, "next_exact_at"), num(c, "fair_resume_at")) <= t;
}
inline J Campaign::exact_job() {
    J pending = J::array();
    double t = elapsed();
    for (auto &c : cases)
        if (eligible(c, t))
            pending.push_back(c["id"]);
    if (pending.empty())
        return nullptr;
    inci(m, "evaluation_counter");
    long long tick = iv(m, "evaluation_counter");
    std::map<std::string, int> bycase, bye;
    for (const auto &task : active)
        if (has(J::array({"plan", "screen", "exact"}), task.job["kind"]) && truth(get(task.job, "cid"))) {
            auto cid = task.job["cid"].get<std::string>();
            if (cases.contains(cid)) {
                ++bycase[cid];
                ++bye[cases[cid]["endpoint"]];
            }
        }
    J admissible = J::array();
    for (const auto &id : pending)
        if (bye[cases.at(id.get<std::string>())["endpoint"]] < iv(a, "exact_endpoint_slots"))
            admissible.push_back(id);
    if (admissible.empty())
        return nullptr;
    J ranked = evaluation_order(admissible), ready = J::array(), unplanned = J::array();
    for (const auto &id : ranked) {
        const auto &c = cases.at(id.get<std::string>());
        if (truth(get(c, "plan")) && truth(get(c, "frontier")) &&
            bycase[id.get<std::string>()] < iv(a, "exact_family_slots"))
            ready.push_back(id);
        if (!truth(get(c, "plan")) && !bycase[id.get<std::string>()] && iv(c, "plan_attempts") < 2)
            unplanned.push_back(id);
    }
    auto plan = [&]() -> J {
        if (unplanned.empty() ||
            running("plan").size() >= std::size_t(std::max(1LL, iv(r, "exact_slots") / 3)))
            return nullptr;
        auto &c = cases[unplanned[0].get<std::string>()];
        inci(c, "plan_attempts");
        dc.insert(c["id"]);
        return J{{"kind", "plan"},
                 {"cid", c["id"]},
                 {"states", c["states"]},
                 {"table_bytes", r["table_bytes"]},
                 {"address_bytes", r["address_bytes"]},
                 {"leaf_operations", a["leaf_operations"]},
                 {"max_frontier", a["chunk_frontier"]},
                 {"seconds", a["plan_seconds"]}};
    };
    if (ready.empty() || tick % 4 == 1) {
        J j = plan();
        if (!j.is_null())
            return j;
    }
    if (tick % 16 == 0 && running("screen").size() < std::size_t(std::max(1LL, iv(r, "exact_slots") / 8)))
        for (const auto &id : slice(ranked, 64)) {
            auto &c = cases[id.get<std::string>()];
            if (iv(c, "screen_attempts") < 2 && !bycase[id.get<std::string>()] &&
                num(get(c, "screen"), "ess") < 8) {
                inci(c, "screen_attempts");
                dc.insert(c["id"]);
                return J{{"kind", "screen"},
                         {"cid", c["id"]},
                         {"pattern", patterns[c["pid"].get<std::string>()]["pattern"]},
                         {"states", c["states"]},
                         {"paths", a["screen_paths"]},
                         {"steps", a["screen_steps"]},
                         {"seconds", a["screen_seconds"]}};
            }
        }
    std::stable_sort(ready.begin(), ready.end(), [&](const J &x, const J &y) {
        return bycase[x.get<std::string>()] < bycase[y.get<std::string>()];
    });
    for (const auto &id : ready) {
        auto &c = cases[id.get<std::string>()];
        std::set<std::string> busy;
        for (auto *j : running("exact", id))
            busy.insert(j->at("box_id"));
        if (c["frontier"].size() >= std::size_t(iv(a, "max_pending_boxes"))) {
            c["last_resource_reason"] = "FRONTIER_STORAGE_LIMIT";
            c["next_exact_at"] = num(a, "wall_hours") * 3600;
            dc.insert(c["id"]);
            continue;
        }
        std::string best;
        std::pair<int, int> bestscore = {-1, 0};
        for (auto it = c["frontier"].begin(); it != c["frontier"].end(); ++it)
            if (!busy.count(it.key())) {
                int bits = 0;
                for (const auto &p : it.value()) {
                    Z z(p[1].get<std::string>(), 16);
                    bits += mpz_popcount(z.get_mpz_t());
                }
                auto score = std::make_pair(int(it.value().size()), -bits);
                if (score > bestscore) {
                    best = it.key();
                    bestscore = score;
                }
            }
        if (best.empty())
            continue;
        long long attempts = iv(c["chunk_attempts"], best);
        c["chunk_attempts"][best] = attempts + 1;
        dc.insert(c["id"]);
        return J{{"kind", "exact"},
                 {"cid", c["id"]},
                 {"states", c["states"]},
                 {"box_id", best},
                 {"box", c["frontier"][best]},
                 {"table_bytes", r["table_bytes"]},
                 {"address_bytes", r["address_bytes"]},
                 {"leaf_operations",
                  std::max(1024LL, iv(a, "leaf_operations") / (1LL << std::min(12LL, attempts)))},
                 {"max_frontier", std::max(2LL, std::min(iv(a, "chunk_frontier"),
                                                         iv(a, "max_pending_boxes") -
                                                             static_cast<long long>(c["frontier"].size())))},
                 {"seconds", a["exact_seconds"]}};
    }
    return plan();
}
inline void Campaign::committed(const J &j) {
    std::string kind = j["kind"];
    auto &sa = m["search_allocation"];
    if (has(J::array({"pattern", "search", "expand", "seed"}), kind))
        inci(sa, "lane_turn");
    if (kind == "pattern") {
        inci(sa, "generation_turn");
        inci(sa["generation_by_active"], str(j["active"]));
    }
    if (kind == "search") {
        auto pid = j["pid"].get<std::string>();
        auto &p = patterns[pid];
        std::string copies = str(j["copies"]);
        inci(p["allocation_searches"], copies);
        p["allocation_last_turn"] = sa["endpoint_turn"];
        if (!p.contains("replica_searches")) {
            p["legacy_unclassified_searches"] = get(p, "searches").is_null() ? J(0) : p["searches"];
            p["replica_searches"] = {{"1", 0}, {"2", 0}, {"4", 0}};
        }
        inci(p["replica_searches"], copies);
        inci(p, "searches");
        dp.insert(pid);
        inci(sa, "endpoint_turn");
        const auto &info = j["allocation"];
        sa["group_cursors"][info["role"].get<std::string>()] = info["next_cursor"];
        inci(sa["dispatches"], info["role"]);
        inci(sa["dispatches"], info["reason"]);
        if (!truth(get(j, "states")))
            inci(sa["dispatches"], "without_saved_witness");
    }
    auto &js = m["joint_search"];
    std::string role = has(J::array({"plan", "exact", "screen"}), kind) ? "evaluation" : kind;
    inci(js["jobs"], role);
    J e = get(j, "endpoint");
    if (e.is_null() && truth(get(j, "cid")) && cases.contains(j["cid"]))
        e = cases[j["cid"].get<std::string>()]["endpoint"];
    if (!e.is_null()) {
        auto &s = def(js["endpoint_jobs"], e, J::object());
        inci(s, role);
        long long n = 0;
        for (const auto &v : js["jobs"])
            n += v.get<long long>();
        s[role + "_last"] = n;
    }
    if (has(J::array({"ensemble", "search", "expand", "seed", "pattern", "corridor"}), kind))
        inci(js, "turn");
    if (kind == "corridor") {
        auto key = std::to_string(iv(j, "input_active")) + ":" + std::to_string(iv(j, "cap"));
        auto &d = m["corridor"]["adaptation"]["domains"].at(key);
        inci(d, "submitted");
        d["last_submitted"] = m["corridor"]["jobs"];
        inci(m["corridor"], "jobs");
    }
    if (kind == "pair") {
        auto &pm = m["pair_transfer"];
        inci(pm, "turn");
        auto &task = def(pm["tasks"], j["pair_id"],
                         J{{"endpoint", j["endpoint"]},
                           {"cap", j["cap"]},
                           {"jobs", 0},
                           {"cpu_seconds", 0},
                           {"done", false},
                           {"first", elapsed()},
                           {"stalls", 0},
                           {"progress", nullptr}});
        inci(task, "jobs");
        task["last"] = pm["turn"];
        task["work"] = j["work"];
    }
}
}
