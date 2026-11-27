#pragma once
#include "runtime.hpp"
#include "binary_seed.hpp"
#include "common_value_seed.hpp"
#include "support_transfer.hpp"
#include "activity_replica.hpp"
namespace thermo {
inline J interrupted_search_result(const ProcessResult &p) {

    const bool budget = p.code == 2 && p.err.rfind("INCOMPLETE:", 0) == 0;
    if (p.code > 0 && !budget)
        throw std::runtime_error("Search engine: " + p.err);
    J result{{"status", "INCOMPLETE"},
             {"reason", budget ? "NATIVE_BUDGET_OR_SIGNAL" : "NATIVE_INTERRUPTED"},
             {"native_exit_code", p.code}};
    if (budget)
        result["native_message"] = p.err;
    return result;
}
inline void write_plain(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    require(bool(f), "Cannot create native input");
    f << s;
    f.close();
    require(bool(f), "Cannot finish native input");
}
inline J parallel_partition(const Family &f, const J &parent, int target, const Deadline &dl) {
    auto initial = box_domains(f, parent);
    std::vector<Domains> leaves = {initial};
    std::vector<int> degrees(f.domains.size());
    for (auto &g : f.factors)
        for (int v : Cipher::scope(g))
            ++degrees[v];
    while (int(leaves.size()) < target) {
        dl.check();
        int index = -1;
        Z largest = -1;
        for (int i = 0; i < int(leaves.size()); ++i) {
            bool can = false;
            for (auto &d : leaves[i])
                if (d.size() > 1)
                    can = true;
            Z volume = product(leaves[i]);
            if (can && (volume > largest || (volume == largest && i > index))) {
                index = i;
                largest = volume;
            }
        }
        if (index < 0)
            break;
        auto dom = leaves[index];
        leaves.erase(leaves.begin() + index);
        int var = -1;
        for (int v = 0; v < int(dom.size()); ++v)
            if (dom[v].size() > 1 && (var < 0 || std::make_tuple(degrees[v], dom[v].size(), -v) >
                                                     std::make_tuple(degrees[var], dom[var].size(), -var)))
                var = v;
        auto left = dom, right = dom;
        auto mid = dom[var].begin() + dom[var].size() / 2;
        left[var] = {dom[var].begin(), mid};
        right[var] = {mid, dom[var].end()};
        leaves.push_back(std::move(left));
        leaves.push_back(std::move(right));
    }
    J boxes = J::array();
    if (leaves.size() == 1)
        boxes.push_back(parent);
    else
        for (auto &d : leaves) {
            J box = J::array();
            for (int v = 0; v < int(d.size()); ++v)
                if (d[v] != f.domains[v])
                    box.push_back({v, mask_text(d[v])});
            boxes.push_back(box);
        }
    return J{{"status", "EXACT_PROGRESS"},
             {"reason", "PARALLEL_PARTITION"},
             {"numerator", "0"},
             {"denominator_power", f.exponent},
             {"closed_volume", "0"},
             {"initial_volume", product(initial).get_str()},
             {"frontier", boxes},
             {"solved_nodes", 0},
             {"zero_nodes", 0},
             {"splits", leaves.size() - 1},
             {"component_cache_hits", 0}};
}
class Workers {
    const Cipher &c;
    Resources &ram;
    fs::path root;
    Cache<Model> models{256};
    Cache<Family> families{96};
    Cache<CommonCatalog> common_catalogs{1};
    Cache<CommonKernel> common_kernels{1};
    Symmetry symmetry;

