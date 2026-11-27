#pragma once
#include "campaign.hpp"
#include "adaptive_scheduler.hpp"
namespace thermo {
inline J Campaign::search_job() {

    if(workers.support && !workers.support->ready() &&
       running("pattern_dp").size()<std::max(1u,searchslots/4)) {
        std::set<std::pair<unsigned,unsigned>> busy;
        for(const auto *j:running("pattern_dp")) {

            busy.emplace(j->at("layer").get<unsigned>(),j->at("count").get<unsigned>());
        }
        auto task=workers.support->next(busy);
        if(!task.is_null()) {
            task["kind"]="pattern_dp";task["seconds"]=a.pattern_seconds;task["evaluation"]=false;
            return task;
        }
    }

    if (cipher.present) {
        auto task = present_search();
        if (!task.is_null()) return task;
    }
    if (!cipher.present && !cipher.midori && a.common_value_seeds && running("seed").empty()) {
        auto start = common_seed_job(m, a, seed);
        if (!start.is_null()) return start;
    }
    if (cipher.midori && cipher.B == 4 && a.midori_binary_seeds && running("seed").empty()) {
        const auto &s = m.at("midori_binary_seeds");
        const auto endpoint_turn = count(m.at("mass_search_allocation"), "endpoint_turn");
        if (endpoint_turn >= count(s, "next_endpoint_turn"))
            return J{{"kind", "seed"}, {"seed_kind", "binary"}, {"counts", a.counts},
                {"seconds", 60}, {"keep", 4}, {"evaluation", false},
                {"binary_seed_sequence", count(s, "submitted")}, {"binary_seed_endpoint_turn", endpoint_turn}};
    }
    refresh_unions();
    auto turn = count(m.at("mass_search_allocation"), "lane_turn");
    if(a.activity_replicas && turn%4==0 &&
       running("activity").size()+running("expand").size()<std::max(1u,searchslots/6) &&
       ((turn/4)%2==0 || partial_cache.empty())) {
        auto task=activity_search_job();if(!task.is_null())return task;
    }
    if (!partial_cache.empty() && turn % 4 == 0 &&
        running("expand").size() < std::max(1u, searchslots / 6)) {
        if (a.adaptive_patterns) return adaptive_expansion_job();
        std::vector<const J *> choices;
        for (std::size_t i = 0; i < std::min<std::size_t>(4, partial_cache.size()); ++i)
            if (!partial_cache[i]["component_ids"].empty())
                choices.push_back(&partial_cache[i]);
        if (!choices.empty()) {
            auto *u = *std::min_element(choices.begin(), choices.end(), [&](const J *x, const J *y) {
                return count(m["expansions"], x->at("endpoint").get<std::string>().c_str()) <
                       count(m["expansions"], y->at("endpoint").get<std::string>().c_str());
            });
            auto ep = u->at("endpoint").get<std::string>();
            auto n = count(m["expansions"], ep.c_str());
            auto cid = u->at("component_ids")[n % u->at("component_ids").size()].get<std::string>();
            auto &c = cases.at(cid);
            return J{{"kind", "expand"},
                     {"cid", cid},
                     {"states", c["states"]},
                     {"center", std::array<int, 6>{0, 4, 8, 16, 24, 32}[n % 6]},
                     {"expansion_allocation", J{{"endpoint", ep}, {"sequence", n},
                                                {"certified_union", u->at("mass")},
                                                {"has_unfinished_patterns", count(*u, "unfinished_patterns") > 0}}},
                     {"seconds", a.expansion_seconds},
                     {"evaluation", false}};
        }
    }
    const bool use_transfer=workers.support && workers.support->ready() &&
        (!a.mixed_pattern_generators || (count(m.at("pattern_generation_allocation"),"sequence")/a.counts.size())%2==0);
    auto pattern_job = [&]() { return J{{"kind", "pattern"},
                 {"generator",use_transfer?"transfer":"legacy"},
                 {"active", next_pattern_active(m, a.counts)},
                 {"seconds", a.pattern_seconds},
                 {"keep", 8},
                 {"evaluation", false}}; };
    bool generate=true;
    if(use_transfer) {
        std::uint64_t pending=0;
        for(const auto &kv:patterns)pending+=kv.second.value("discovery_generator",std::string())=="transfer" && !count(kv.second,"searches");

        for(const auto *j:running("pattern"))pending+=j->value("generator",std::string())=="transfer"?count(*j,"keep",8):0;
        generate=pending<(a.transfer_lookahead?a.transfer_lookahead:std::max(1u,searchslots));
    }
    if (generate && running("pattern").size() < std::max(1u, searchslots / 8)) return pattern_job();
    auto endpoint_job=endpoint_search_job();

    return endpoint_job.is_null() ? pattern_job() : endpoint_job;
}
inline J Campaign::activity_search_job() {
    refresh_ranking();
    const auto turn=count(m,"activity_turn");
    unsigned copies=turn%2?4:2;std::string mode="activity_"+std::to_string(copies);
    std::set<std::string> busy;
    for(const auto *j:running("activity"))if(j->at("copies")==copies)busy.insert(j->at("pid"));
    const J *chosen=nullptr,*start=nullptr;double best=-INFINITY,reference=INFINITY;
    bool coverage=turn%3==2;
    for(const auto &kv:patterns) {
        const auto &p=kv.second;if(p.at("states").is_null() || busy.count(kv.first))continue;
        auto it=search_quality.find(kv.first);if(it==search_quality.end() || it->second.empty())continue;
        const auto &candidate=cases.at(it->second.begin()->second);
        double F=replica_quality(candidate);
        double score=adaptive_priority(p,mode,F,a.search_seconds,0);
        if(!std::isfinite(score))continue;
        auto order=[&](const J &v){return std::make_tuple(count(v,"activity_submissions"),count(v,"activity_last_job"),v.at("id"));};
        if(!chosen || (coverage ? order(p)<order(*chosen) :
             score>best || (score==best && order(p)<order(*chosen)))) {
            chosen=&p;start=&candidate;reference=F;best=score;
        }
    }
    if(!chosen)return nullptr;
    J job{{"kind","activity"},{"pid",chosen->at("id")},{"states",start->at("states")},
          {"copies",copies},{"activity_levels",a.activity_levels},{"seconds",a.search_seconds},{"evaluation",false},
          {"activity_selection",coverage?"coverage":"mass_and_yield"},
          {"adaptive_action",{{"pid",chosen->at("id")},{"mode",mode},{"reference_F",reference},
              {"revision",adaptive_revision},{"selection_log2_score",best},{"bundle_patterns",0}}}};
    const auto key=std::to_string(copies);
    if(chosen->contains("activity_continuation") && chosen->at("activity_continuation").contains(key))
        job["activity_resume"]=chosen->at("activity_continuation").at(key);
    return job;
}
inline J Campaign::endpoint_search_job() {
    refresh_ranking();
    const auto &allocation = m.at("mass_search_allocation");
    const auto turn = count(allocation, "endpoint_turn");
    std::map<std::string, std::uint64_t> busy;
    for (auto *j : running("search"))
        ++busy[patterns.at(j->at("pid"))["orbit_id"]];
    std::vector<J *> choices, witnessed;
    for (auto &kv : patterns) {
        auto &p = kv.second;
        if (!a.adaptive_patterns && busy[p["orbit_id"]] >= 3)
            continue;
        bool has = !p["states"].is_null();
        int n = p["active"];
        if (std::find(a.counts.begin(), a.counts.end(), n) == a.counts.end() && !has)
            continue;
        choices.push_back(&p);
        if (has)
            witnessed.push_back(&p);
    }
    if (choices.empty())
        return nullptr;
    const unsigned requested = std::array<unsigned, 3>{1, 2, 4}[turn % 3];
    const unsigned copies = requested == 1 || witnessed.empty() ? 1 : requested;
    auto bucket = [&](const J &p) {
        int n = p.at("active");
        return std::find(a.counts.begin(), a.counts.end(), n) != a.counts.end()
                   ? std::to_string(n) : std::string("outside_initial_counts");
    };
    std::string role = copies == 1 ? "breadth" : "replica_" + std::to_string(copies);
    std::uint64_t next_cursor = count(allocation.at("group_cursors"), role.c_str());
    auto group = [&](const std::vector<J *> &pool) {
        std::vector<std::string> labels;
        for (int n : a.counts) labels.push_back(std::to_string(n));
        labels.push_back("outside_initial_counts");
        for (std::size_t offset = 0; offset < labels.size(); ++offset) {
            auto index = (next_cursor + offset) % labels.size();
            std::vector<J *> selected;
            for (auto *p : pool)
                if (bucket(*p) == labels[index]) selected.push_back(p);
            if (!selected.empty()) {
                next_cursor = (index + 1) % labels.size();
                return selected;
            }
        }
        throw std::runtime_error("Empty endpoint-search count bucket");
    };
    auto order = [](const J &p) { return p.at("mass_search_order").get<std::uint64_t>(); };
    auto last = [](const J &p) { return p.at("mass_search_last_turn").get<std::int64_t>(); };
    auto quality = [&](const J &p) {
        auto it = search_quality.find(p.at("id").get<std::string>());
        return it == search_quality.end() || it->second.empty() ? double(INFINITY) : it->second.begin()->first;
    };
    J *p = nullptr;
    std::string reason;
    if (copies == 1) {
        std::vector<J *> fresh;
        for (auto *v : choices) if (!count(*v, "searches")) fresh.push_back(v);
        reason = fresh.empty() ? "breadth_revisit" : "first_pattern_search";
        if(workers.support && workers.support->ready() && (turn/3)%2==0) {
            std::vector<J *> transfer;
            for(auto *v:fresh)if(v->value("discovery_generator",std::string())=="transfer")transfer.push_back(v);
            if(!transfer.empty()) {fresh=std::move(transfer);reason="first_transfer_search";}
        }
        auto pool = group(fresh.empty() ? choices : fresh);
        p = *std::min_element(pool.begin(), pool.end(), [&](const J *x, const J *y) {
            return std::make_tuple(count(*x, "searches"), last(*x), order(*x)) <
                   std::make_tuple(count(*y, "searches"), last(*y), order(*y));
        });
    } else {
        const bool targeted = (turn / 3) % 3 != 2;
        reason = targeted ? "promising_replica" : "replica_coverage";
        const auto key = std::to_string(copies);
        auto attempts = [&](const J &v) { return count(v.at("mass_search_counts"), key.c_str()); };
        if (targeted && !a.replica_focus_patterns) {

            p = *std::min_element(witnessed.begin(), witnessed.end(), [&](const J *x, const J *y) {
                return std::make_tuple(quality(*x), attempts(*x), last(*x), order(*x)) <
                       std::make_tuple(quality(*y), attempts(*y), last(*y), order(*y));
            });
        } else {
            auto pool = witnessed;
            if (targeted) {
                std::sort(pool.begin(), pool.end(), [&](const J *x, const J *y) {
                    return std::make_tuple(quality(*x), last(*x), order(*x)) <
                           std::make_tuple(quality(*y), last(*y), order(*y));
                });
                if (pool.size() > a.replica_focus_patterns) pool.resize(a.replica_focus_patterns);
            }
            pool = group(pool);
            p = *std::min_element(pool.begin(), pool.end(), [&](const J *x, const J *y) {
                return std::make_tuple(attempts(*x), last(*x), order(*x)) <
                       std::make_tuple(attempts(*y), last(*y), order(*y));
            });
        }
    }
    J job{{"kind", "search"},
             {"pid", (*p)["id"]},
             {"pattern", (*p)["pattern"]},
             {"states", (*p)["states"]},
             {"copies", copies},
             {"search_allocation", J{{"schema", mass_search_schema}, {"turn", turn}, {"role", role},
                                     {"reason", reason}, {"requested_copies", requested},
                                     {"bucket", bucket(*p)}, {"next_cursor", next_cursor},
                                     {"first_pattern_search", count(*p, "searches") == 0},
                                     {"priority_F", std::isfinite(quality(*p)) ? J(quality(*p)) : J()}}},
             {"seconds", a.search_seconds},
             {"evaluation", false}};
    return adaptive_search_job(std::move(job), witnessed, copies>1 && (turn/3)%3!=2);
}
#include "../../shared/indexed_evaluation.inc"
inline J Campaign::evaluation_job(unsigned capacity) {
    increment(m, "evaluation_counter");
    auto turn = count(m, "evaluation_counter");
    unsigned cap =  (capacity ? capacity : evalslots);

    if (cipher.present) {
        J pair = pair_job(cap > used_cores(true) ? cap - used_cores(true) : 0);
        if (!pair.is_null())
            return pair;
    }
    if(cipher.cfg.at("adaptive").get<bool>())return indexed_evaluation_job(cap,turn);
    std::vector<J *> ranked;

    refresh_ranking();
    auto eligible = [&](const std::string &id) {
        auto &c = cases.at(id);
        if (!c["exact"].is_null() || covered(c) || number(c, "next_at") > elapsed())
            return;
        if (admission_blocked(id))
            return;

        ranked.push_back(&c);
    };
    {for (const auto &rank:rank_index)eligible(std::get<2>(rank));}
    if (ranked.empty())
        return J();
    if(a.cost_priority && turn%4==0) {
        std::map<const J*,double> keys;
        for(const auto* c:ranked)keys.emplace(c,mass_cost_score(*c,std::get<0>(rank_keys.at(c->at("id"))),a.exact_seconds));
        std::stable_sort(ranked.begin(),ranked.end(),[&](const J*x,const J*y){return keys.at(x)<keys.at(y);});
    }
    auto screenjob = [&](J &c) {
        return J{{"kind", "screen"},
                 {"cid", c["id"]},
                 {"states", c["states"]},
                 {"pattern", patterns.at(c["pid"])["pattern"]},
                 {"paths", a.paths},
                 {"steps", a.steps},
                 {"seconds", a.screen_seconds},
                 {"evaluation", true}};
    };
    unsigned auxiliary = std::max(1u, cap / 8);
    if (running("screen").size() < auxiliary) {
        J *chosen = nullptr;
        for (auto *c : ranked) {
            if (!running("screen", c->at("id")).empty())
                continue;
            if(cipher.cfg["adaptive"].get<bool>()){
                if((*c)["plan"].is_null())continue;
                const auto bound=c->value("root_bound",J::object());
                if(bound.contains("domain_volume") && integer(bound.at("domain_volume"))*std::max<std::uint64_t>(1,count(*c,"active"))<=Z(a.leaf_operations))continue;
            }
            bool first = count(*c, "screen_attempts") == 0;
            bool again =
                count(*c, "screen_attempts") < 2 && number((*c)["screen"], "ess") < 8 && turn % 8 == 0;
            if ((first && (turn % 3 != 0)) || again) {
                chosen = c;
                break;
            }
        }
        if (chosen)
            return screenjob(*chosen);
    }
    if (!cipher.cfg["adaptive"].get<bool>()) {
        if (turn % 8 == 0)
            std::stable_sort(ranked.begin(), ranked.end(), [](const J *x, const J *y) {
                return number(*x, "exact_cpu_seconds") < number(*y, "exact_cpu_seconds");
            });
        unsigned familycap =  std::max(1u, (cap + std::min<unsigned>(4, ranked.size()) - 1) /
                                                       std::min<unsigned>(4, ranked.size()));
        for (auto *c : ranked) {
            auto id = c->at("id").get<std::string>();
            auto busy = running("exact", id);
            if (busy.size() >= familycap || ((*c)["plan"].is_null() && !busy.empty()))
                continue;
            auto total = (*c)["plan"].is_null() ? 1 : (*c)["plan"]["slices"].get<std::uint64_t>();

            std::set<std::uint64_t> taken;
            for (auto *j : busy)
                taken.insert(j->at("slice").get<std::uint64_t>());
            std::uint64_t slice = 0;
            while (slice < total && ((*c)["slices"].contains(std::to_string(slice)) || taken.count(slice)))
                ++slice;
            if (slice >= total)
                continue;
            double attempts = number((*c)["attempts"], std::to_string(slice).c_str());
            double seconds =
                std::min(a.max_exact_seconds, a.exact_seconds * std::exp2(std::min(8., attempts)));

            return J{{"kind", "exact"},          {"cid", id},
                     {"states", (*c)["states"]}, {"plan", (*c)["plan"]},
                     {"slice", slice},           {"seconds", seconds},
                     {"evaluation", true}};
        }
        return nullptr;
    }
    auto planjob = [&](J &c) {
        return J{{"kind", "plan"},
                 {"cid", c["id"]},
                 {"states", c["states"]},
                 {"seconds",  a.plan_seconds * std::exp2(std::min(4., number(c, "plan_failures")))},
                 {"leaf_operations", a.leaf_operations},
                 {"max_frontier", a.chunk_frontier},

                 {"evaluation", true}};
    };
    std::vector<J *> ready, unplanned;
    for (auto *c : ranked) {
        auto id = c->at("id").get<std::string>();
        if ((*c)["plan"].is_null()) {
            if (running("plan", id).empty())
                unplanned.push_back(c);
        } else if (!(*c)["frontier"].empty())
            ready.push_back(c);
    }

    if (running("plan").size() < auxiliary && !unplanned.empty() && (ready.empty() || turn % 8 == 1)) {
        return planjob(*unplanned.front());
    }
    auto &focus = m["focus"];
    focus = J::array();

    for (auto *c : ready) {
        if (focus.size() == a.focus)
            break;
        focus.push_back(c->at("id"));
    }
    std::vector<J *> order;
    for (auto &id : focus)
        order.push_back(&cases.at(id.get<std::string>()));
    std::stable_sort(order.begin(), order.end(), [&](const J *x, const J *y) {
        return std::make_tuple(running("exact", x->at("id")).size(), rank_keys.at(x->at("id"))) <
               std::make_tuple(running("exact", y->at("id")).size(), rank_keys.at(y->at("id")));
    });
    const std::set<J*> focused(order.begin(),order.end());
    for(auto* c:ready)if(!focused.count(c))order.push_back(c);
    for (auto *c : order) {
        if (number(*c, "next_at") > elapsed())
            continue;
        auto id = c->at("id").get<std::string>();
        auto busy = running("exact", id);
        std::set<std::string> taken;
        for(auto *job:busy)taken.insert(job->at("box_id"));

        std::string key;
        std::pair<int, int> best{-1, 0};
        for (auto it = (*c)["frontier"].begin(); it != (*c)["frontier"].end(); ++it) {
            if (taken.count(it.key()))
                continue;
            int mass = 0;
            for (auto &b : it.value()) {
                Z mask(b[1].get<std::string>(), 16);
                mass += mpz_popcount(mask.get_mpz_t());
            }
            auto k = std::make_pair(int(it.value().size()), -mass);
            if (key.empty() || k > best) {
                key = it.key();
                best = k;
            }
        }
        if (key.empty())
            continue;
        auto attempts = count((*c)["attempts"], key.c_str());
        J j{{"kind", "exact"},
            {"cid", id},
            {"states", (*c)["states"]},
            {"box_id", key},
            {"box", (*c)["frontier"][key]},
            {"seconds", exact_seconds_for(*c, key)},
            {"leaf_operations",
             std::max<std::uint64_t>(1024, a.leaf_operations >> std::min<std::uint64_t>(attempts, 12))},
            {"max_frontier", a.chunk_frontier},

            {"evaluation", true}};

        bool splitting = false;
        for (auto &x : active)
            if (x.second.job.value("partition_target", 0) > 1)
                splitting = true;
        std::size_t available = 0;
        for (auto *q : order)
            available += q->at("frontier").size() -
                         std::min(q->at("frontier").size(), running("exact", q->at("id")).size());
        auto used = used_cores(true);
        bool costly = number((*c)["plan"], "operation_score") > a.leaf_operations || attempts > 0;
        if (!splitting && costly && !(*c).value("unsplittable", J::object()).contains(key) &&
            cap > used + available) {
            j["partition_target"] = std::min<std::size_t>(a.chunk_frontier, cap - used - available + 1);
        }
        return j;
    }
    if (running("plan").size() < auxiliary && !unplanned.empty())
        return planjob(*unplanned.front());
    return nullptr;
}

inline void gather_witnesses(const J &v, int rows, int width, std::map<std::string, J> &out) {
    if (v.is_array()) {
        bool good = int(v.size()) == rows;
        for (auto &s : v)
            if (!s.is_string() || int(s.get<std::string>().size()) != width ||
                upper(s.get<std::string>()).find_first_not_of("0123456789ABCDEF") != std::string::npos)
                good = false;
        if (good) {
            J u = v;
            for (auto &s : u)
                s = upper(s.get<std::string>());
            out[digest(u)] = u;
            return;
        }
        for (auto &x : v)
            gather_witnesses(x, rows, width, out);
    } else if (v.is_object())
        for (auto &x : v)
            if (x.is_structured())
                gather_witnesses(x, rows, width, out);
}
inline void Campaign::import_previous() {
    std::vector<fs::path> sources = a.reuse;
    sources.insert(sources.end(), a.imports.begin(), a.imports.end());
    for (auto &path : sources) {
        require(fs::weakly_canonical(path) != fs::weakly_canonical(a.out),
                "Cannot import the active output as a previous run");
        auto id = digest(path.string());
        bool seen = false;
        for (auto &x : m["imports"])
            if (x["id"] == id)
                seen = true;
        if (seen)
            continue;
        std::map<std::string, J> wits;
        if (fs::is_directory(path) && fs::exists(path / "state.sqlite3")) {
            Database old(path / "state.sqlite3", true);
            for (auto &kv : old.load("cases"))
                gather_witnesses(kv.second, cipher.R + 1, cipher.N * (cipher.B / 4), wits);
        } else
            gather_witnesses(read_json(fs::is_directory(path) ? path / "RESULTS.json" : path), cipher.R + 1,
                             cipher.N * (cipher.B / 4), wits);

        J values = J::array();
        for (auto &kv : wits)
            values.push_back(kv.second);
        for (std::size_t i = 0; i < values.size(); i += a.keep) {
            J batch = J::array();
            for (std::size_t k = i; k < std::min(values.size(), i + a.keep); ++k)
                batch.push_back(values[k]);
            J j{{"kind", "import"},
                {"witnesses", batch},
                {"seconds", a.screen_seconds},
                {"evaluation", false}};
            while (!submit(j)) {
                collect();
                if (stopping || elapsed() >= a.hours * 3600)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            while (active.size() >= slots) {
                collect();
                if (had_error || stopping)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (stopping || had_error || elapsed() >= a.hours * 3600)
                break;
        }
        m["imports"].push_back(J{{"id", id},
                                 {"path", path.string()},
                                 {"witnesses_seen", values.size()},
                                 {"cost_counted_in_this_run", true},
                                 {"imported_mass", false}});
        flush();
    }
}
inline int Campaign::execute() {
    std::cout << cipher.cfg["cipher"].get<std::string>() << " " << cipher.R << "r" << std::endl;
    try {
        while (true) {
            const auto completed_revision=pool.revision();
            begin_service();
            drain_before_refill();
            if (had_error)
                stopping = true;
            if (stopping) {
                stage = "STOPPING";
                reason = had_error ? "ERROR" : "USER_INTERRUPTED";
                if (active.empty())
                    break;
            } else if (elapsed() >= a.hours*3600) {
                stage="DRAINING_AT_DEADLINE";
                if(active.empty()){reason="TIME_BUDGET";break;}
            } else {
                for(unsigned n=0;n<queue_capacity();++n) {
                    if(service_full()){commit_service();begin_service();}

                    if(n && n%service_batch_jobs==0 && pending_receipts()>=slots && used_cores(false)+used_cores(true)>=slots)break;
                    if(had_error || stopping || elapsed()>=a.hours*3600 || !admission_room())break;
                    const double select_started=now();J job;
                    if(evaluation_first())job=evaluation_job();
                    if(job.is_null())job=search_job();
                    if(job.is_null())job=evaluation_job();
                    if(job.is_null())job=evaluation_job(slots);
                    m["pipeline"]["selection_wall_seconds"]=number(m["pipeline"],"selection_wall_seconds")+now()-select_started;
                    if(job.is_null())break;
                    submit(job);
                }
                commit_service();

            }
            if(service_batch)commit_service();
            if (now() - last_publish >= 15)
                publish();
            if (now() - last_flush >= 2)
                flush();

            if(!pending_receipts()&&pool.revision()==completed_revision)pool.wait(completed_revision);
        }
    } catch (const std::exception &e) {
        if(service_batch)abort_service();
        had_error = true;
        reason = "ERROR";
        stopping = true;
        record_error(e.what());
        while (!active.empty()) {
            try {
                collect();
            } catch (const std::exception &x) {
                std::cerr << "Result preserved for restart: " << x.what() << std::endl;
                active.clear();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    flush(true);
    publish(true);
    return had_error ? 1 : 0;
}
}
