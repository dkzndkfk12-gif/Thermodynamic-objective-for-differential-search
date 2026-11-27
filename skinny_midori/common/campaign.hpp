#pragma once
#include "options.hpp"
#include "pattern_allocation.hpp"
#include "search_allocation.hpp"
#include "adaptive_search.hpp"
#include "mass_priority.hpp"
#include "binary_seed_allocation.hpp"
#include "common_value_seed_allocation.hpp"
namespace thermo {
struct Running {
    J job;
    std::shared_ptr<TaskState> state;
    std::future<J> future;
};
class Campaign {
  public:
    Options a;
    Cipher cipher;
    std::uint64_t available_at_start, budget_at_start;
    unsigned cores, slots, evalslots, searchslots;
    double started = now(), base = 0, last_publish = -1, last_flush = -1;
    std::uint64_t seed = 0;
    Resources ram;
    Workers workers;
    Pool pool;
    RunLock lock;
    Database db;
    std::map<std::string, J> patterns, cases, jobs;
    std::map<std::string, std::set<std::string>> endpoint_cases;
    std::set<std::string> union_dirty_endpoints;
    std::map<std::string, J> partial_by_endpoint, complete_by_endpoint;
    std::set<std::string> dp, dc;
    std::map<std::uint64_t, Running> active;
    J m, protocol;
    fs::path scratch;
    std::vector<J> complete_cache, partial_cache;
    bool unions_dirty = true;
    double union_clock = -1;
    using RankKey = std::tuple<double, double, std::string>;
    std::set<RankKey> rank_index;
    std::map<std::string, RankKey> rank_keys;
    using SearchQualityKey = std::pair<double, std::string>;
    std::map<std::string, std::set<SearchQualityKey>> search_quality;
    std::map<std::string, std::pair<std::string, double>> search_quality_keys;
    std::set<std::string> rank_dirty;
    std::map<std::string, std::uint64_t> admission_waits;
    std::map<std::string, std::pair<std::uint64_t, double>> report_progress;
    bool ranking_initialized = false;
    std::string stage = "SEARCH_AND_EXACT", reason = "RUNNING";
    bool had_error = false;
    static fs::path prepare_out(const fs::path &p) {
        fs::create_directories(p);
        return p;
    }
    static std::uint64_t memory_budget(const Options &a, std::uint64_t free) {
        return a.ram_gib > 0 ? std::min(free, std::uint64_t(a.ram_gib * GiB))
                             : std::uint64_t(free * a.ram_fraction);
    }
    static unsigned worker_capacity(unsigned cores, std::uint64_t budget) {
        const auto controller=Resources::controller+mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0);
        auto room = budget > controller ? budget - controller : 0;
        return unsigned(std::min<std::uint64_t>(cores > 1 ? cores - 1 : 0,
                                                room / (Resources::overhead + Resources::minimum)));
    }
    explicit Campaign(Options options)
        : a(std::move(options)), cipher(read_json(a.variant / "config.json")),
          available_at_start(available_memory()), budget_at_start(memory_budget(a, available_at_start)),
          cores(std::min(a.cores, available_cpus())), slots(worker_capacity(cores, budget_at_start)),
          evalslots(std::max(1u, slots / 3)), searchslots(slots > evalslots ? slots - evalslots : 0),
          ram(available_at_start, budget_at_start, slots), workers(cipher, ram, a.variant), pool(slots),
          lock(prepare_out(a.out) / "run.lock"), db(a.out / "state.sqlite3") {
        require(cores >= 2, "At least two available CPUs are required");
        require(slots >= 1, "Available RAM cannot admit one worker and the controller reserve");
        if(a.transfer_patterns && !cipher.present) {
            auto requested=SupportTransfer::memory_bytes(cipher.R,*std::max_element(a.counts.begin(),a.counts.end()));
            auto occupied=Resources::controller+mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0)+
                          std::uint64_t(slots)*(Resources::overhead+Resources::minimum);
            if(occupied<budget_at_start && requested<=budget_at_start-occupied) {
                ram.reserve_shared_preparation(requested);
                workers.support=std::make_shared<SupportTransfer>(SupportSpec(cipher),cipher.R,
                    *std::max_element(a.counts.begin(),a.counts.end()));
            } else workers.support_fallback="Insufficient RAM for shared DP AND all admitted workers; legacy generator active";
        }
        patterns = db.load("patterns");
        cases = db.load("cases");
        rebuild_endpoint_index();
        jobs = db.load("jobs");
        auto meta = db.load("meta");
        const bool resumed = meta.count("state") != 0;
        if (resumed) {
            m = meta.at("state");
            require(m.value("schema", std::string()) == "THERMO_CPP_CAMPAIGN_V1",
                    "This is not a C++ run; use a NEW directory, optionally --reuse-run OLD");
            base = number(m, "elapsed");
            if (!m.value("closed", true))
                m["timing_has_unclean_session"] = true;
            seed = m.at("seed");
            if (a.seed)
                require(*a.seed == seed, "Resume seed changed");
        } else {
            require(patterns.empty() && cases.empty() && jobs.empty(),
                    "Output contains a different experiment ledger");
            std::random_device rd;
            seed = a.seed.value_or((std::uint64_t(rd()) << 32) ^ rd());
            m = J{{"schema", "THERMO_CPP_CAMPAIGN_V1"},
                  {"seed", seed},
                  {"elapsed", 0},
                  {"counter", 0},
                  {"closed", true},
                  {"cpu", J::object()},
                  {"completed", J::object()},
                  {"phase_seconds", J::object()},
                  {"expansions", J::object()},
                  {"errors", J::array()},
                  {"imports", J::array()},
                  {"evaluation_counter", 0},
                  {"search_counter", 0},
                  {"focus", J::array()},

                  {"source_history", J::array()},
                  {"corridor_certificates", J::object()},
                  {"pair_tasks", J::object()},
                  {"domains", J::object()},
                  {"corridor_finished", 0},
                  {"scope_expansions", 0},
                  {"next_scope_expansion", a.adapt_every},
                  {"scope_turn", 0},
                  {"pair_turn", 0}};
        }
        for (const auto *key : {"present_roles", "endpoint_roles"})
            if (!m.contains(key) || m[key].is_null())
                m[key] = J::object();
        auto oldpath = a.out / "PROTOCOL.json";
        auto identity = J{{"config", cipher.cfg}, {"scientific", a.scientific()}};
        if (fs::exists(oldpath)) {
            auto old = read_json(oldpath);
            require(old.at("identity") == identity,
                    "Run model changed; use a new output directory");
        }