  public:
    std::shared_ptr<SupportTransfer> support;
    std::string support_fallback="disabled by option";
    Workers(const Cipher &cipher, Resources &r, fs::path variant)
        : c(cipher), ram(r), root(std::move(variant)), symmetry(cipher) {}
    std::shared_ptr<Model> model(const Pattern &p, const Deadline &d) {
        return models.get(digest(p), d, [&]() { return c.compile(p, d); });
    }
    J pattern_record(const Pattern &p, const Deadline &d) {
        auto m = model(p, d);
        return J{{"id", m->id},
                 {"pattern", p},
                 {"active", m->active},
                 {"dimension", m->nv},
                 {"orbit_id", digest(symmetry.canonical_pattern(p))}};
    }
    J hit(const std::vector<std::string> &states, const Deadline &dl, double first) {
        auto sy = symmetry.canonical(states);
        auto p = c.activity(sy.first);
        auto m = model(p, dl);
        auto w = c.encode_witness(*m, sy.first);
        auto ep = sy.first.front() + "_" + sy.first.back();
        return J{{"states", sy.first},
                 {"symmetry", sy.second},
                 {"pattern_record", pattern_record(p, dl)},
                 {"id", digest(J::array({ep, p}))},
                 {"endpoint", ep},
                 {"input", sy.first.front()},
                 {"output", sy.first.back()},
                 {"weight", w["weight"]},
                 {"witness_numerator", w["witness_numerator"]},
                 {"first_seconds", first}};
    }
    std::shared_ptr<Family> family(const J &job, const Deadline &d) {
        auto states = job.at("states").get<std::vector<std::string>>();
        auto p = c.activity(states);
        auto key = digest(J::array({states.front(), states.back(), p}));
        return families.get(key, d, [&]() { return c.family(states, d); });
    }
    J certificate(const J &row, const Deadline &dl) {
        int cap = std::stoi(str(row.at("cap")));
        auto states = row.at("states").is_array() ? row.at("states").get<std::vector<std::string>>()
                                                  : split(row.at("states").get<std::string>(), ',');
        require(cap >= 1 && cap <= c.N && int(states.size()) == c.R + 1, "Invalid transfer certificate scope");
        require(states.front() == row.at("input") && states.back() == row.at("output"),
                "Transfer endpoint mismatch");
        auto pat = c.activity(states);
        int a0 = int(std::count(pat[0].begin(), pat[0].end(), '1'));
        for (int r = 1; r < c.R; ++r)
            require(int(std::count(pat[r].begin(), pat[r].end(), '1')) <= cap,
                    "Transfer witness outside cap");
        unsigned e = 3 * a0 + 3 * cap * (c.R - 1);
        require(std::stoul(str(row.at("denominator_power"))) == e, "Transfer denominator mismatch");
        auto h = hit(states, dl, 0);
        Z witness = integer(h.at("witness_numerator"));
        unsigned we = 3 * h["pattern_record"]["active"].get<unsigned>();
        Z num = integer(row.at("numerator"));
        require(e >= we && num >= (witness << (e - we)), "Transfer excluded witness mass");
        return J{
            {"input", states.front()},
            {"output", states.back()},
            {"endpoint", states.front() + "_" + states.back()},
            {"states", states},
            {"cap", cap},
            {"mass", dyadic(num, e)},
            {"witness_weight", h["weight"]},
            {"exact", true},
            {"scope", "all trails with internal per-round activity <= cap; final linear layer included"}};
    }
    J common_seed(const J &job, const Deadline &dl, double started) {
        J out{{"kind","seed"},{"status","INCOMPLETE"},{"hits",J::array()},
              {"common_seed_progress",job.at("common_seed_progress")}};
        std::optional<CommonRow> row;
        unsigned keep=job.at("keep");
        try {
            const auto campaign_seed=job.at("common_seed_campaign_seed").get<std::uint64_t>();
            auto catalog=common_catalogs.get(std::to_string(campaign_seed),dl,[&](){
                return CommonCatalog(c,campaign_seed,dl);
            });
            auto found=catalog->by_active.find(job.at("active").get<int>());
            if(found==catalog->by_active.end()) {
                out["status"]="COMPLETE";out["common_seed_no_support"]=true;
                out["common_seed_supports"]=0;return out;
            }
            const auto &supports=found->second;
            auto coordinate=common_coordinate(job.at("common_seed_cursor"),supports.size(),c.Q,campaign_seed);
            auto masks=common_masks(c,supports.at(coordinate.first));
            std::vector<int> profile;
            for(int r=0;r<c.R;++r)profile.push_back(__builtin_popcount(unsigned(masks[r])));
            require(std::accumulate(profile.begin(),profile.end(),0)==job.at("active").get<int>(),
                    "Common seed active count mismatch");
            auto kernel=common_kernels.get("skinny",dl,[&](){return CommonKernel::skinny(c);});
            auto identity=digest(J::array({"SKINNY_COMMON_ROW_V1",digest(c.cfg),masks,coordinate.second,
                                          job.at("common_seed_cursor")}));
            const auto &progress=job.at("common_seed_progress");
            if(progress.is_object())keep=progress.at("keep").get<unsigned>();
            require(keep>0,"Empty common seed archive selection");
            row.emplace(*kernel,profile,coordinate.second,identity,progress);
            out["common_seed_supports"]=supports.size();
            while(!row->complete())row->step(dl);
            auto outputs=row->outputs(keep);
            require(row->next_output<=outputs.size(),"Common seed output cursor mismatch");
            const unsigned exponent=c.DB*std::accumulate(profile.begin(),profile.end(),0);
            for(;row->next_output<outputs.size();) {
                dl.check();
                const int v=outputs[row->next_output];

                auto sample_seed=std::stoull(digest(J::array({campaign_seed,identity,v})).substr(0,16),nullptr,16);
                std::mt19937_64 rng(sample_seed);
                auto values=row->sample(v,rng,dl);
                std::vector<std::string> states;
                for(int r=0;r<=c.R;++r) {
                    std::vector<int> x(16);
                    for(int i=0;i<16;++i)x[i]=(masks[r]&(1<<i))?values[r]:0;
                    states.push_back(c.encode(x));
                }
                auto h=hit(states,dl,now()-started);
                require(h.at("pattern_record").at("active")==job.at("active") &&
                        integer(h.at("witness_numerator"))<=row->prefix.back()[v],
                        "Sampled common-value witness outside summed family");
                h["common_value_seed"]={{"schema","SKINNY_COMMON_FAMILY_V1"},
                    {"mass",dyadic(row->prefix.back()[v],exponent)},
                    {"round_active_counts",profile},{"input_value",coordinate.second},{"output_value",v},
                    {"initial_support_before_canonicalization",supports.at(coordinate.first)},
                    {"row_identity",identity},{"row_cursor",job.at("common_seed_cursor")},
                    {"scope","all per-round common-value sequences in this support at these endpoints"},
                    {"ordinary_exact_overlap","ranking certificate only; never add to ordinary partial sums"}};
                out["hits"].push_back(std::move(h));
                ++row->next_output;
            }
            out["status"]="COMPLETE";out["common_seed_row_complete"]=true;
            out["common_seed_progress"]=nullptr;
        } catch(const Cancelled &e) {
            out["reason"]=e.what();
            if(row) {out["common_seed_progress"]=row->checkpoint();out["common_seed_progress"]["keep"]=keep;}
        }
        return out;
    }
    J execute(J job, const std::shared_ptr<TaskState> &state) {
        double started = now(), cpu_start = thread_cpu(), prep_end = started, verify_start = started;
        ProcessResult proc;
        bool invoked = false,inline_compute=false;
        J result{{"kind", job.at("kind")}, {"status", "INCOMPLETE"}};
        fs::path dir = job.at("scratch").get<std::string>();
        Deadline dl{job.at("deadline").get<double>()};
        state->set(1);
        try {
            fs::create_directories(dir);
            std::string kind = job.at("kind");
            std::vector<std::string> args;
            std::shared_ptr<Family> fp;
            std::shared_ptr<Model> mp;
            std::vector<std::shared_ptr<Model>> joint_members;
            JointCatalog joint_catalog;
            J witness;
            auto add = [&](const std::string &a, const J &b) {
                args.push_back(a);
                args.push_back(b.is_string() ? b.get<std::string>() : b.dump());
            };
            auto native_memory = [&](std::uint64_t desired, std::uint64_t required = 0) {
                std::uint64_t table = 0;
                state->set(5);
                required = std::max(required, job.value("table_required", std::uint64_t(0)));
                if (job.value("ram_pre_admitted", false)) {
                    table = state->table.load();
                    require(table >= std::max(Resources::minimum, required), "Pre-admitted table lease changed");
                    state->set(1);
                    job["table_bytes"] = table;
                    job["address_bytes"] = table + Resources::overhead;
                    result["table_bytes_used"] = table;
                    return table;
                }
                if (!ram.native(state, desired, required, job.value("table_cap", std::uint64_t(0)), table))
                    throw Cancelled("WAITING_FOR_EXACT_RAM");
                state->set(1);
                job["table_bytes"] = table;
                job["address_bytes"] = table + Resources::overhead;
                result["table_bytes_used"] = table;
                return table;
            };
            if (kind == "pattern_dp") {
                require(bool(support),"Support DP not configured");
                prep_end=now();inline_compute=true;state->set(2);
                result.update(support->build(job.at("layer"),job.at("count"),dl));
                verify_start=now();state->set(3);
            } else if (kind=="pattern" && job.value("generator",std::string())=="transfer") {
                require(support && support->ready(),"Support transfer table not ready");
                result["patterns"]=J::array();result["invalid_patterns"]=0;
                result["generator"]="transfer";
                auto n=job.at("active").get<unsigned>();
                result["compatible_sequence_count"]=support->total(n).decimal();
                std::mt19937_64 rng(job.at("seed").get<std::uint64_t>());
                prep_end=now();inline_compute=true;state->set(2);
                std::vector<Pattern> proposed;
                try {
                    for(unsigned i=0;i<job.at("keep").get<unsigned>();++i) {
                        auto masks=support->sample(n,rng,dl);if(masks.empty())break;
                        Pattern p;for(auto mask:masks){std::string s;for(int b=0;b<16;++b)s+=(mask&(1u<<b))?'1':'0';p.push_back(s);}
                        proposed.push_back(symmetry.canonical_pattern(p));
                    }
                } catch(const Cancelled &e) {result["reason"]=e.what();}
                verify_start=now();state->set(3);
                for(const auto &p:proposed)try {auto rec=pattern_record(p,{});rec["discovery_generator"]="transfer";result["patterns"].push_back(std::move(rec));}
                    catch(const std::runtime_error &e) {
                        std::string why=e.what();
                        if(why!="Active cell forced zero" && why!="Active S-box forced zero" &&
                           why!="Active nibble has no incoming bits" && why!="Active output forced zero" &&
                           why!="Pattern has empty DDT support")throw;
                        result["invalid_patterns"]=result["invalid_patterns"].get<unsigned>()+1;
                    }
                result["support_draws"]=proposed.size();result["status"]="COMPLETE";
            } else if (kind=="activity") {
                prep_end=now();inline_compute=true;state->set(2);
                result.update(activity_walk::run(c,job,dl));
                verify_start=now();state->set(3);result["hits"]=J::array();
                for(const auto &h:result.at("activity_raw_hits"))
                    result["hits"].push_back(hit(h.at("states").get<std::vector<std::string>>(),{},prep_end-started+h.at("first_seconds").get<double>()));

                for(const auto &ss:result.at("activity_resume"))
                    hit(ss.get<std::vector<std::string>>(),{},0);
                result.erase("activity_raw_hits");
            } else if (kind == "import") {
                result["hits"] = J::array();
                for (const auto &s : job.at("witnesses")) {
                    dl.check();
                    result["hits"].push_back(hit(s.get<std::vector<std::string>>(), dl, 0));
                }
                result["status"] = "COMPLETE";
            } else if (is_common_seed(job)) {
                result=common_seed(job,dl,started);
                prep_end=verify_start=now();
            } else if (kind == "seed" && job.value("seed_kind", std::string()) == "binary") {
                auto starts = midori_binary_witnesses(c, job.at("counts").get<std::vector<int>>(),
                    job.at("seed"), job.at("keep"), dl);
                result["hits"] = J::array();
                for (const auto &states : starts) {
                    dl.check();

                    result["hits"].push_back(hit(states, dl, now() - started));
                }
                result["diagnostics"] = {{"seed_method", "independent_binary_supports"},
                    {"enumerated_supports", 65535}, {"all_equal_values_are_only_initialization", true}};
                result["status"] = "COMPLETE";
            } else if (kind == "exact" || kind == "plan") {
                fp = family(job, dl);
                Family f = *fp;
                auto states = job.at("states").get<std::vector<std::string>>();
                if (f.states != states) {
                    auto w = c.encode_witness(f.model, states);
                    auto x = c.digits(w["coordinates"].get<std::string>() +
                                      w["witness_internal"].get<std::string>());
                    f.witness = {x.begin() + f.model.nend, x.end()};
                    f.witness_num = integer(w["witness_numerator"]);
                    f.weight = w["weight"];
                    f.states = states;
                }
                result["root_bound"] = f.root_bound;
                result["witness_weight"] = f.weight;
                if (job.value("recover_checkpoint", false)) {
                    result.update(read_json(dir / "checkpoint.json"));
                    result["reason"] = "RECOVERED_DURABLE_CHECKPOINT";
                } else if (job.value("partition_target", 0) > 1) {
                    result.update(parallel_partition(f, job.at("box"), job.at("partition_target"), dl));
                } else if (c.cfg.at("adaptive").get<bool>()) {
                    auto table = native_memory(job.at("table_desired"));
                    write_plain(dir / "exact.txt", c.exact_text(f, J::array({job.value("box", J::array())})));
                    args = {(root / "bin/exact").string(),
                            kind == "plan" ? "plan" : "eval",
                            (dir / "exact.txt").string(),
                            (dir / "checkpoint.json").string(),
                            std::to_string(table + Resources::overhead + mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0)),
                            std::to_string(table),
                            str(job.at("leaf_operations")),
                            std::to_string(dl.remaining()),
                            str(job.at("max_frontier")),
                            "2"};
                    prep_end = now();
                    invoked = true;
                    proc = invoke(args, dir, dl, state);
                    verify_start = now();
                    state->set(3);
                    if (proc.code == 0)
                        result.update(J::parse(proc.out));
                    else if (kind == "exact" && fs::exists(dir / "checkpoint.json")) {
                        result.update(read_json(dir / "checkpoint.json"));
                        result["reason"] = "RECOVERED_DURABLE_CHECKPOINT";
                    } else if (proc.code < 0 || proc.err.find("TIME_LIMIT") != std::string::npos ||
                               proc.err.find("bad_alloc") != std::string::npos)
                        result["reason"] = proc.err;
                    else
                        throw std::runtime_error("Exact engine: " + proc.err);
                } else {
                    J plan = job.value("plan", J());
                    auto required =
                        plan.is_null() ? 0 : plan.at("estimated_table_bytes").get<std::uint64_t>();
                    auto table = native_memory(job.at("table_desired"), required);
                    if (plan.is_null())
                        plan = make_plan(f, table, job.at("max_slices"), dl);
                    result["plan"] = plan;
                    if (plan.value("status", std::string()) == "RESOURCE_DEFERRED")
                        result.update(plan);
                    else {
                        result["bound_slice_volume"] =
                            product(slice_domains(f, plan, job.at("slice"))).get_str();
                        write_plain(dir / "exact.txt", c.exact_text(f, J::array(), plan));
                        args = {(root / "bin/exact").string(), (dir / "exact.txt").string(),
                                str(job.at("slice")),          std::to_string(table + Resources::overhead + mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0)),
                                std::to_string(table),         std::to_string(dl.remaining())};
                        prep_end = now();
                        invoked = true;
                        proc = invoke(args, dir, dl, state);
                        verify_start = now();
                        state->set(3);
                        if (proc.code == 0) {
                            result.update(J::parse(proc.out));
                        } else if (proc.code < 0)
                            result["reason"] = "STATIC_SLICE_INTERRUPTED";
                        else
                            throw std::runtime_error("Static exact engine: " + proc.out + proc.err);
                    }
                }
                state->set(3);
                auto status = result.at("status").get<std::string>();
                if (status == "PLAN") {
                    require(integer(result.at("initial_volume")) == product(f.domains),
                            "Native plan domain volume mismatch");
                    require(result.at("operation_score").is_number_unsigned() &&
                                result.at("estimated_table_bytes").is_number_unsigned() &&
                                result.at("fits_leaf_budget").is_boolean(),
                            "Invalid native cost plan");
                } else if (status == "EXACT_PROGRESS" || status == "EXACT_DONE") {
                    auto parent = job.value("box", J::array());
                    verify_partition(f, parent, result);
                    auto parent_info =
                        parent.empty() ? f.root_bound : c.upper_bound(f, box_domains(f, parent));
                    Z n = integer(result.at("numerator")), bound = integer(parent_info["upper_numerator"]);
                    require(n <= bound, "Exact chunk exceeds parent bound");
                    J bs = J::array();
                    for (auto &b : result.at("frontier")) {
                        Z hi = integer(parent_info["factor_ceiling"]) * product(box_domains(f, b));
                        if (hi > bound - n)
                            hi = bound - n;
                        bs.push_back(hi.get_str());
                    }
                    result["child_upper_numerators"] = bs;
                    auto solved = result.at("solved_nodes").get<std::uint64_t>(),
                         zero = result.at("zero_nodes").get<std::uint64_t>();
                    require(zero <= solved,
                            "Exact subproblem count mismatch");
                } else if (status == "EXACT_SLICE") {
                    require(result["denominator_power"].get<unsigned>() == f.exponent,
                            "Static denominator mismatch");
                    require(integer(result["numerator"]) >= 0, "Negative exact slice");
                }
            } else if (kind == "pair") {
                auto table = native_memory(job.at("table_desired"));
                auto ep = split(job.at("endpoint"), '_');
                auto work = job.at("work").get<std::string>();
                fs::create_directories(work);
                args = {(root / "bin/pair").string(),
                        "--input",
                        ep.at(0),
                        "--output",
                        ep.at(1),
                        "--cap",
                        str(job.at("cap")),
                        "--threads",
                        str(job.at("cpu_cores")),
                        "--seconds",
                        std::to_string(dl.remaining()),
                        "--nodes",
                        std::to_string(std::max<std::uint64_t>(128, table / (256 + 12 * ((3*c.N*c.R+32)/32)))),
                        "--memory",
                        std::to_string(table + Resources::overhead + mass_messages::setting("THERMO_MESSAGE_CACHE_BYTES",0)),
                        "--token",
                        str(job.at("id")),
                        "--work",
                        work,
                        "--out",
                        (dir / "pair.json").string()};
                prep_end = now();
                invoked = true;
                proc = invoke(args, dir, dl, state, 120);
                verify_start = now();
                state->set(3);
                auto path =
                    fs::exists(dir / "pair.json") ? dir / "pair.json" : fs::path(work) / "progress.json";
                if (fs::exists(path)) {
                    J p = read_json(path);
                    if (p.value("token", std::uint64_t(0)) == job.at("id").get<std::uint64_t>()) {
                        require(p["input"] == ep[0] && p["output"] == ep[1] && p["cap"] == job["cap"],
                                "Pair receipt mismatch");
                        result["pair_progress"] = p;
                        result["status"] = p["status"];
                        result["reason"] = p.value("reason", std::string());
                        if (p.value("probability_complete", false)) {
                            J row = p;
                            row["states"] = job["states"];
                            result["corridor_records"] = J::array({certificate(row, {})});
                        }
                    }
                }
                if (result.at("status") == "INCOMPLETE")
                    result["reason"] = proc.err.empty() ? "PAIR_CHECKPOINT_PENDING" : proc.err;
                if (proc.code > 0 && proc.err.find("RAM") == std::string::npos &&
                    proc.err.find("bad_alloc") == std::string::npos &&
                    proc.err.find("MERGE_ABORT") == std::string::npos &&
                    proc.err.find("WORK_DIRECTORY_BUSY") == std::string::npos)
                    throw std::runtime_error("Pair engine: " + proc.err);
            } else {
                fs::path out = dir / "result.tsv";
                args = {(root / "bin/search").string(),
                        kind == "screen" ? "ais" : kind,
                        "--out",
                        out.string(),
                        "--seed",
                        str(job.at("seed")),
                        "--limit",
                        str(job.at("keep"))};
                if (kind == "pattern" || kind == "seed") {
                    add("--active", job.at("active"));
                    add("--sweeps", 1000000000);
                    if(kind=="seed" && job.value("particle_seed",false)){
                        add("--particle-seed",1);add("--center",job.at("cap"));add("--width",job.at("input_active"));
                        add("--paths",job.at("particles"));add("--seed-transition-work",job.at("seed_work"));
                        add("--seed-bidirectional",job.value("seed_bidirectional",false)?1:0);add("--seed-lookahead-work",job.value("seed_lookahead_work",std::uint64_t(0)));
                    }
                } else if (kind == "search" || kind == "screen") {
                    mp = model(job.at("pattern").get<Pattern>(), dl);
                    if (kind == "search" && job.contains("joint_members")) {
                        std::ostringstream initial;
                        const auto &members = job.at("joint_members");
                        require(members.size() >= 2, "Joint search needs at least two members");
                        initial << "THERMO_JOINT_WITNESSES_V1 " << members.size() << '\n';
                        std::string endpoints;
                        for (std::size_t i = 0; i < members.size(); ++i) {
                            auto mm = model(members[i].at("pattern").get<Pattern>(), dl);
                            require(mm->id == members[i].at("pid"), "Joint pattern identity mismatch");
                            auto ww = c.encode_witness(*mm, members[i].at("states").get<std::vector<std::string>>());
                            std::string ep = ww.at("coordinates");
                            if (i == 0) endpoints = ep;
                            require(ep == endpoints, "Joint initialization endpoints differ");
                            std::string in = ww.at("witness_internal");
                            initial << i << ' ' << ep << ' ' << (in.empty() ? "-" : in) << '\n';
                            joint_members.push_back(std::move(mm));
                        }
                        joint_catalog = c.native_catalog(joint_members);
                        write_plain(dir / "model.txt", joint_catalog.text);
                        write_plain(dir / "joint_initial.txt", initial.str());
                        add("--bridge", (dir / "joint_initial.txt").string());
                        result["joint_patterns"] = joint_members.size();
                        result["joint_target"] = "(sum of distinct catalog-pattern Z(e))^copies";
                    } else
                        write_plain(dir / "model.txt", c.native_model(*mp));
                    add("--model", (dir / "model.txt").string());
                    if (job.contains("states") && !job["states"].is_null())
                        witness = c.encode_witness(*mp, job["states"].get<std::vector<std::string>>());
                    if (kind == "search") {
                        add("--copies", job.at("copies"));
                        add("--sweeps", 1000000000);
                        if (!witness.is_null() && joint_members.empty())
                            add("--initial", witness["coordinates"].get<std::string>() +
                                                 witness["witness_internal"].get<std::string>());
                    } else {
                        fp = family(job, dl);
                        write_plain(dir / "domains.txt", c.native_domains(*fp));
                        add("--endpoint", fp->endpoint_coordinates);
                        add("--pattern", 0);
                        add("--domains", (dir / "domains.txt").string());
                        add("--paths", job.at("paths"));
                        add("--steps", job.at("steps"));
                    }
                } else if (kind == "expand") {
                    std::ostringstream s;
                    s << c.cfg.at("bridge_magic").get<std::string>() << '\n';
                    for (auto &row : job.at("states"))
                        s << row.get<std::string>() << '\n';
                    write_plain(dir / "bridge.txt", s.str());
                    add("--bridge", (dir / "bridge.txt").string());
                    add("--center", job.at("center"));
                    add("--width", 8);
                    add("--sweeps", 1000000000);
                } else if (kind == "ensemble") {
                    std::ostringstream s;
                    s << "GIFT_ENSEMBLE_V1 " << c.R << ' ' << job.at("initial_trails").size() << '\n';
                    for (auto &ss : job.at("initial_trails"))
                        for (auto &v : ss)
                            s << v.get<std::string>() << '\n';
                    write_plain(dir / "ensemble.txt", s.str());
                    add("--bridge", (dir / "ensemble.txt").string());
                    add("--copies", job.at("copies"));
                    add("--active", job.at("active"));
                    add("--sweeps", 1000000000);
                } else if (kind == "corridor") {
                    auto table = native_memory(job.at("table_desired"));
                    add("--center", job.at("cap"));
                    add("--width", job.at("input_active"));
                    add("--copies", job.at("copies"));
                    add("--corridor-nodes", job.value("max_nodes", std::uint64_t(0))
                                                ? job["max_nodes"]
                                                : J(std::max<std::uint64_t>(128, table / (256 + 8 * ((3*c.N*c.R+32)/32)))));

                    auto edges = job.at("max_edges").get<std::uint64_t>();
                    add("--sweeps", edges ? edges : std::numeric_limits<std::uint64_t>::max());
                    add("--start-active", job.value("start_active", 0));
                    add("--memory-bytes", table + Resources::overhead);
                    if (job.contains("initial") && !job["initial"].is_null())
                        add("--initial", job["initial"]);
                } else
                    throw std::runtime_error("Unknown native task " + kind);
                add("--seconds", dl.remaining());
                prep_end = now();
                invoked = true;
                proc = invoke(args, dir, dl, state);
                verify_start = now();
                state->set(3);
                if (kind == "corridor" && proc.code < 0 &&
                    fs::exists(fs::path(out.string() + ".checkpoint"))) {
                    out += ".checkpoint";
                    proc.code = 0;
                    result["reason"] = "RECOVERED_COMPLETED_ROWS";
                }
                if (proc.code != 0)
                    result.update(interrupted_search_result(proc));
                if (proc.code == 0 && fs::exists(out)) {
                    auto rows = read_table(out);
                    if(kind=="seed" && job.value("particle_seed",false) && fs::exists(fs::path(out.string()+".seed.json")))
                        result["seed_diagnostics"]=read_json(fs::path(out.string()+".seed.json"));
                    result["status"] = "COMPLETE";
                    if (kind == "pattern") {
                        result["patterns"] = J::array();
                        result["invalid_patterns"] = 0;
                        for (auto &row : rows) {
                            try {
                                result["patterns"].push_back(pattern_record(
                                    symmetry.canonical_pattern(split(row.at("pattern"), ',')), {}));
                            } catch (const std::runtime_error &e) {
                                std::string why = e.what();
                                if (why != "Active cell forced zero" && why != "Active S-box forced zero" &&
                                    why != "Active nibble has no incoming bits" &&
                                    why != "Active output forced zero" &&
                                    why != "Pattern has empty DDT support")
                                    throw;
                                result["invalid_patterns"] = result["invalid_patterns"].get<int>() + 1;
                            }
                        }
                    } else if (kind == "corridor") {
                        result["corridor_records"] = J::array();
                        result["hits"] = J::array();
                        for (auto &row : rows) {
                            auto cert = certificate(row, {});
                            require(cert["cap"] == job["cap"], "Corridor cap receipt mismatch");
                            cert["first_seconds"] =
                                prep_end - started + std::stod(row.at("first_seconds").get<std::string>());
                            result["corridor_records"].push_back(cert);
                            result["hits"].push_back(hit(cert["states"].get<std::vector<std::string>>(), {},
                                                         cert["first_seconds"]));
                        }
                    } else if (kind == "screen") {
                        require(rows.size() == job.at("paths").get<std::size_t>(), "Incomplete AIS batch");
                        std::vector<long double> logs;
                        for (std::size_t i = 0; i < rows.size(); ++i) {
                            auto &row = rows[i];
                            require(std::stoull(row.at("path_id").get<std::string>()) == i &&
                                        row.at("endpoint") == fp->endpoint_coordinates &&
                                        std::stoi(row.at("pattern").get<std::string>()) == 0,
                                    "AIS receipt mismatch");
                            auto x = c.digits(row.at("endpoint").get<std::string>() +
                                              row.at("internal").get<std::string>());
                            require(x.size() == mp->labels.size(), "AIS coordinate size");
                            for (int v = 0; v < mp->nend; ++v)
                                require(x[v] == c.digits(fp->endpoint_coordinates)[v],
                                        "AIS endpoint changed");
                            for (int v = 0; v < mp->nv; ++v)
                                require(std::find(fp->domains[v].begin(), fp->domains[v].end(),
                                                  x[v + mp->nend]) != fp->domains[v].end(),
                                        "AIS coordinate outside prepared domain");
                            Z n = 1;
                            for (auto &f : mp->factors)
                                n *= c.entry(f.type, Cipher::eval(f.av, x), Cipher::eval(f.bv, x));
                            double tw = n == 0 ? -1 : c.DB * mp->active - log2z(n),
                                   rw = std::stod(row.at("trail_weight").get<std::string>());
                            long double w = std::stold(row.at("log_weight").get<std::string>());
                            require(std::abs(tw - rw) <= 1e-7 && !std::isnan(w) &&
                                        w != std::numeric_limits<long double>::infinity() &&
                                        ((n == 0) == (w == -std::numeric_limits<long double>::infinity())),
                                    "AIS terminal audit mismatch");
                            logs.push_back(w);
                        }
                        auto top = *std::max_element(logs.begin(), logs.end());
                        long double sum = 0, sum2 = 0, maxw = 0;
                        int positive = 0;
                        if (std::isfinite(top))
                            for (auto w : logs) {
                                auto v = std::isfinite(w) ? std::exp(w - top) : 0.0L;
                                sum += v;
                                sum2 += v * v;
                                maxw = std::max(maxw, v);
                                positive += v > 0;
                            }
                        result["F"] =
                            sum > 0 ? J(double(-(top + std::log(sum / logs.size())) / std::log(2.0L))) : J();
                        result["ess"] = sum2 > 0 ? double(sum * sum / sum2) : 0;
                        result["paths"] = logs.size();
                        result["positive"] = positive;
                        result["max_weight_fraction"] = sum > 0 ? double(maxw / sum) : 0;
                        result["sampling_is_only_screening"] = true;
                    } else {
                        result["hits"] = J::array();
                        for (auto &row : rows) {
                            std::vector<std::string> ss;
                            if (kind == "search") {
                                std::string internal = row.at("internal");
                                auto selected = mp;
                                if (!joint_members.empty()) {
                                    int p = std::stoi(row.at("pattern").get<std::string>());
                                    require(p >= 0 && p < int(joint_members.size()), "Joint receipt pattern index");
                                    selected = joint_members.at(p);
                                    internal = c.joint_internal(joint_catalog, p, internal);
                                } else
                                    require(std::stoi(row.at("pattern").get<std::string>()) == 0, "Search pattern index");
                                auto decoded = c.decode(*selected, row.at("endpoint"), internal);
                                const double checked_weight=decoded.at("weight").get<double>();
                                const double reported_weight=std::stod(row.at("trail_weight").get<std::string>());
                                if (!std::isfinite(reported_weight) || std::abs(checked_weight-reported_weight)>=1e-7) {
                                    std::ostringstream why;
                                    why << std::setprecision(17) << "Native witness weight mismatch: recomputed="
                                        << checked_weight << ", reported=" << reported_weight
                                        << ", pattern=" << selected->id << ", coordinates=" << row.at("endpoint")
                                        << ", internal=" << internal << ", joint=" << !joint_members.empty();
                                    throw std::runtime_error(why.str());
                                }
                                ss = decoded["states"].get<std::vector<std::string>>();
                            } else
                                ss = split(row.at("states"), ',');
                            result["hits"].push_back(hit(
                                ss, {},
                                prep_end - started + std::stod(row.at("first_seconds").get<std::string>())));
                        }
                    }
                    auto diag =
                        fs::path((dir / "result.tsv").string() + (kind == "corridor"   ? ".corridor.json"
                                                                  : kind == "ensemble" ? ".ensemble.json"
                                                                  : kind == "search"   ? ".search.json"
                                                                                       : ".expand.json"));
                    if (fs::exists(diag))
                        result["diagnostics"] = read_json(diag);
                } else if (proc.code == 0)
                    result["reason"] = "NATIVE_INTERRUPTED";
            }
        } catch (const Cancelled &e) {

            result = J{{"kind", job.at("kind")}, {"status", "INCOMPLETE"}, {"reason", e.what()}};
        } catch (const std::bad_alloc &) {
            if(job.at("kind")=="pattern_dp" && support)support->disable("RAM allocation failed; legacy pattern generator remains active");
            result = J{{"kind", job.at("kind")},
                       {"status", "INCOMPLETE"},
                       {"reason", "PREPARATION_OR_VERIFICATION_MEMORY_LIMIT"}};
        } catch (const std::exception &e) {
            result = J{{"kind", job.at("kind")}, {"status", "ERROR"}, {"reason", e.what()}};
        }
        if ((result["status"] == "INCOMPLETE" || result["status"] == "ERROR") &&
            fs::exists(dir / "checkpoint.json"))
            result["unverified_native_checkpoint"] = true;
        if (state->table.load())
            result["table_bytes_used"] = state->table.load();
        double ended = now();
        if(inline_compute && verify_start<prep_end)verify_start=ended;
        if (!invoked && !inline_compute) {
            prep_end = ended;
            verify_start = ended;
        }
        result["job_id"] = job.at("id");
        result["job_digest"] = job.at("receipt_digest");
        if (invoked) {
            result["native_exit_code"]=proc.code;
            if (result["status"]=="ERROR") {
                result["native_stderr"]=proc.err.substr(0,8192);
                result["native_stdout"]=proc.out.substr(0,8192);
            }
        }
        result["cpu_seconds"] = proc.cpu;
        result["cpp_thread_cpu_seconds"] = thread_cpu() - cpu_start;
        result["job_wall_seconds"] = ended - started;
        result["peak_rss_kib"] = std::max<long>(proc.peak_rss, result.value("peak_rss_kib", long(0)));
        result["phase_seconds"] = {{"preparation", prep_end - started},
                                   {"native", inline_compute?std::max(0.,verify_start-prep_end):proc.wall},
                                   {"verification", ended - verify_start}};
        result["forced_native_termination"] = proc.forced;
        try {
            save_json(dir / "receipt.json", result);
        } catch (const std::exception &e) {
            result["status"] = "ERROR";
            result["reason"] = std::string("Receipt write: ") + e.what();
        }
        ram.release(state);
        state->set(4);
        return result;
    }
};
}
