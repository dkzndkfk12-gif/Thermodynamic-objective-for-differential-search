#pragma once
#include "worker.hpp"
namespace present_native {
struct Active {
    J job;
    std::future<J> future;
};
class Campaign {
  public:
    fs::path root, out, scratch;
    J cfg, a, r, protocol;
    Records m{J()}, patterns, cases;
    Model model;
    RunLock lock;
    Database db;
    std::set<std::string> dp, dc, speculative, deleted_cases;
    std::list<Active> active;
    double started = now(), base = 0, group_clock = -INFINITY;
    Records group_cache;
    J old_start_pool;
    std::string stop_reason = "RUNNING";
    Campaign(const fs::path &variant, const fs::path &output, const J &options, const J &resources = nullptr);
#ifdef PRESENT_TEST_CLOCK
    double frozen_elapsed = 0;
    double elapsed() const { return frozen_elapsed; }
#else
    double elapsed() const { return base + now() - started; }
#endif
    J config(const std::string &k) const { return cfg["protocol_template"][k]; }
    void flush(bool closed = false);
    std::vector<J *> running(const std::string &kind, const std::string &key = "") {
        std::vector<J *> v;
        for (auto &task : active)
            if (task.job["kind"] == kind &&
                (key.empty() || task.job.value("cid", task.job.value("pid", std::string())) == key))
                v.push_back(&task.job);
        return v;
    }
    int used() const {
        int n = 0;
        for (const auto &x : active)
            n += iv(x.job, "cpu_cores", 1);
        return n;
    }
    int eval_used() const {
        int n = 0;
        for (const auto &x : active)
            if (has(J::array({"pair", "exact", "plan", "screen"}), x.job["kind"]))
                n += iv(x.job, "cpu_cores", 1);
        return n;
    }
    static bool unstarted(const J &c) {
        return !truth(get(c, "plan")) && get(c, "exact").is_null() && zi(get(c, "partial_numerator")) == 0 &&
               !truth(get(c, "frontier")) && !truth(get(c, "solved_nodes")) &&
               get(c, "exact_first_submitted_seconds").is_null();
    }
    double priority(const J &c) const {
        const auto &s = get(c, "screen");
        return get(s, "F").is_null()
                   ? num(c, "weight")
                   : num(s, "F") + std::log2(std::max(1., num(s, "paths", num(a, "screen_paths")) /
                                                              std::max(1., num(s, "ess", 1))));
    }
    void register_pattern(J &p);
    J *add_pattern(const J &p, bool overflow = false);
    void add_witness(const J &states, const J &origin, double when);
    bool covered(const J &c) const;
    J unions(bool partial = false) const;
    const Records &groups();
    J select_group(const J &pool, const std::string &role);
    J breadth_job();
    J ensemble_job();
    J expansion_job();
    J search_job();
    void initialize();
    void committed(const J &j);
    void consume(const J &j, const J &result);
    bool submit(Pool &pool, J job);
    void publish(bool terminal = false);
    int execute();
    void expand_domains();
    J select_domain();
    J corridor_job();
    void corridor_completed(const J &j, const J &result, int improvements);
    int merge_records(const J &j, const J &records, const J &result,
                      const std::string &origin = "collapsed_transfer");
    int pair_threads() const;
    Records pair_candidates();
    J pair_job(int available);
    void pair_consume(const J &j, const J &result);
    void clean_pair(const fs::path &path);
    J evaluation_order(J pending);
    bool eligible(J &c, double t);
    J exact_job();
};
inline Campaign::Campaign(const fs::path &variant, const fs::path &output, const J &options,
                          const J &resources)
    : root(fs::canonical(variant)), out(fs::absolute(output).lexically_normal()),
      cfg(read_json(root / "config.json")), a(cfg["defaults"]), model(cfg["rounds"]),
      lock((fs::create_directories(out), out / "run.lock")), db(out / "state.sqlite3") {
    a.update(options);
    started -= num(options, "build_seconds");
    r = resources.is_null() ? layout(a) : resources;
    for (auto table : {"patterns", "cases"})
        for (auto &[id, value] : db.load(table))
            (std::string(table) == "patterns" ? patterns : cases)[id] = value;
    auto meta = db.load("meta");
    for (auto &[id, value] : meta)
        if (id == "state")
            m = value;
    if (m.is_null())
        m = J{{"elapsed", 0},
              {"counter", 0},
              {"cpu", J::object()},
              {"completed", J::object()},
              {"expansions", J::object()},
              {"admitted", 0},
              {"dropped", 0},
              {"invalid_patterns", 0},
              {"first_certified", nullptr},
              {"closed", true},
              {"imports", J::array()},
              {"errors", J::array()}};
    base = num(m, "elapsed");
    if (!truth(get(m, "closed")))
        m["timing_has_unclean_session"] = true;
    J old = fs::exists(out / "PROTOCOL.json") ? read_json(out / "PROTOCOL.json") : J();
    if (!old.is_null() && truth(get(old, "joint_patterns_search")))
        a["ensemble_max_active"] = old["joint_patterns_search"]["active_cap"];
    require(iv(a, "ensemble_max_active") >= model.R && iv(a, "ensemble_max_active") <= 16 * model.R,
            "Invalid saved joint-search active cap");
    if (get(a, "seed").is_null()) {
        if (!old.is_null())
            a["seed"] = old["seed"];
        else {
            std::ifstream f("/dev/urandom", std::ios::binary);
            std::uint64_t seed;
            require(bool(f.read(reinterpret_cast<char *>(&seed), sizeof(seed))), "Cannot read random seed");
            a["seed"] = seed;
        }
    }
    J hashes = J::object();
    std::vector<fs::path> files;
    for (const auto &dir : {root.parent_path() / "common", root / "src"})
        for (const auto &e : fs::recursive_directory_iterator(dir))
            if (e.is_regular_file())
                files.push_back(e.path());
    files.push_back(root / "config.json");
    files.push_back(root / "run.sh");
    files.push_back(root.parent_path() / "build.sh");
    std::sort(files.begin(), files.end());
    for (const auto &p : files)
        hashes[fs::relative(p, root.parent_path()).generic_string()] = sha256(read_text(p));
    protocol = cfg["protocol_template"];
    protocol["seed"] = a["seed"];
    protocol["source_hash"] = digest(hashes);
    protocol["implementation"] = "native_cpp_present_v1";
    protocol["joint_patterns_search"]["active_cap"] = a["ensemble_max_active"];
    protocol["seed_activity_cap"] = a["ensemble_max_active"];
    if (!old.is_null()) {
        require(get(old, "rounds") == model.R && get(old, "model") == protocol["model"] &&
                    get(old, "seed") == a["seed"],
                "Saved model or seed differs");
        bool same = get(old, "source_hash") == protocol["source_hash"],
             legacy = get(old, "source_hash") == cfg["legacy_source_hash"];
        require(same || legacy, "Source or model changed; use a new output directory");
        if (legacy) {
            def(m, "implementation_history", J::array())
                .push_back(J{{"previous_source_hash", old["source_hash"]},
                             {"source_hash", protocol["source_hash"]},
                             {"implementation", "native_cpp_present_v1"},
                             {"seconds", elapsed()}});
        }
    }
    save_json(out / "PROTOCOL.json", protocol);
    m["closed"] = false;
    m["resources"] = r;
    def(m, "builds", J::array()).push_back(J{{"compiler", __VERSION__},
                                           {"cpp_standard", __cplusplus},
                                           {"source_hash", protocol["source_hash"]},
                                           {"wall_seconds", num(options, "build_seconds")}});
    def(m, "focus", J::array());
    def(m, "reuse_runs", J::array());
    def(m, "evaluation_counter", 0);
    for (auto &c : cases) {
        if (unstarted(c))
            speculative.insert(c["id"]);
        if (get(c, "last_resource_reason") == "FRONTIER_STORAGE_LIMIT" &&
            get(c, "frontier").size() < std::size_t(iv(a, "max_pending_boxes"))) {
            c["next_exact_at"] = 0;
            c["last_resource_reason"] = nullptr;
            dc.insert(c["id"]);
        }
    }
    initialize();
}
inline void Campaign::flush(bool closed) {
    db.exec("BEGIN IMMEDIATE");
    try {
        for (const auto &id : deleted_cases)
            db.erase("cases", id);
        for (const auto &id : dp)
            if (patterns.contains(id))
                db.put("patterns", id, patterns[id]);
        for (const auto &id : dc)
            if (cases.contains(id))
                db.put("cases", id, cases[id]);
        m["elapsed"] = elapsed();
        m["closed"] = closed;
        m["last_heartbeat_unix"] =
            std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        db.put("meta", "state", m);
        db.exec("COMMIT");
        dp.clear();
        dc.clear();
        deleted_cases.clear();
    } catch (...) {
        try {
            db.exec("ROLLBACK");
        } catch (...) {
        }
        throw;
    }
}
inline void Campaign::register_pattern(J &p) {
    if (p.contains("allocation_order"))
        return;
    auto &s = m["search_allocation"];
    p["allocation_order"] = s["next_registration"];
    inci(s, "next_registration");
    p["allocation_searches"] = {{"1", 0}, {"2", 0}, {"4", 0}};
    p["allocation_last_turn"] = -1;
    dp.insert(p["id"]);
}
inline J *Campaign::add_pattern(const J &p, bool overflow) {
    auto id = digest(p);
    if (patterns.contains(id))
        return &patterns[id];
    if (patterns.size() >= std::size_t(iv(a, "max_patterns")) && !overflow)
        return nullptr;
    J cat;
    try {
        cat = model.catalog(J::array({p}));
    } catch (const std::runtime_error &) {
        inci(m, "invalid_patterns");
        return nullptr;
    }
    auto mm = cat["patterns"][0];
    patterns[id] = J{{"id", id},      {"pattern", p},     {"active", mm["active"]}, {"dimension", mm["d"]},
                     {"searches", 0}, {"states", nullptr}};
    dp.insert(id);
    register_pattern(patterns[id]);
    return &patterns[id];
}
inline void Campaign::add_witness(const J &states, const J &origin, double when) {
    J p = model.pattern(states), cat = model.catalog(J::array({p})), w = model.encode_trail(cat, states);
    auto endpoint =
             w["endpoints"]["input"].get<std::string>() + "_" + w["endpoints"]["output"].get<std::string>(),
         cid = digest(J::array({endpoint, p}));
    if (!cases.contains(cid) && speculative.size() >= std::size_t(iv(a, "max_candidates"))) {
        inci(m, "candidate_admission_attempts");
        std::set<std::string> busy;
        for (const auto &task : active)
            if (truth(get(task.job, "cid")))
                busy.insert(task.job["cid"]);
        std::map<std::string, std::vector<std::string>> gs;
        std::vector<std::string> ordered(speculative.begin(), speculative.end());
        std::stable_sort(ordered.begin(), ordered.end(), [&](const auto &x, const auto &y) {
            return num(cases[x], "first_retained_seconds") < num(cases[y], "first_retained_seconds");
        });
        for (const auto &id : ordered)
            gs[cases[id]["endpoint"]].push_back(id);
        std::vector<std::string> endpoints;
        for (const auto &[e, ids] : gs)
            endpoints.push_back(e);
        std::sort(endpoints.begin(), endpoints.end(), [&](auto &x, auto &y) {
            return gs[x].size() != gs[y].size() ? gs[x].size() > gs[y].size() : x < y;
        });
        std::string victim;
        for (const auto &e : endpoints) {
            int visited = 0;
            for (const auto &id : gs[e]) {
                if (visited++ >= 64)
                    break;
                const auto &c = cases[id];
                if (unstarted(c) && !busy.count(id) &&
                    (victim.empty() || std::make_pair(priority(c), -num(c, "first_retained_seconds")) >
                                           std::make_pair(priority(cases[victim]),
                                                          -num(cases[victim], "first_retained_seconds"))))
                    victim = id;
            }
            if (!victim.empty())
                break;
        }
        if (victim.empty()) {
            inci(m, "dropped");
            return;
        }
        speculative.erase(victim);
        cases.erase(victim);
        dc.erase(victim);
        deleted_cases.insert(victim);
        inci(m, "archive_discarded_entries");
        inci(m, "candidate_queue_evictions");
    }
    J *pp = add_pattern(p, true);
    require(pp, "Legal witness pattern failed compilation");
    auto pid = pp->at("id").get<std::string>();
    if (!truth(get(*pp, "states"))) {
        (*pp)["states"] = states;
        dp.insert(pid);
    }
    if (!cases.contains(cid)) {
        cases[cid] = J{{"id", cid},
                       {"pid", pid},
                       {"endpoint", endpoint},
                       {"input", w["endpoints"]["input"]},
                       {"output", w["endpoints"]["output"]},
                       {"states", states},
                       {"weight", w["witness_weight"]},
                       {"witness_numerator", w["witness_numerator"]},
                       {"active", (*pp)["active"]},
                       {"first_retained_seconds", when},
                       {"origins", J::array({origin})},
                       {"screen", nullptr},
                       {"screen_attempts", 0},
                       {"plan", nullptr},
                       {"frontier", J::object()},
                       {"partial_numerator", "0"},
                       {"closed_volume", "0"},
                       {"root_volume", nullptr},
                       {"chunk_attempts", J::object()},
                       {"solved_nodes", 0},
                       {"zero_nodes", 0},
                       {"cache_hits", 0},
                       {"exact", nullptr},
                       {"next_exact_at", 0},
                       {"exact_completed_job_cpu_seconds", 0},
                       {"exact_sum_job_wall_seconds", 0},
                       {"fair_slice_start_cpu", 0},
                       {"fair_phase_start_cpu", 0},
                       {"exact_peak_rss_kib", 0},
                       {"exact_first_submitted_seconds", nullptr}};
        inci(m, "admitted");
        speculative.insert(cid);
    } else {
        auto &c = cases[cid];
        if (!has(c["origins"], origin) && c["origins"].size() < 8)
            c["origins"].push_back(origin);
        c["first_retained_seconds"] = std::min(num(c, "first_retained_seconds"), when);
        if (zi(w["witness_numerator"]) > zi(c["witness_numerator"])) {
            c["states"] = states;
            c["weight"] = w["witness_weight"];
            c["witness_numerator"] = w["witness_numerator"];
        }
    }
    dc.insert(cid);
}
inline bool Campaign::covered(const J &c) const {
    const auto &certs = m["corridor"]["certificates"];
    auto e = c["endpoint"].get<std::string>();
    if (!certs.contains(e))
        return false;
    int cap = certs[e]["cap"];
    auto pid = c["pid"].get<std::string>();
    const auto &p = patterns.at(pid)["pattern"];
    for (std::size_t i = 1; i < p.size(); ++i)
        if (std::count(p[i].get_ref<const std::string &>().begin(), p[i].get_ref<const std::string &>().end(),
                       '1') > cap)
            return false;
    return true;
}
inline J Campaign::unions(bool partial) const {
    const auto &certs = m["corridor"]["certificates"];
    Records groups, inside;
    for (auto it = certs.begin(); it != certs.end(); ++it)
        groups[it.key()] = J::array();
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto &c : cases) {
        auto e = c["endpoint"].get<std::string>(), pid = c["pid"].get<std::string>();
        require(seen.insert({e, pid}).second, "Duplicate activity mass");
        J mass = c["exact"];
        if (mass.is_null() && partial)
            mass = dyadic(zi(get(c, "partial_numerator")), 3 * iv(c, "active"));
        if (mass.is_null() || zi(mass["numerator"]) == 0)
            continue;
        if (covered(c))
            def(inside, e, J::array()).push_back(mass);
        else
            def(groups, e, J::array()).push_back(J::array({c["id"], mass}));
    }
    J rows = J::array();
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        auto e = it.key();
        J parts = J::array(), ids = J::array();
        int complete = 0, unfinished = 0;
        for (const auto &item : it.value()) {
            ids.push_back(item[0]);
            parts.push_back(item[1]);
            if (cases.at(item[0].get<std::string>())["exact"].is_null())
                ++unfinished;
            else
                ++complete;
        }
        if (certs.contains(e)) {
            if (inside.contains(e))
                require(probability(sum_mass(inside[e])) <= probability(certs[e]["mass"]),
                        "Covered pattern sum exceeds complete corridor mass");
            parts.push_back(certs[e]["mass"]);
        }
        if (parts.empty())
            continue;
        std::sort(ids.begin(), ids.end());
        auto ep = split(e, '_');
        J row = {{"endpoint", e},
                 {"input", ep[0]},
                 {"output", ep[1]},
                 {"mass", sum_mass(parts)},
                 {"component_ids", ids},
                 {"patterns", it.value().size()},
                 {"complete_patterns", complete},
                 {"unfinished_patterns", unfinished}};
        if (certs.contains(e)) {
            row["corridor"] = certs[e];
            row["pattern_count_scope"] = "explicit additional patterns OUTSIDE corridor; implicit corridor "
                                         "pattern count not enumerated";
        }
        rows.push_back(row);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const J &x, const J &y) { return probability(x["mass"]) > probability(y["mass"]); });
    return rows;
}
inline void Campaign::publish(bool terminal) {
    J complete = unions(), partial = unions(true),
      best = !partial.empty()    ? partial[0]
             : !complete.empty() ? complete[0]
                                 : J();
    J status = {{"status", terminal ? "FINISHED" : "SEARCH_AND_EXACT"},
                {"reason", stop_reason},
                {"elapsed_seconds", elapsed()},
                {"best", best},
                {"partial_lower_bound_including_unfinished_patterns", best},
                {"errors", m["errors"]}};
    save_json(out / "STATUS.json", status);
    J exported = J::array();
    for (const auto &u : slice(complete, 10)) {
        J item = u;
        item["components"] = J::array();
        for (const auto &id : u["component_ids"]) {
            const auto &c = cases.at(id.get<std::string>());
            J component =
                select_fields(c, {"id", "states", "active", "screen", "origins", "first_retained_seconds",
                                  "exact_completed_seconds", "exact_first_submitted_seconds",
                                  "exact_completed_job_cpu_seconds", "exact_sum_job_wall_seconds",
                                  "exact_peak_rss_kib", "solved_nodes"});
            component["pattern"] = patterns.at(c["pid"].get<std::string>())["pattern"];
            component["mass"] = c["exact"];
            component["component_cache_hits"] = c["cache_hits"];
            item["components"].push_back(component);
        }
        int n = 0;
        for (const auto &c : cases)
            n += c["endpoint"] == u["endpoint"] && c["exact"].is_null();
        item["discovered_but_unfinished_patterns"] = n;
        exported.push_back(item);
    }
    J rs = status;
    rs["partial_lower_bound_including_unfinished_patterns"] = partial.empty() ? J() : partial[0];
    save_json(out / "RESULTS.json",
              J{{"protocol", protocol},
                {"status", rs},
                {"exact_unions", exported},
                {"certified_partial_unions", slice(partial, 3)},
                {"scope", "Nested corridors: largest exact cap only, plus disjoint OUTSIDE patterns/partial "
                          "boxes. Certified DDT full-hull lower bound."},
                {"search_archive_is_exhaustive", false},
                {"prior_imports", m["imports"]},
                {"omitted_candidates_due_to_ledger_cap", m["dropped"]},
                {"archive_discarded_entries",
                 get(m, "archive_discarded_entries").is_null() ? J(0) : m["archive_discarded_entries"]}});
    std::ostringstream text;
    text << "elapsed=" << std::fixed << std::setprecision(1) << elapsed() / 60 << " min | best=";
    if (best.is_null())
        text << "pending";
    else
        text << "2^-" << std::setprecision(9) << num(best["mass"], "F") << " | "
             << best["input"].get<std::string>() << " -> " << best["output"].get<std::string>();
    if (terminal)
        text << " | " << stop_reason;
    write_atomic(out / "REPORT_KO.md", text.str() + "\n");
    std::cout << text.str() << std::endl;
}
}