        const std::string gain_revision = "fixed_denominator_delta_v2";
        if (m.value("mass_feedback_revision", std::string()) != gain_revision) {
            std::uint64_t reset_cases=0,reset_patterns=0;
            for (auto &kv:cases) if (kv.second.erase("mass_work")) {dc.insert(kv.first);++reset_cases;}
            for (auto &kv:patterns) if (kv.second.erase("mass_work")) {dp.insert(kv.first);++reset_patterns;}
            if (reset_cases || reset_patterns)
                m["mass_feedback_resets"].push_back(J{{"elapsed",base},{"cases",reset_cases},
                    {"patterns",reset_patterns},{"reason","reduced numerator gain bug; certificates preserved"}});
            m["mass_feedback_revision"] = gain_revision;
        }
        protocol =
            J{{"schema", "THERMO_CPP_CAMPAIGN_V1"},
              {"seed", seed},
              {"identity", identity},
              {"runtime", "C++17 controller/preparation/verification; C++ native workers; no Python"},
              {"source_digest", source_digest()},
              {"numerical_arithmetic",
               "exact integer DDT products and dyadic sums; AIS floating point only for ranking"},
              {"cpu_requested", a.cores},
              {"cpu_available", available_cpus()},
              {"cpu_used", cores},
              {"worker_capacity", slots},
              {"cpu_policy", "one coordinator; remaining slots are shared; refill after each receipt; borrow idle search/evaluation capacity; bootstrap fills spare slots with count-balanced generation"},
              {"workers_limited_by_RAM", slots < cores - 1},
              {"command", a.command},

              {"sampling_is_probability_certificate", false},
              {"stop_on_probability", false},

              {"hours", a.hours},

              {"ram_policy", "one locked admission snapshot; a budget decrease defers NEW leases"},
              {"search_archive_exhaustive", false},
              {"scheduler_revision", "unrestricted_exact_evaluation_v1"},

              {"pattern_discovery",{{"generator",a.transfer_patterns?(a.mixed_pattern_generators?"mixed":"transfer"):"legacy"},
                  {"shared_DP_enabled",bool(workers.support)},
                  {"fallback",workers.support?J():J(workers.support_fallback)},
                  {"pattern_target","uniform labelled locally compatible binary sequences at specified total activity; canonicalized afterwards"},
                  {"DP_is_probability",false},{"DDT_legal_witness_required",true},
                  {"preparation_policy","parallel shared rows; existing search runs during preparation; no extra files"},
                  {"mixed_generator_policy","alternate complete count-list cycles, not global job parity; preserves legacy PT discovery and covers each count in both generators"},
                  {"unvisited_transfer_lookahead",a.transfer_lookahead?a.transfer_lookahead:std::max(1u,searchslots)},
                  {"lookahead_policy","pause new transfer generation while first-search queue is full; no pattern deletion or permanent scope restriction; alternate new-transfer/ordinary breadth opportunities"},
                  {"activity_replicas",a.activity_replicas},{"activity_levels",a.activity_levels},
                  {"activity_target","product of actual DDT/Q across independent shared-endpoint replicas; endpoint marginal Z(e)^m"},
                  {"activity_scope","all nonzero endpoints and legal trajectories; no fixed pattern or active-count rejection"},
                  {"activity_update","16-way shared endpoint block with independently summed 16^2 private XOR amplitudes in each replica"},
                  {"cold_sampling_convergence_claimed",false}}},
              {"mass_engine",{{"collapsed_every",a.collapsed_every},{"collapsed_work",a.collapsed_work},
                  {"mass_feedback_revision",gain_revision},
                  {"message_cache_requested_mib",a.message_cache_mib==UINT64_MAX?J("auto"):J(a.message_cache_mib)},{"message_cache_actual_bytes",mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0)},
                  {"mass_branch_work",a.branch_work},{"midori_fine_plan_bits",a.fine_plan_bits},{"local_bound_work",a.local_bound_work},
                  {"evaluation_policy",a.cost_priority?"mass-cost":"mass"},
                  {"scope_change",false},{"cache_is_a_probability_receipt",false}}},
              {"skinny_common_value_seeds", {{"enabled", !cipher.midori && !cipher.present && a.common_value_seeds},
                  {"scope", "exact common-value seed families; ordinary replica/AIS/exact releases equalities"},
                  {"max_concurrent_jobs", 1}, {"endpoint_submission_interval", a.common_seed_every},
                  {"job_seconds", a.common_seed_seconds>0?a.common_seed_seconds:a.pattern_seconds},
                  {"outputs_retained_per_completed_row", a.keep}, {"reference_endpoints_used", false},
                  {"probability_accounting", "separate seed certificate and ranking bound; never added to ordinary exact partial sums"}}},
              {"midori_binary_seeds", {{"enabled", cipher.midori && cipher.B == 4 && a.midori_binary_seeds},
                  {"scope", "independent equal-value legal starts only; later replica/AIS/exact releases equalities"},
                  {"keep_per_active_count_per_job", 4}, {"job_seconds", 60},
                  {"refresh_endpoint_submissions", 3*a.counts.size()}, {"reference_endpoints_used", false}}},
              {"adaptive_pattern_search", {{"enabled", a.adaptive_patterns}, {"joint_patterns", a.joint_patterns},
                                          {"recent_completed_jobs_per_action", a.adaptive_window_jobs},
                                          {"selection", "rank feasible source/action/bundle combinations; selected action owns its cost"},
                                          {"target", "(sum of listed representative-pattern masses at e)^m"},
                                          {"equilibrium_certified", false}}},

              {"mass_search_policy", "partial-inclusive certified expansion; independent breadth/2/4-replica turns; mass-directed revisits plus count-bucket coverage; PRESENT separate policy unchanged"},
              {"replica_focus_patterns", a.replica_focus_patterns},
              {"pattern_allocation", "dedicated persisted sequence; advance only on committed pattern submission; equal submission counts, not equal CPU time or yield"},
              {"evaluation_priority", "AIS with existing ESS penalty, bounded by certified partial mass; "
                                      "all retained candidates ranked without arrival-order override or queue cutoff"},
              {"exact_focus", "highest current probability priorities; refreshed at every scheduling decision; "
                               "running jobs finish without cancellation"},
              {"exact_retry", "unchanged-region retries double --exact-seconds up to --max-exact-seconds "
                               "within the remaining search budget"},
              {"prior_runs_copied", false}};
        m["source_history"].push_back(J{{"elapsed", base}, {"digest", protocol["source_digest"]},
                                         {"pattern_discovery",protocol.at("pattern_discovery")},
                                         {"mass_engine", protocol.at("mass_engine")},
                                         {"common_value_seeds", a.common_value_seeds},
                                         {"common_seed_every", a.common_seed_every},
                                         {"common_seed_seconds", a.common_seed_seconds},
                                         {"adaptive_patterns", a.adaptive_patterns},
                                         {"joint_patterns", a.joint_patterns},
                                         {"adaptive_window_jobs", a.adaptive_window_jobs},
                                         {"midori_binary_seeds", cipher.midori && cipher.B == 4 && a.midori_binary_seeds},
                                         {"replica_focus_patterns", a.replica_focus_patterns}});
        m["session_error"] = nullptr;
        m["closed"] = false;
        m["last_runtime_options"] = {{"hours", a.hours},  {"cores", cores},
                                      {"pattern_discovery",protocol.at("pattern_discovery")},
                                      {"mass_engine", protocol.at("mass_engine")},
                                         {"common_value_seeds", a.common_value_seeds},
                                      {"common_seed_every", a.common_seed_every},
                                      {"common_seed_seconds", a.common_seed_seconds},
                                      {"adaptive_patterns", a.adaptive_patterns},
                                      {"joint_patterns", a.joint_patterns},
                                      {"adaptive_window_jobs", a.adaptive_window_jobs},
                                      {"midori_binary_seeds", cipher.midori && cipher.B == 4 && a.midori_binary_seeds},
                                      {"replica_focus_patterns", a.replica_focus_patterns}};
        if (a.build_seconds > 0) {
            base += a.build_seconds;
            m["build_wall_seconds"] = number(m, "build_wall_seconds") + a.build_seconds;
        }
        initialize_pattern_allocation(m, a.counts, patterns, jobs, base, resumed);
        initialize_mass_search_allocation(m, patterns, dp, base, resumed, !cipher.present);
        if (cipher.midori && cipher.B == 4) initialize_binary_seeds(m, base);
        if (!cipher.midori && !cipher.present) initialize_common_seeds(m, a.counts, base);
        if(a.adaptive_window_jobs)for(auto &kv:patterns)if(kv.second.contains("adaptive_actions"))
            for(auto &s:kv.second["adaptive_actions"])while(s["window"].size()>a.adaptive_window_jobs) {
                s["window"].erase(s["window"].begin());dp.insert(kv.first);
            }
        auto scratch_id = digest(J::array({a.out.string(), seed, cipher.cfg["name"]})).substr(0, 24);
        scratch = (a.scratch.empty() ? fs::temp_directory_path() / "thermocryptanalysis_cpp" : a.scratch) /
                  scratch_id;
        fs::create_directories(scratch);
        require(!fs::is_symlink(scratch), "Scratch root may not be a symlink");
        for(auto &entry:cases) if(entry.second["exact"].is_null()) {
            entry.second["next_at"]=0;
            dc.insert(entry.first);
        }
        if (cipher.present)
            initialize_domains();
        save_json(oldpath, protocol);
        flush();

        recover();
    }
    double elapsed() const {
        return base + now() - started;
    }

    std::string source_digest() const {
        J files = J::object();
        for (auto dir : {a.variant / "src", a.variant.parent_path() / "common"})
            for (auto &entry : fs::recursive_directory_iterator(dir))
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension();
                    if (ext == ".hpp" || ext == ".cpp" || ext == ".h")
                        files[fs::relative(entry.path(), a.variant.parent_path()).generic_string()] =
                            sha256(read_text(entry.path()));
                }
        files["config.json"] = sha256(read_text(a.variant / "config.json"));
        return digest(files);
    }
    void flush(bool closed = false) {
        db.exec("BEGIN IMMEDIATE");
        try {
            for (auto &id : dp)
                db.put("patterns", id, patterns.at(id));
            for (auto &id : dc)
                db.put("cases", id, cases.at(id));
            m["elapsed"] = elapsed();
            m["closed"] = closed;
            m["last_heartbeat_unix"] = std::time(nullptr);
            db.put("meta", "state", m);
            db.exec("COMMIT");
            dp.clear();
            dc.clear();
            last_flush = now();
        } catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
    }
    std::vector<const J *> running(const std::string &kind, const std::string &key = "") const {
        std::vector<const J *> r;
        for (auto &kv : active) {
            auto &j = kv.second.job;
            if (j["kind"] == kind && (key.empty() || j.value("cid", j.value("pid", std::string())) == key))
                r.push_back(&j);
        }
        return r;
    }
    unsigned used_cores(bool evaluation) const {
        unsigned n = 0;
        for (auto &kv : active)
            if (kv.second.job.at("evaluation").get<bool>() == evaluation)
                n += kv.second.job.value("cpu_cores", 1u);
        return n;
    }
    void pattern(const J &record) {
        auto id = record.at("id").get<std::string>();
        if (patterns.count(id)) {
            require(patterns[id]["pattern"] == record["pattern"], "Pattern identity collision");
            return;
        }
        auto p = record;
        p["searches"] = 0;
        p["states"] = nullptr;
        patterns[id] = p;
        if (!cipher.present)
            register_search_pattern(m, patterns.at(id), dp);
        dp.insert(id);
    }
    void rebuild_endpoint_index() {
        endpoint_cases.clear();
        union_dirty_endpoints.clear();
        partial_by_endpoint.clear(); complete_by_endpoint.clear();
        for (const auto &kv : cases)
            endpoint_cases[kv.second.at("endpoint").get<std::string>()].insert(kv.first);
        for (const auto &kv : endpoint_cases) union_dirty_endpoints.insert(kv.first);
    }
    void hit(const J &h, const J &job) {
        pattern(h["pattern_record"]);
        auto pid = h["pattern_record"]["id"].get<std::string>(), id = h["id"].get<std::string>();
        auto &p = patterns.at(pid);
        if (p["states"].is_null()) {
            p["states"] = h["states"];
            dp.insert(pid);
        }
        J origin{{"kind", job["kind"]},
                 {"job", job["id"]},
                 {"seed", job["seed"]},
                 {"replicas", job.value("copies", J())},
                 {"search_allocation", job.value("search_allocation", J())},
                 {"expansion_allocation", job.value("expansion_allocation", J())},
                 {"adaptive_action", job.value("adaptive_action", J())},
                 {"joint_pattern_ids", J::array()},
                 {"warm_started", job.contains("states") && !job["states"].is_null()}};
        if (is_binary_seed(job)) origin["seed_method"] = "independent_binary_supports";
        if (is_common_seed(job)) origin["seed_method"] = "exact_common_value_family_mass";
        for (const auto &member : job.value("joint_members", J::array()))
            origin["joint_pattern_ids"].push_back(member.at("pid"));
        double first = job.at("started").get<double>() + number(h, "first_seconds");
        if (!cases.count(id)) {
            J c{{"id", id},
                {"pid", pid},
                {"endpoint", h["endpoint"]},
                {"input", h["input"]},
                {"output", h["output"]},
                {"states", h["states"]},
                {"symmetry", h["symmetry"]},
                {"weight", h["weight"]},
                {"witness_numerator", h["witness_numerator"]},
                {"active", p["active"]},
                {"first_retained_seconds", first},
                {"origins", J::array({origin})},
                {"screen", J()},
                {"screen_attempts", 0},
                {"plan", J()},
                {"frontier", J::object()},
                {"frontier_upper", J::object()},
                {"slices", J::object()},
                {"attempts", J::object()},
                {"partial_numerator", "0"},
                {"closed_volume", "0"},
                {"solved_nodes", 0},
                {"zero_nodes", 0},
                {"cache_hits", 0},
                {"exact", J()},
                {"next_at", 0},
                {"exact_cpu_seconds", 0},
                {"exact_job_wall_seconds", 0},
                {"exact_peak_rss_kib", 0}};
            cases.emplace(id, std::move(c));
            endpoint_cases[h.at("endpoint").get<std::string>()].insert(id);
        } else {
            auto &c = cases.at(id);
            require(c["symmetry"]["pattern_orbit"] == h["symmetry"]["pattern_orbit"],
                    "Symmetry orbit changed");
            c["first_retained_seconds"] = std::min(number(c, "first_retained_seconds"), first);
            if (c["origins"].size() < 8)
                c["origins"].push_back(origin);
            if (integer(h["witness_numerator"]) > integer(c["witness_numerator"])) {
                c["witness_numerator"] = h["witness_numerator"];
                c["states"] = h["states"];
                c["weight"] = h["weight"];
            }
        }
        if (h.contains("common_value_seed")) {
            require(is_common_seed(job), "Common-value seed certificate without matching worker provenance");
            merge_common_seed(cases.at(id), h.at("common_value_seed"));
        }
        dc.insert(id);
        rank_dirty.insert(id);
        union_dirty_endpoints.insert(h.at("endpoint"));
        unions_dirty = true;
    }
    unsigned exponent(const J &c) const {
        return cipher.DB * c.at("active").get<unsigned>();
    }
    unsigned multiplicity(const J &c) const {
        return c.at("symmetry").at("multiplicity").get<unsigned>();
    }
    J raw_mass(const J &c) const {
        return !c.at("exact").is_null() ? c["exact"] : dyadic(integer(c["partial_numerator"]), exponent(c));
    }
    J orbit_mass(const J &c) const {
        return scale_mass(raw_mass(c), multiplicity(c));
    }
    J bounds(const J &c) const {
        auto lo = raw_mass(c);
        Z n = integer(c["partial_numerator"]), hi = pow2(exponent(c));
        if (!c["exact"].is_null())
            hi = n;
        else {
            if (c.contains("root_bound"))
                hi = integer(c["root_bound"]["upper_numerator"]);
            if (c.contains("proven_total_upper"))
                hi = std::min(hi, integer(c["proven_total_upper"]));
        }
        require(hi >= n, "Certified lower exceeds upper");
        Z oh = hi * multiplicity(c);
        return J{{"lower", scale_mass(lo, multiplicity(c))},
                 {"upper", dyadic(std::min(oh, pow2(exponent(c))), exponent(c))},
                 {"scope", "represented same-endpoint activity-pattern orbit only"},
                 {"complete", !c["exact"].is_null()}};
    }
    double score(const J &c) const {
        auto &s = c["screen"];
        double v = number(c, "weight", INFINITY);
        if (s.is_object() && s.contains("F") && s["F"].is_number())
            v = number(s, "F") +
                std::log2(std::max(1., number(s, "paths", a.paths) / std::max(1., number(s, "ess"))));
        if (c.contains("root_bound")) {
            auto u = bounds(c)["upper"];
            if (u["F"].is_number())
                v = std::max(v, u["F"].get<double>() + std::log2(multiplicity(c)));
        }

        auto certified = raw_mass(c);
        if (certified["F"].is_number())
            v = std::min(v, certified["F"].get<double>());
        v = std::min(v, common_seed_F(c));
        return v;
    }
    void refresh_ranking() {
        if (!ranking_initialized) {
            for (auto &kv : cases)
                rank_dirty.insert(kv.first);
            ranking_initialized = true;
        }
        for (const auto &id : rank_dirty) {
            auto old = rank_keys.find(id);
            if (old != rank_keys.end())
                rank_index.erase(old->second);
            auto &c = cases.at(id);
            RankKey key{score(c), number(c, "first_retained_seconds"), id};
            rank_keys.insert_or_assign(id, key);
            rank_index.insert(std::move(key));
            if (!cipher.present) {
                auto prior = search_quality_keys.find(id);
                if (prior != search_quality_keys.end())
                    search_quality[prior->second.first].erase({prior->second.second, id});
                const std::string pid = c.at("pid");
                const double q = replica_quality(c);
                search_quality[pid].insert({q, id});
                search_quality_keys.insert_or_assign(id, std::make_pair(pid, q));
            }
        }
        rank_dirty.clear();
    }
    double replica_quality(const J &c) const {
        if (a.adaptive_patterns) {
            if (!c.at("exact").is_null())
                return number(c.at("exact"), "F", INFINITY);

            return score(c);
        }

        if (std::isfinite(common_seed_F(c))) return score(c);
        auto certified = raw_mass(c);
        if (certified["F"].is_number())
            return certified["F"].get<double>();
        if (!c.at("exact").is_null())
            return INFINITY;
        const auto &s = c.at("screen");
        if (s.is_object() && s.contains("F") && s["F"].is_number())
            return number(s, "F") + std::log2(std::max(1., number(s, "paths", a.paths) /
                                                                        std::max(1., number(s, "ess", 1))));
        return number(c, "weight", INFINITY);
    }
    bool admission_blocked(const std::string &key) {
        auto it = admission_waits.find(key);
        return it != admission_waits.end() && it->second == ram.admission_version();
    }
    static std::string admission_key(const J &j) {
        if (j.contains("cid"))
            return j.at("cid");
        if (j.contains("pair_id"))
            return "pair:" + j.at("pair_id").get<std::string>();
        if (j.contains("domain"))
            return "domain:" + j.at("domain").get<std::string>();
        return "kind:" + j.at("kind").get<std::string>();
    }
    double exact_seconds_for(const J &c, const std::string &key) const {

        return std::min(a.max_exact_seconds,
                        a.exact_seconds * std::exp2(std::min<std::uint64_t>(20, count(c.at("attempts"), key.c_str()))));
    }
    bool covered(const J &c) const {
        if (!cipher.present)
            return false;
        auto ep = c["endpoint"].get<std::string>();
        if (!m["corridor_certificates"].contains(ep))
            return false;
        int cap = m["corridor_certificates"][ep]["cap"];
        return activity_cap(c) <= cap;
    }
    int activity_cap(const J &c) const {
        int n = 0;
        auto &p = patterns.at(c.at("pid").get<std::string>())["pattern"];
        for (int r = 1; r < cipher.R; ++r) {
            auto s = p[r].get<std::string>();
            n = std::max(n, int(std::count(s.begin(), s.end(), '1')));
        }
        return n;
    }
    void refresh_unions(bool force = false) {
        if (!unions_dirty || (!force && now() - union_clock < 2))
            return;
        union_clock = now();
        complete_cache.clear();
        partial_cache.clear();
        if (!cipher.present) {

            if (union_dirty_endpoints.empty() && !cases.empty()) rebuild_endpoint_index();
            for (const auto &ep : union_dirty_endpoints) {
                partial_by_endpoint.erase(ep); complete_by_endpoint.erase(ep);
                for (bool partial : {false, true}) {
                    J g; std::vector<J> parts;
                    for (const auto &id : endpoint_cases.at(ep)) {
                        const auto &c=cases.at(id);
                        if (!partial && c.at("exact").is_null()) continue;
                        auto mass=orbit_mass(c);
                        if (integer(mass.at("numerator"))==0) continue;
                        if (g.is_null()) g={{"endpoint",ep},{"input",c.at("input")},{"output",c.at("output")},
                            {"component_ids",J::array()},{"complete_patterns",0},{"unfinished_patterns",0}};
                        g["component_ids"].push_back(id);
                        increment(g,c.at("exact").is_null()?"unfinished_patterns":"complete_patterns",multiplicity(c));
                        parts.push_back(std::move(mass));
                    }
                    if (!g.is_null()) {
                        g["mass"]=add_mass(parts);

                        (partial?partial_by_endpoint:complete_by_endpoint)[ep]=std::move(g);
                    }
                }
            }
            for (const auto &kv:partial_by_endpoint)partial_cache.push_back(kv.second);
            for (const auto &kv:complete_by_endpoint)complete_cache.push_back(kv.second);
            auto less=[](const J &x,const J &y){int c=compare_mass(x.at("mass"),y.at("mass"));
                return c?c>0:x.at("endpoint")<y.at("endpoint");};
            std::sort(partial_cache.begin(),partial_cache.end(),less);
            std::sort(complete_cache.begin(),complete_cache.end(),less);
            union_dirty_endpoints.clear();unions_dirty=false;return;
        }
        for (bool partial : {false, true}) {
            std::map<std::string, J> groups;
            std::map<std::string, std::vector<J>> parts, inside;
            for (auto &kv : cases) {
                auto &c = kv.second;
                if (!partial && c["exact"].is_null())
                    continue;
                auto mass = orbit_mass(c);
                if (integer(mass["numerator"]) == 0)
                    continue;
                auto ep = c["endpoint"].get<std::string>();
                if (covered(c)) {
                    inside[ep].push_back(mass);
                    continue;
                }
                if (!groups.count(ep))
                    groups[ep] = J{{"endpoint", ep},         {"input", c["input"]},
                                   {"output", c["output"]},  {"component_ids", J::array()},
                                   {"complete_patterns", 0}, {"unfinished_patterns", 0}};
                auto &g = groups[ep];
                g["component_ids"].push_back(c["id"]);
                increment(g, c["exact"].is_null() ? "unfinished_patterns" : "complete_patterns",
                          multiplicity(c));
                parts[ep].push_back(mass);
            }
            for (auto it = m["corridor_certificates"].begin(); it != m["corridor_certificates"].end(); ++it) {
                auto ep = it.key();
                auto &cert = it.value();
                if (inside.count(ep))
                    require(compare_mass(add_mass(inside[ep]), cert["mass"]) <= 0,
                            "Covered pattern mass exceeds corridor");
                if (!groups.count(ep))
                    groups[ep] = J{{"endpoint", ep},           {"input", cert["input"]},
                                   {"output", cert["output"]}, {"component_ids", J::array()},
                                   {"complete_patterns", 0},   {"unfinished_patterns", 0}};
                groups[ep]["corridor"] = cert;
                parts[ep].push_back(cert["mass"]);
            }
            auto &dest = partial ? partial_cache : complete_cache;
            for (auto &kv : groups) {
                auto g = kv.second;
                g["mass"] = add_mass(parts[kv.first]);

                dest.push_back(g);
            }
            std::sort(dest.begin(), dest.end(), [](const J &x, const J &y) {
                int cmp = compare_mass(x["mass"], y["mass"]);
                return cmp ? cmp > 0 : x["endpoint"] < y["endpoint"];
            });
        }
        unions_dirty = false;
    }
    void initialize_domains();
    J corridor_job();
    J pair_job(unsigned available);
    J present_search();
    void corridor_completed(const J &, const J &);
    void merge_certificates(const J &, const J &);
    J search_job();
    J endpoint_search_job();
    J activity_search_job();
    J adaptive_search_job(J job, const std::vector<J *> &witnessed, bool targeted);
    J adaptive_expansion_job();
    J evaluation_job(unsigned capacity = 0);
    J final_pair_job(unsigned available);

    void import_previous();
    static std::string resource_key(const J &j) {
        return j.at("kind") == "plan" ? "plan"
               : j.contains("box_id") ? "box:" + j.at("box_id").get<std::string>()
                                      : "slice:" + str(j.at("slice"));
    }

    bool submit(J j) {
        if (j.is_null())
            return false;
        tag_pattern_submission(j, m, a.counts);
        validate_mass_search_submission(m, j);

        if (used_cores(false) + used_cores(true) + j.value("cpu_cores", 1u) > slots)
            return false;
        const auto wait_key = admission_key(j);
        if (admission_blocked(wait_key))
            return false;
        double remaining =  a.hours * 3600 - elapsed();
        if (remaining <= 0 || stopping)
            return false;
        auto id = count(m, "counter") + 1;
        auto state = std::make_shared<TaskState>();
        if (!ram.attach(id, state)) {
            m["resource_wait"] = "RAM admission is waiting; existing leases are preserved";
            admission_waits[wait_key] = ram.admission_version();
            return false;
        }
        m["resource_wait"] = nullptr;
        auto seconds = std::min(remaining, j.at("seconds").get<double>());
        j["id"] = id;
        j["seed"] = std::stoull(digest(J::array({seed, id, j["kind"]})).substr(0, 16), nullptr, 16);
        j["started"] = elapsed();
        j["seconds"] = seconds;
        j["deadline"] = now() + seconds;
        j["keep"] = j.value("keep", a.keep);
        j["scratch"] = (scratch / "jobs" / std::to_string(id)).string();
        j["table_desired"] = sat_mul(ram.base_table( evalslots), j.value("cpu_cores", 1u));
        j["table_cap"] = a.job_ram_gib > 0 ? std::uint64_t(a.job_ram_gib * GiB) : 0;

        j["max_slices"] = a.max_slices;
        if (j["kind"] == "pair")
            j["table_required"] = count(m["pair_tasks"].value(j["pair_id"].get<std::string>(), J::object()),
                                        "required_table_bytes");
        if ((j["kind"] == "exact" || j["kind"] == "plan") && j.contains("cid")) {
            auto &cc = cases.at(j["cid"]);
            auto needs = cc.value("resource_required_tables", J::object());
            j["table_required"] = count(needs, resource_key(j).c_str());
        }
        if (j["kind"] == "exact" && j.contains("cid")) {
            auto &cc = cases.at(j["cid"]);
            auto plan = cc.value("plan", J());
            if (plan.is_object() && plan.contains("estimated_table_bytes") &&
                plan["estimated_table_bytes"].is_number_unsigned() &&
                plan["estimated_table_bytes"].get<std::uint64_t>() > 0) {
                auto estimate = plan["estimated_table_bytes"].get<std::uint64_t>();
                if (!cipher.cfg["adaptive"].get<bool>())
                    j["table_required"] = std::max(count(j, "table_required"), estimate);
                j["table_desired"] = std::min(j["table_desired"].get<std::uint64_t>(),
                                              std::max(Resources::minimum, sat_mul(estimate, 5) / 4));
            }
        }
        if (j.contains("cid") && j.contains("table_required") &&
            count(j, "table_required") > ram.maximum_table( searchslots, count(j, "table_cap"))) {
            auto &cc = cases.at(j["cid"]);
            const auto why = "WAITING_FOR_LARGER_EXACT_RAM: required " + str(j["table_required"]) + " bytes";
            cc["last_reason"] = why;
            dc.insert(cc["id"]);

            ram.release(state);
            admission_waits[wait_key] = ram.admission_version();
            m["resource_wait"] = why;
            return false;
        }
        if (j["kind"] == "pair" &&
            count(j, "table_required") > ram.maximum_table( searchslots, count(j, "table_cap"))) {
            auto name = j["pair_id"].get<std::string>();
            auto &pt = m["pair_tasks"][name];
            pt["reason"] = "WAITING_FOR_LARGER_PAIR_RAM";

            ram.release(state);
            admission_waits[wait_key] = ram.admission_version();
            m["resource_wait"] = pt["reason"];
            return false;
        }

        const std::string kind = j["kind"];
        if ((kind == "exact" || kind == "plan" || kind == "pair" || kind == "corridor") &&
            j.value("partition_target", 0) <= 1) {
            std::uint64_t table = 0;
            if (!ram.native(state, count(j, "table_desired"), count(j, "table_required"),
                            count(j, "table_cap"), table)) {
                ram.release(state);
                admission_waits[wait_key] = ram.admission_version();
                m["resource_wait"] = "WAITING_FOR_EXACT_RAM: not launched; retry when available RAM changes";
                if (j.contains("cid")) {
                    auto &cc = cases.at(j["cid"]);
                    cc["last_reason"] = "WAITING_FOR_EXACT_RAM";
                    dc.insert(cc["id"]);
                }
                return false;
            }
            j["ram_pre_admitted"] = true;
            j["table_bytes"] = table;
        }
        bool submission_transaction = false;
        try {
            db.exec("BEGIN IMMEDIATE");
            submission_transaction = true;
            admission_waits.erase(wait_key);
            j["receipt_digest"] = digest(j);
            m["counter"] = id;
            record_pattern_submission(m, j);
            record_mass_search_submission(m, patterns, dp, j);
            record_binary_seed_submission(m, j);
            record_common_seed_submission(m, j);
            if(kind=="activity") {
                increment(m,"activity_turn");
                auto &p=patterns.at(j.at("pid").get<std::string>());
                increment(p,"activity_submissions");p["activity_last_job"]=id;dp.insert(p.at("id"));
            }
            adaptive_submit(patterns, dp, j);
            if (!j["evaluation"].get<bool>())
                increment(m, "search_counter");
            if (cipher.present) {
                auto kind = j["kind"].get<std::string>();
                increment(m["present_roles"], kind);
                if (j.contains("endpoint")) {
                    auto ep = j["endpoint"].get<std::string>();
                    if (!m["endpoint_roles"].contains(ep))
                        m["endpoint_roles"][ep] = J::object();
                    increment(m["endpoint_roles"][ep], kind);
                    m["endpoint_roles"][ep][kind + "_last"] = id;
                }
                if (kind == "pair")
                    increment(m, "pair_turn");
            }
            if (j["kind"] == "search") {
                auto &p = patterns.at(j["pid"].get<std::string>());
                increment(p, "searches");
                dp.insert(p["id"]);
            }
            if (j["kind"] == "screen") {
                auto &c = cases.at(j["cid"]);
                increment(c, "screen_attempts");
                dc.insert(c["id"]);
            }
            if (j["kind"] == "corridor") {
                auto &d = m["domains"][j["domain"].get<std::string>()];
                increment(d, "submitted");
                d["last_submitted"] = id;
            }
            if (j["kind"] == "pair") {
                auto &pt = m["pair_tasks"][j["pair_id"].get<std::string>()];
                if (pt.is_null())
                    pt = J{{"endpoint", j["endpoint"]},
                           {"cap", j["cap"]},
                           {"cpu_seconds", 0},
                           {"stalls", 0},
                           {"done", false}};
                increment(pt, "jobs");
                pt["last"] = id;
                pt["states"] = j["states"];
                pt["work"] = j["work"];
            }
            if (j["kind"] == "exact") {
                auto &c = cases.at(j["cid"]);
                if (!c.contains("exact_first_submitted_seconds"))
                    c["exact_first_submitted_seconds"] = elapsed();
                dc.insert(c["id"]);
            }
            if (j["kind"] == "plan") {
                auto &c = cases.at(j["cid"]);
                if (!c.contains("plan_first_submitted_seconds"))
                    c["plan_first_submitted_seconds"] = elapsed();
                dc.insert(c["id"]);
            }
            for (auto &k : dp)
                db.put("patterns", k, patterns.at(k));
            for (auto &k : dc)
                db.put("cases", k, cases.at(k));
            db.put("jobs", std::to_string(id), j);
            m["elapsed"] = elapsed();
            db.put("meta", "state", m);
            db.exec("COMMIT");
            submission_transaction = false;
            dp.clear();
            dc.clear();
        } catch (...) {
            if (submission_transaction)
                db.exec("ROLLBACK");
            ram.release(state);

            patterns = db.load("patterns");
            cases = db.load("cases");
            rebuild_endpoint_index();
            jobs = db.load("jobs");
            m = db.load("meta").at("state");
            dp.clear();
            dc.clear();
            rank_index.clear();
            rank_keys.clear();
            rank_dirty.clear();
            search_quality.clear();
            search_quality_keys.clear();
            ranking_initialized = false;
            unions_dirty = true;
            throw;
        }
        jobs[std::to_string(id)] = j;
        std::function<J()> task = [this, j, state]() { return workers.execute(j, state); };
        auto future = pool.submit(std::move(task));
        active.emplace(id, Running{j, state, std::move(future)});
        return true;
    }
    void consume(const J &job, const J &r) {

        try {
            apply_receipt(job, r);
        } catch (...) {
            patterns = db.load("patterns");
            cases = db.load("cases");
            rebuild_endpoint_index();
            jobs = db.load("jobs");
            m = db.load("meta").at("state");
            dp.clear();
            dc.clear();
            rank_index.clear();
            rank_keys.clear();
            rank_dirty.clear();
            search_quality.clear();
            search_quality_keys.clear();
            ranking_initialized = false;
            unions_dirty = true;
            throw;
        }
    }
    void record_error(const std::string &why, const J &job = J(), const J &receipt = J()) {
        J error{{"reason", why}, {"seconds", elapsed()}, {"stage", stage}};
        if (job.is_object()) {
            error["job"] = job.at("id"); error["kind"] = job.at("kind");
            for (const char *key : {"cid", "pid", "receipt_digest", "scratch"})
                if (job.contains(key)) error[key] = job.at(key);
        }
        if (receipt.is_object()) {
            for (const char *key : {"native_exit_code", "native_stderr", "native_stdout"})
                if (receipt.contains(key)) error[key] = receipt.at(key);
        }
        m["errors"].push_back(error);
        if (!m.contains("session_error") || m["session_error"].is_null()) {
            m["session_error"] = error;

            J detail=error; if (job.is_object()) detail["submitted_job"]=job;
            try { save_json(a.out / "ERROR.json", detail); }
            catch (const std::exception &x) { std::cerr << "Error diagnostic save failed: " << x.what() << std::endl; }
        }
        std::cerr << "ERROR";
        if (job.is_object()) std::cerr << " [" << str(job.at("kind")) << " job " << job.at("id") << ']';
        std::cerr << ": " << why << std::endl;
    }
    void apply_receipt(const J &job, const J &r) {
        require(jobs.count(str(job.at("id"))) != 0, "Receipt was already consumed or was never submitted");
        require(jobs.at(str(job.at("id"))) == job, "Job identity differs from its durable submission");
        require(r.at("job_id") == job.at("id") && r.at("job_digest") == job.at("receipt_digest"),
                "Worker receipt identity mismatch");
        std::string kind = job.at("kind");
        Z mass_before=0;
        if(kind=="exact" && job.contains("cid"))mass_before=integer(cases.at(job.at("cid").get<std::string>()).at("partial_numerator"));
        increment(m["completed"], kind);
        double cpu = number(r, "cpu_seconds") + number(r, "cpp_thread_cpu_seconds");
        m["cpu"][kind] = number(m["cpu"], kind.c_str()) + cpu;
        for (auto ph : {"preparation", "native", "verification"})
            m["phase_seconds"][ph] =
                number(m["phase_seconds"], ph) + number(r.value("phase_seconds", J::object()), ph);
        if (r.value("forced_native_termination", false))
            increment(m, "forced_native_terminations");
        if (r.value("status", std::string()) == "ERROR") {
            record_error(r.value("reason", std::string("worker error")), job, r);
            had_error = true;
            reason = "ERROR";
        }
        std::uint64_t new_patterns = 0;
        for (auto &p : r.value("patterns", J::array())) {
            new_patterns += patterns.count(p.at("id").get<std::string>()) == 0;
            pattern(p);
        }
        record_pattern_receipt(m, job, r, new_patterns);
        if(kind=="pattern") {
            const auto generator=r.value("generator",std::string("legacy"));
            auto &s=m["pattern_generators"][generator];if(s.is_null())s=J::object();
            increment(s,"jobs");increment(s,"new_patterns",new_patterns);
            increment(s,"drawn_patterns",count(r,"support_draws"));increment(s,"structurally_invalid",count(r,"invalid_patterns"));
            s["cpu_seconds"]=number(s,"cpu_seconds")+cpu;
            auto &by_count=s["by_active"][str(job.at("active"))];if(by_count.is_null())by_count=J::object();
            increment(by_count,"jobs");increment(by_count,"new_patterns",new_patterns);
            increment(by_count,"drawn_patterns",count(r,"support_draws"));increment(by_count,"structurally_invalid",count(r,"invalid_patterns"));
            by_count["cpu_seconds"]=number(by_count,"cpu_seconds")+cpu;
            if(r.contains("compatible_sequence_count"))s["compatible_sequences_by_active"][str(job.at("active"))]=r.at("compatible_sequence_count");
        }
        if(kind=="search" && r.contains("diagnostics")){
            auto &metrics=m["mass_engine_counters"];if(metrics.is_null())metrics=J::object();
            for(const auto*key:{"collapsed_attempted","collapsed_completed","collapsed_work_skips","collapsed_endpoint_changes","collapsed_internal_changes","search_message_hits"})
                increment(metrics,key,count(r.at("diagnostics"),key));
            metrics["collapsed_cpu_seconds"]=number(metrics,"collapsed_cpu_seconds")+number(r.at("diagnostics"),"collapsed_cpu_seconds");
        }
        record_mass_search_receipt(m, job, r);
        std::vector<std::string> created;
        std::uint64_t duplicates = 0,activity_new_patterns=0;
        J seed_new_patterns = J::object();
        for (auto &h : r.value("hits", J::array())) {
            auto cid = h.at("id").get<std::string>();
            if (!cases.count(cid)) created.push_back(cid); else ++duplicates;
            if(kind=="activity" && !patterns.count(h.at("pattern_record").at("id")))++activity_new_patterns;
            if ((is_binary_seed(job) || is_common_seed(job)) && !patterns.count(h.at("pattern_record").at("id")))
                increment(seed_new_patterns, str(h.at("pattern_record").at("active")));
            hit(h, job);
        }
        if(kind=="activity") {
            auto &stats=m["activity_search"];if(stats.is_null())stats=J::object();
            increment(stats,"jobs");increment(stats,"new_families",created.size());increment(stats,"duplicate_families",duplicates);
            increment(stats,"new_patterns",activity_new_patterns);
            stats["cpu_seconds"]=number(stats,"cpu_seconds")+cpu;
            for(const auto *key:{"updates","changed","shared_updates","shared_changed","swap_attempts","swaps","observed_pattern_changes","observed_endpoint_changes","distinct_endpoint_patterns"})
                increment(stats,key,count(r.value("activity_diagnostics",J::object()),key));
            if(r.contains("activity_resume")) {
                auto &p=patterns.at(job.at("pid").get<std::string>());
                p["activity_continuation"][str(job.at("copies"))]=r.at("activity_resume");dp.insert(p.at("id"));
            }
        }
        record_binary_seed_receipt(m, job, r, seed_new_patterns, created.size(), duplicates);
        record_common_seed_receipt(m, job, r, seed_new_patterns, created.size(), duplicates);
        adaptive_receipt(patterns, cases, dp, job, r, created, duplicates, a.adaptive_window_jobs);
        if (r.contains("corridor_records"))
            merge_certificates(job, r);
        if (kind == "corridor")
            corridor_completed(job, r);
        if (kind == "pair") {
            auto &pt = m["pair_tasks"][job["pair_id"].get<std::string>()];
            pt["cpu_seconds"] = number(pt, "cpu_seconds") + cpu;
            J old = pt.value("progress", J::object()), p = r.value("pair_progress", J::object());
            bool progress = !p.empty() && (p.value("phase", J()) != old.value("phase", J()) ||
                                           p.value("round", J()) != old.value("round", J()) ||
                                           count(p, "completed_sources") > count(old, "completed_sources"));
            pt["stalls"] = progress ? 0 : count(pt, "stalls") + 1;
            if (!p.empty())
                pt["progress"] = p;
            pt["retry_at"] =
                elapsed() +
                (progress ? 1 : std::min(120., 5 * std::exp2(std::min(4., number(pt, "stalls")))));
            pt["done"] = !r.value("corridor_records", J::array()).empty();
            pt["reason"] = r.value("reason", std::string());
            const auto why = pt["reason"].get<std::string>();
            bool memory = why.find("RESOURCE_LIMIT") != std::string::npos ||
                          why.find("RAM") != std::string::npos || why.find("MEMORY") != std::string::npos;
            bool can_grow = false;
            if (memory && !pt["done"].get<bool>()) {
                auto old = count(r, "table_bytes_used", Resources::minimum);
                auto maximum = ram.maximum_table( searchslots,
                                                 a.job_ram_gib > 0 ? std::uint64_t(a.job_ram_gib * GiB) : 0);
                auto need = std::min(maximum, std::max(sat_mul(old, 2), sat_add(old, Resources::minimum)));
                can_grow = need > old;
                if (can_grow)
                    pt["required_table_bytes"] = need;
            }

        }
        if (kind == "screen" || kind == "plan" || kind == "exact") {
            auto &c = cases.at(job.at("cid").get<std::string>());
            dc.insert(c["id"]);
            rank_dirty.insert(c["id"]);
            auto status = r.value("status", std::string());
            if(kind=="exact" && (status=="EXACT_PROGRESS" || status=="EXACT_DONE" || status=="EXACT_SLICE")) {

                require(r.at("denominator_power").is_number_integer() &&
                        integer(r.at("denominator_power"))==Z(exponent(c)),
                        "Exact receipt denominator differs from family ledger");
                require(integer(r.at("numerator"))>=0,"Negative exact receipt numerator");
            }
            if (r.contains("root_bound")) {

                const auto &fresh=r.at("root_bound");
                Z upper=integer(fresh.at("upper_numerator"));
                require(upper>=integer(c.at("partial_numerator")) &&
                        upper>=integer(c.at("witness_numerator")) && upper<=pow2(exponent(c)),
                        "Root upper bound contradicts certified family mass");
                if(c.contains("root_bound")) {
                    const auto &old=c.at("root_bound");
                    if(old.contains("domain_volume") && fresh.contains("domain_volume"))
                        require(integer(old.at("domain_volume"))==integer(fresh.at("domain_volume")),
                                "Root domain volume changed on resume");
                    if(integer(old.at("upper_numerator"))>upper)c["root_bound"]=fresh;
                } else c["root_bound"]=fresh;
                if(c.contains("proven_total_upper"))
                    c["proven_total_upper"]=std::min(integer(c.at("proven_total_upper")),
                        integer(c.at("root_bound").at("upper_numerator"))).get_str();
            }
            if (kind == "screen")
                c["screen"] = combine_screen(c["screen"], r);
            else if (kind == "plan") {
                if (status == "PLAN") {
                    c["plan"] = r;
                    c["root_volume"] = r["initial_volume"];
                    auto k = digest(J::array());
                    c["frontier"][k] = J::array();
                    c["frontier_upper"][k] = c["root_bound"]["upper_numerator"];
                    c["last_reason"] = nullptr;
                    c["next_at"] = 0;
                } else {
                    increment(c, "plan_failures");
                    c["last_reason"] = r.value("reason", std::string("PLAN_INCOMPLETE"));
                    c["next_at"] =
                        elapsed() +
                        std::min(600., 30 * std::exp2(std::min(5., number(c, "plan_failures") - 1)));
                }
            } else {
                c["exact_cpu_seconds"] = number(c, "exact_cpu_seconds") + cpu;
                c["exact_job_wall_seconds"] =
                    number(c, "exact_job_wall_seconds") + number(r, "job_wall_seconds");
                c["exact_peak_rss_kib"] = std::max(count(c, "exact_peak_rss_kib"), count(r, "peak_rss_kib"));
                if (status == "EXACT_PROGRESS" || status == "EXACT_DONE") {
                    require(job.value("partition_target", 0) <= 1 || count(r, "solved_nodes") == 0,
                            "Partition-only job cannot consume completed subproblems");

                    auto key = job["box_id"].get<std::string>();
                    if (job.value("partition_target", 0) > 1 && r["frontier"].size() == 1 &&
                        r["frontier"][0] == job["box"]) {
                        if (!c.contains("unsplittable"))
                            c["unsplittable"] = J::object();
                        c["unsplittable"][key] = true;
                    }
                    require(c["frontier"].contains(key) && c["frontier"][key] == job["box"],
                            "Stale/duplicate exact box result");
                    Z parent = integer(c["frontier_upper"].at(key)), n = integer(r["numerator"]);
                    require(n <= parent, "Chunk mass exceeds recorded upper bound");
                    c["frontier"].erase(key);
                    c["frontier_upper"].erase(key);
                    bool advanced =
                        integer(r["closed_volume"]) > 0 || r["frontier"] != J::array({job["box"]});
                    if (advanced)
                        c["attempts"].erase(key);
                    else
                        increment(c["attempts"], key);
                    for (std::size_t i = 0; i < r["frontier"].size(); ++i) {
                        auto box = r["frontier"][i];
                        auto child = digest(box);
                        require(!c["frontier"].contains(child), "Duplicate unresolved exact region");
                        c["frontier"][child] = box;
                        Z bound = integer(r["child_upper_numerators"][i]);
                        c["frontier_upper"][child] = std::min(bound, Z(parent - n)).get_str();
                    }
                    c["partial_numerator"] = Z(integer(c["partial_numerator"]) + n).get_str();
                    c["closed_volume"] =
                        Z(integer(c["closed_volume"]) + integer(r["closed_volume"])).get_str();
                    increment(c, "solved_nodes", count(r, "solved_nodes"));
                    increment(c, "zero_nodes", count(r, "zero_nodes"));
                    increment(c, "cache_hits", count(r, "component_cache_hits"));
                    Z hi = integer(c["partial_numerator"]);
                    for (auto &v : c["frontier_upper"])
                        hi += integer(v);
                    c["proven_total_upper"] =
                        std::min(hi,
                                 integer(c.value("proven_total_upper", c["root_bound"]["upper_numerator"])))
                            .get_str();
                    c["last_reason"] = r.value("reason", std::string());
                    c["next_at"] = 0;
                    if (c["frontier"].empty()) {
                        require(integer(c["closed_volume"]) == integer(c["root_volume"]),
                                "Exact family volume not closed");
                        finish_family(c);
                    }
                } else if (status == "EXACT_SLICE") {
                    auto k = str(job["slice"]);
                    require(!c["slices"].contains(k), "Duplicate static slice result");
                    if (c["plan"].is_null())
                        c["plan"] = r["plan"];
                    require(c["plan"] == r["plan"], "Static plan changed after accumulation");
                    require(r["slice"] == job["slice"], "Static slice identity mismatch");
                    c["slices"][k] = r["numerator"];
                    c["partial_numerator"] =
                        Z(integer(c["partial_numerator"]) + integer(r["numerator"])).get_str();
                    increment(c, "solved_nodes");
                    if (integer(r["numerator"]) == 0)
                        increment(c, "zero_nodes");
                    c["next_at"] = 0;
                    c["last_reason"] = nullptr;
                    if (c["slices"].size() == c["plan"]["slices"].get<std::uint64_t>())
                        finish_family(c);
                } else {
                    if (!cipher.cfg["adaptive"].get<bool>() && r.contains("plan") &&
                        r["plan"].contains("slices") && c["plan"].is_null())
                        c["plan"] = r["plan"];
                    auto k = job.contains("box_id") ? job["box_id"].get<std::string>() : str(job["slice"]);
                    increment(c["attempts"], k);
                    c["next_at"] = elapsed() + 1;
                    c["last_reason"] = r.value("reason", std::string("EXACT_INCOMPLETE"));
                }

                bounds(c);
                union_dirty_endpoints.insert(c.at("endpoint"));
                unions_dirty = true;
            }
            if (kind == "plan" || kind == "exact") {
                const auto key = resource_key(job);
                bool progressed = status == "PLAN" || status == "EXACT_SLICE" || status == "EXACT_DONE" ||
                                  (status == "EXACT_PROGRESS" && (integer(r["closed_volume"]) > 0 ||
                                                                  r["frontier"] != J::array({job["box"]})));
                const auto why = r.value("reason", status);
                bool memory = false;
                for (const auto &word : {"MEMORY", "ADDRESS_SPACE", "TABLE_PLAN_LIMIT", "bad_alloc"})
                    memory = memory || why.find(word) != std::string::npos;
                auto &needs = c["resource_required_tables"];
                if (needs.is_null())
                    needs = J::object();
                bool can_grow = false;
                if (progressed) {
                    needs.erase(key);
                } else if (memory) {
                    auto old = count(r, "table_bytes_used", Resources::minimum);
                    auto maximum = ram.maximum_table(
                         searchslots, a.job_ram_gib > 0 ? std::uint64_t(a.job_ram_gib * GiB) : 0);
                    auto next = std::min(maximum, std::max({sat_add(old, Resources::minimum), sat_mul(old, 2),
                                                            count(needs, key.c_str())}));
                    if (next > old) {
                        needs[key] = next;
                        can_grow = true;
                    }
                }

            }
        }
        if (job.contains("cid") && (kind == "screen" || kind == "plan" || kind == "exact")) {
            auto &c = cases.at(job.at("cid").get<std::string>());
            if (kind == "exact") union_dirty_endpoints.insert(c.at("endpoint"));
            bool screened = c.at("screen").is_object() && c.at("screen").contains("F") &&
                            c.at("screen").at("F").is_number();
            bool certified = integer(raw_mass(c).at("numerator")) > 0;
            if(kind=="exact"){

                Z gain=integer(c.at("partial_numerator"))-mass_before;
                require(gain>=0,"Certified family mass decreased while merging an exact receipt");
                auto certificate=dyadic(gain,exponent(c));double gain_F=number(certificate,"F",INFINITY);
                mass_work_feedback(c,gain_F,cpu,a.adaptive_window_jobs);

                auto &p=patterns.at(c.at("pid").get<std::string>());
                mass_work_feedback(p,gain_F,cpu,a.adaptive_window_jobs);dp.insert(p.at("id"));
            }
            auto &metrics=m["mass_engine_counters"];if(metrics.is_null())metrics=J::object();
            for(const auto *key:{"shared_message_hits","shared_message_stores","fine_bit_contractions"})increment(metrics,key,count(r,key));
            adaptive_feedback(patterns, dp, c, screened || certified ? replica_quality(c) : INFINITY,
                              screened, certified, cpu);
            dc.insert(c.at("id"));
        }
        auto id = str(job["id"]);
        db.exec("BEGIN IMMEDIATE");
        try {
            for (auto &k : dp)
                db.put("patterns", k, patterns.at(k));
            for (auto &k : dc)
                db.put("cases", k, cases.at(k));
            db.erase("jobs", id);
            m["elapsed"] = elapsed();
            db.put("meta", "state", m);
            db.exec("COMMIT");
            dp.clear();
            dc.clear();
            jobs.erase(id);
        } catch (...) {
            db.exec("ROLLBACK");
            throw;
        }
        if (r.value("status", std::string()) != "ERROR" && !r.value("unverified_native_checkpoint", false))
            cleanup_job(job);
        if (cipher.present) {
            for (auto it = m["pair_tasks"].begin(); it != m["pair_tasks"].end(); ++it) {
                auto &t = it.value();
                const auto ep = t["endpoint"].get<std::string>();
                bool resolved = t.value("done", false) || (m["corridor_certificates"].contains(ep) &&
                                                           m["corridor_certificates"][ep]["cap"] >= t["cap"]);
                bool busy = false;
                for (const auto &kv : active)
                    busy = busy || kv.second.job.value("pair_id", std::string()) == it.key();
                if (resolved && !busy) {
                    auto path = scratch / "pair" / it.key();
                    if (fs::exists(path) && !fs::is_symlink(path)) {
                        require(it.key().size() == 64 &&
                                    it.key().find_first_not_of("0123456789abcdef") == std::string::npos,
                                "Invalid pair scratch identity");
                        std::error_code ec;
                        fs::remove_all(path, ec);
                    }
                }
            }
        }
    }
    void finish_family(J &c) {
        Z n = integer(c["partial_numerator"]);
        require(n >= integer(c["witness_numerator"]), "Complete family omitted verified witness");
        c["exact"] = dyadic(n, exponent(c));
        c["exact_completed_seconds"] = elapsed();
        c["last_reason"] = nullptr;
        rank_dirty.insert(c["id"]);
        union_dirty_endpoints.insert(c.at("endpoint"));
        unions_dirty = true;
    }
    void cleanup_job(const J &job) {
        fs::path path = job["scratch"].get<std::string>();
        auto expected = scratch / "jobs" / str(job["id"]);
        if (fs::weakly_canonical(path) == fs::weakly_canonical(expected) && !fs::is_symlink(path)) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    void recover() {
        auto copy = jobs;
        for (auto &kv : copy) {
            auto j = kv.second;
            fs::path path = j["scratch"].get<std::string>();
            if (fs::exists(path / "receipt.json")) {
                consume(j, read_json(path / "receipt.json"));
                increment(m, "recovered_receipts");
            } else if (j["kind"] == "exact" && cipher.cfg["adaptive"].get<bool>() &&
                       fs::exists(path / "checkpoint.json")) {
                m["timing_has_unclean_session"] = true;
                auto copy = j;
                copy["recover_checkpoint"] = true;
                copy["deadline"] = std::numeric_limits<double>::infinity();
                auto state = std::make_shared<TaskState>();
                require(ram.attach(j["id"], state),
                        "Not enough RAM to verify retained checkpoint; all files kept for resume");
                auto result = workers.execute(copy, state);
                require(result["status"] == "EXACT_PROGRESS" || result["status"] == "EXACT_DONE",
                        "Checkpoint verification incomplete; original work is retained");
                consume(j, result);
                increment(m, "recovered_native_checkpoints");
            } else {
                m["timing_has_unclean_session"] = true;
                increment(m, "unfinished_jobs_rescheduled");

                record_pattern_interruption(m, j);
                record_binary_seed_interruption(m, j);
                record_common_seed_interruption(m, j);
                db.exec("BEGIN IMMEDIATE");
                try {
                    db.erase("jobs", kv.first);
                    db.put("meta", "state", m);
                    db.exec("COMMIT");
                } catch (...) {
                    db.exec("ROLLBACK");
                    m = db.load("meta").at("state");
                    throw;
                }
                jobs.erase(kv.first);
                cleanup_job(j);
            }
        }
        flush();
    }
    void collect(unsigned maximum = UINT_MAX) {
        unsigned consumed = 0;
        for (auto it = active.begin(); it != active.end();) {
            if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto job = it->second.job;
            const auto &state = it->second.state;
            if (state->phase.load() == 4)
                m["maximum_receipt_wait_seconds"] = std::max(number(m, "maximum_receipt_wait_seconds"),
                                                            now() - state->phase_started.load());
            auto result = it->second.future.get();
            it = active.erase(it);
            consume(job, result);
            if (++consumed >= maximum) break;
        }
    }

    void publish(bool terminal = false);
    int execute();
};
}
