#pragma once
#include "scheduler.hpp"
namespace thermo {
inline J domain(unsigned n, unsigned k, bool initial = false) {
    return J{{"input_active", n},       {"cap", k},        {"initial", initial},  {"submitted", 0},
             {"finished", 0},           {"rows", 0},       {"positive_rows", 0},  {"productive_jobs", 0},
             {"allocation_seconds", 0}, {"best_F", J()},   {"improvements", 0},   {"failure_streak", 0},
             {"retry_after", 0},        {"retry_wall", 0}, {"last_submitted", 0}, {"stops", J::object()}};
}
inline std::string domain_key(unsigned n, unsigned k) {
    return std::to_string(n) + ":" + std::to_string(k);
}
inline unsigned input_activity(const std::string &s) {
    return unsigned(std::count_if(s.begin(), s.end(), [](char c) { return c != '0'; }));
}
inline void Campaign::initialize_domains() {
    for (int k : a.caps) {
        auto key = domain_key(a.input_cap, k);
        if (!m["domains"].contains(key))
            m["domains"][key] = domain(a.input_cap, k, true);
    }
    if (!m.contains("present_roles"))
        m["present_roles"] = J::object();
    if (!m.contains("endpoint_roles"))
        m["endpoint_roles"] = J::object();
    if (!m.contains("failed_inputs"))
        m["failed_inputs"] = J::object();
}
inline J Campaign::corridor_job() {
    if (a.caps.empty())
        return nullptr;
    auto permitted = [&](const J &d) {
        return d["input_active"].get<unsigned>() <= std::min(a.max_input, 4 * d["cap"].get<unsigned>()) &&
               d["cap"].get<unsigned>() <= a.max_cap;
    };
    if (count(m, "corridor_finished") >= count(m, "next_scope_expansion")) {
        std::map<std::string, std::pair<J, bool>> next;
        for (auto &d : m["domains"]) {
            if (!permitted(d) || !count(d, "finished"))
                continue;
            unsigned n = d["input_active"], k = d["cap"];
            for (auto v : {std::make_tuple(n + 1, k, true), std::make_tuple(n, k + 1, false)}) {
                auto nn = std::get<0>(v), kk = std::get<1>(v);
                auto name = domain_key(nn, kk);
                auto d2 = domain(nn, kk);
                if (!m["domains"].contains(name) && permitted(d2))
                    next[name] = {d2, std::get<2>(v)};
            }
        }
        bool preferred = count(m, "scope_expansions") % 2 == 0;
        auto order = [&](const auto &item) {
            auto &d = item.second.first;
            int u = d["input_active"].template get<int>() - int(a.input_cap),
                v = std::max(0, d["cap"].template get<int>() - a.caps.back());
            return std::make_tuple(std::max(u, v), item.second.second != preferred, u + v,
                                   d["input_active"].template get<int>() + d["cap"].template get<int>());
        };
        if (!next.empty()) {
            auto it = std::min_element(next.begin(), next.end(),
                                       [&](const auto &x, const auto &y) { return order(x) < order(y); });
            m["domains"][it->first] = it->second.first;
            increment(m, "scope_expansions");
            m["next_scope_expansion"] = count(m, "corridor_finished") + a.adapt_every;
        }
    }
    std::map<std::string, unsigned> busy;
    unsigned risky = 0;
    for (auto *j : running("corridor")) {
        ++busy[j->at("domain")];
        risky += j->value("adaptive_probe", false);
    }
    double failures = 0;
    for (auto &d : m["domains"])
        if (d["cap"].get<int>() > 3)
            failures += std::max(0., number(d, "finished") - number(d, "productive_jobs"));
    unsigned probes =
        std::max(1u, unsigned(std::ceil(std::ceil(searchslots * a.expansion_share) / (1 + failures / 16))));
    unsigned lanes =
        std::max(1u, unsigned(std::ceil(double(searchslots) / std::max<std::size_t>(3, a.caps.size()))));
    struct Entry {
        std::string key;
        J *d;
        bool probe;
    };
    std::vector<Entry> choices;
    for (auto it = m["domains"].begin(); it != m["domains"].end(); ++it) {
        auto &d = it.value();
        bool probe = (!d["initial"].get<bool>() || d["cap"].get<int>() > 3) &&
                     (!count(d, "positive_rows") || count(d, "failure_streak"));
        if (admission_blocked("domain:" + it.key()) || !permitted(d) ||
            (count(d, "retry_after") > count(m, "corridor_finished") &&
             number(d, "retry_wall") > elapsed()) ||
            busy[it.key()] >= unsigned(probe ? 1 : lanes) || (probe && risky >= probes))
            continue;
        choices.push_back({it.key(), &d, probe});
    }
    if (choices.empty())
        return nullptr;
    auto turn = count(m, "search_counter");
    std::vector<Entry> fresh;
    for (auto &e : choices)
        if (!count(*e.d, "submitted"))
            fresh.push_back(e);
    if (!fresh.empty())
        choices = fresh;
    else if (turn % 4 == 0) {
        std::vector<Entry> wider;
        for (auto &e : choices)
            if (!e.d->at("initial").get<bool>())
                wider.push_back(e);
        if (!wider.empty())
            choices = wider;
    }
    auto breadth = [&](const Entry &x, const Entry &y) {
        return std::make_tuple(count(*x.d, "submitted") + busy[x.key], count(*x.d, "last_submitted"), x.key) <
               std::make_tuple(count(*y.d, "submitted") + busy[y.key], count(*y.d, "last_submitted"), y.key);
    };
    Entry chosen = *std::min_element(choices.begin(), choices.end(), breadth);
    if (fresh.empty() && turn % 4 > 1) {
        double best = INFINITY;
        for (auto &e : choices)
            best = std::min(best, number(*e.d, "best_F", INFINITY));
        auto utility = [&](const Entry &e) {
            double f = number(*e.d, "best_F", INFINITY);
            double quality =
                (!std::isfinite(best) || !std::isfinite(f)) ? 1 : std::exp2(std::max(-12., best - f));
            return quality * (1 + count(*e.d, "improvements")) /
                   (a.corridor_probe_seconds + number(*e.d, "allocation_seconds")) / (1 + busy[e.key]);
        };
        chosen = *std::max_element(choices.begin(), choices.end(),
                                   [&](const Entry &x, const Entry &y) { return utility(x) < utility(y); });
    }
    auto &d = *chosen.d;
    auto n = count(d, "submitted");
    bool force = d["input_active"].get<unsigned>() > a.input_cap && n % 2 == 0;
    std::string initial;
    if (!force && (n % 2 == 1 || (chosen.probe && n == 0))) {
        std::map<std::string, double> inputs;
        for (auto &kv : cases) {
            auto &c = kv.second;
            auto in = c["input"].get<std::string>();
            if (input_activity(in) <= d["input_active"].get<unsigned>())
                inputs[in] =
                    std::min(inputs.count(in) ? inputs[in] : INFINITY, number(c, "weight", INFINITY));
        }
        for (auto &cert : m["corridor_certificates"]) {
            auto in = cert["input"].get<std::string>();
            if (input_activity(in) <= d["input_active"].get<unsigned>())
                inputs[in] = number(cert["mass"], "F", INFINITY);
        }
        std::vector<std::pair<double, std::string>> pool;
        for (auto &x : inputs) {
            auto key = std::to_string(d["cap"].get<int>()) + ":" + x.first;
            if (!m["failed_inputs"].contains(key) || elapsed() - m["failed_inputs"][key].get<double>() > 600)
                pool.push_back({x.second, x.first});
        }
        std::sort(pool.begin(), pool.end());
        if (!pool.empty())
            initial = pool[(n / 2) % std::min<std::size_t>(128, pool.size())].second;
    }
    return J{{"kind", "corridor"},
             {"domain", chosen.key},
             {"cap", d["cap"]},
             {"input_active", d["input_active"]},
             {"adaptive_probe", chosen.probe},
             {"start_active", force ? d["input_active"] : J(0)},
             {"initial", initial.empty() ? J() : J(initial)},
             {"copies", std::array<int, 3>{1, 2, 4}[n % 3]},
             {"seconds", chosen.probe ? std::min(a.corridor_seconds,
                                                 a.corridor_probe_seconds * std::exp2(std::min<double>(3, n)))
                                      : a.corridor_seconds},
             {"max_nodes", a.corridor_nodes},
             {"max_edges", a.corridor_edges},
             {"keep", 16},
             {"evaluation", false}};
}
inline void Campaign::corridor_completed(const J &j, const J &r) {
    increment(m, "corridor_finished");
    auto &d = m["domains"][j.at("domain").get<std::string>()];
    auto diag = r.value("diagnostics", J::object());
    auto rows = count(diag, "completed_rows"), zero = count(diag, "proved_zero_rows"),
         positive = rows >= zero ? rows - zero : 0;
    increment(d, "finished");
    increment(d, "rows", rows);
    increment(d, "positive_rows", positive);
    if (positive)
        increment(d, "productive_jobs");
    d["allocation_seconds"] =
        number(d, "allocation_seconds") + number(r, "cpu_seconds") + number(r, "cpp_thread_cpu_seconds");
    auto reason = diag.value("stop", r.value("reason", std::string("UNKNOWN")));
    increment(d["stops"], reason);
    for (auto &x : r.value("corridor_records", J::array())) {
        d["best_F"] = std::min(number(d, "best_F", INFINITY), number(x["mass"], "F", INFINITY));
        increment(d, "improvements");
    }
    if (diag.contains("unfinished_input") && diag["unfinished_input"].is_string()) {
        auto value = diag["unfinished_input"].get<std::string>();
        if (input_activity(value))
            m["failed_inputs"][str(j["cap"]) + ":" + value] = elapsed();
    }
    if (!positive) {
        increment(d, "failure_streak");
        d["retry_after"] =
            count(m, "corridor_finished") +
            std::min<std::uint64_t>(256, 4ULL << std::min<std::uint64_t>(6, count(d, "failure_streak")));
        d["retry_wall"] =
            elapsed() +
            std::min(120., a.corridor_probe_seconds * std::exp2(std::min(3., number(d, "failure_streak"))));
    } else {
        d["failure_streak"] = 0;
        d["retry_after"] = 0;
        d["retry_wall"] = 0;
    }
}
inline void Campaign::merge_certificates(const J &j, const J &r) {
    for (auto cert : r.at("corridor_records")) {
        auto ep = cert["endpoint"].get<std::string>();
        auto &dict = m["corridor_certificates"];
        if (dict.contains(ep)) {
            auto &old = dict[ep];
            int cmp = compare_mass(cert["mass"], old["mass"]), k = cert["cap"], k0 = old["cap"];
            require(!(k == k0 && cmp != 0) && !(k > k0 && cmp < 0) && !(k < k0 && cmp > 0),
                    "Nested exact corridor sums disagree");
            if (k <= k0)
                continue;
            std::vector<J> absorbed{old["mass"]};
            for (auto &kv : cases) {
                auto &c = kv.second;
                if (c["endpoint"] == ep && activity_cap(c) > k0 && activity_cap(c) <= k)
                    absorbed.push_back(orbit_mass(c));
            }
            require(compare_mass(cert["mass"], add_mass(absorbed)) >= 0,
                    "Expanded corridor omitted retained outside mass");
        }
        cert["first_seconds"] = number(j, "started") + number(cert, "first_seconds");
        cert["completed_seconds"] = elapsed();
        cert["origin"] = {{"kind", j["kind"]}, {"job", j["id"]}, {"seed", j["seed"]}};
        dict[ep] = cert;
        unions_dirty = true;
    }
}
inline J Campaign::present_search() {
    auto turn = count(m, "search_counter");
    if (turn % 5 != 4) {
        auto j = corridor_job();
        if (!j.is_null())
            return j;
    }
    unsigned cap = *std::max_element(a.counts.begin(), a.counts.end());
    std::map<std::string, std::vector<J *>> groups;
    for (auto &kv : cases)
        if (kv.second["active"].get<unsigned>() <= cap)
            groups[kv.second["endpoint"]].push_back(&kv.second);
    if ((groups.empty() || turn % 24 == 23) &&
        running("seed").size() < std::max(1u, std::min(2u, searchslots / 8)))
        return J{{"kind", "seed"},
                 {"active", cap},
                 {"seconds", std::min(30., a.pattern_seconds)},
                 {"evaluation", false}};
    unsigned fresh = 0;
    for (auto &kv : patterns)
        fresh += count(kv.second, "searches") == 0;
    if ((groups.empty() || turn % 8 == 2) && fresh < std::max(32u, 4 * searchslots) &&
        running("pattern").empty())
        return J{{"kind", "pattern"},
                 {"active", next_pattern_active(m, a.counts)},
                 {"seconds", a.pattern_seconds},
                 {"keep", 8},
                 {"evaluation", false}};
    auto known = [&](J *c) {
        return std::min(number(*c, "weight", INFINITY), number(raw_mass(*c), "F", INFINITY));
    };
    auto groupF = [&](const std::string &e) {
        std::vector<J> parts;
        for (auto *c : groups[e])
            if (!covered(*c)) {
                auto mass = raw_mass(*c), w = dyadic(integer((*c)["witness_numerator"]), exponent(*c));
                parts.push_back(compare_mass(mass, w) > 0 ? mass : w);
            }
        if (m["corridor_certificates"].contains(e))
            parts.push_back(m["corridor_certificates"][e]["mass"]);
        return number(add_mass(parts), "F", INFINITY);
    };
    auto pick = [&](const std::string &role) -> std::string {
        std::map<std::string, unsigned> busy;
        for (auto *j : running(role))
            ++busy[j->at("endpoint")];
        std::vector<std::string> eligible;
        for (auto &kv : groups)
            if (busy[kv.first] < (role == "ensemble" ? 2u : 1u))
                eligible.push_back(kv.first);
        if (eligible.empty())
            return "";
        std::sort(eligible.begin(), eligible.end(), [&](const std::string &x, const std::string &y) {
            return std::make_pair(groupF(x), x) < std::make_pair(groupF(y), y);
        });
        if (count(m["present_roles"], role.c_str()) % 3 != 2 && eligible.size() > 32)
            eligible.resize(32);
        return *std::min_element(
            eligible.begin(), eligible.end(), [&](const std::string &x, const std::string &y) {
                auto sx = m["endpoint_roles"].value(x, J::object()),
                     sy = m["endpoint_roles"].value(y, J::object());
                return std::make_tuple(count(sx, role.c_str()), count(sx, (role + "_last").c_str()), x) <
                       std::make_tuple(count(sy, role.c_str()), count(sy, (role + "_last").c_str()), y);
            });
    };
    const std::array<std::string, 8> roles = {"ensemble", "ensemble", "breadth",  "ensemble",
                                              "expand",   "ensemble", "ensemble", "ensemble"};
    for (auto role : {roles[turn % 8], std::string("ensemble"), std::string("breadth")}) {
        if (role == "breadth") {
            std::set<std::string> busy;
            for (auto *j : running("search"))
                busy.insert(j->at("pid"));
            std::vector<J *> picks;
            for (auto &kv : patterns)
                if (!busy.count(kv.first))
                    picks.push_back(&kv.second);
            if (picks.empty())
                continue;
            auto *p = *std::min_element(picks.begin(), picks.end(), [](const J *x, const J *y) {
                return std::make_tuple(count(*x, "searches"), x->at("active").get<int>(),
                                       x->at("id").get<std::string>()) <
                       std::make_tuple(count(*y, "searches"), y->at("active").get<int>(),
                                       y->at("id").get<std::string>());
            });
            return J{
                {"kind", "search"},         {"pid", (*p)["id"]}, {"pattern", (*p)["pattern"]},
                {"states", (*p)["states"]}, {"copies", 1},       {"seconds", std::min(60., a.search_seconds)},
                {"evaluation", false}};
        }
        auto ep = pick(role);
        if (ep.empty())
            continue;
        auto list = groups[ep];
        std::sort(list.begin(), list.end(), [&](J *x, J *y) {
            return std::make_pair(known(x), x->at("id").get<std::string>()) <
                   std::make_pair(known(y), y->at("id").get<std::string>());
        });
        auto n = count(m["endpoint_roles"].value(ep, J::object()), role.c_str());
        if (role == "expand") {
            auto *c = list[n % std::min<std::size_t>(32, list.size())];
            return J{{"kind", "expand"},
                     {"endpoint", ep},
                     {"cid", (*c)["id"]},
                     {"states", (*c)["states"]},
                     {"center", std::array<int, 6>{0, 4, 8, 16, 24, 32}[n % 6]},
                     {"seconds", std::min(60., a.expansion_seconds)},
                     {"evaluation", false}};
        }
        J seeds = J::array();
        for (std::size_t i = 0; i < std::min<std::size_t>(8, list.size()); ++i)
            seeds.push_back(list[i]->at("states"));
        if (list.size() > 8)
            for (std::size_t i = 0; i < std::min<std::size_t>(8, list.size() - 8); ++i)
                seeds.push_back(list[8 + ((8 * n + i) % (list.size() - 8))]->at("states"));
        return J{{"kind", "ensemble"},
                 {"endpoint", ep},
                 {"initial_trails", seeds},
                 {"active", cap},
                 {"copies", std::array<int, 3>{1, 2, 4}[count(m["present_roles"], "ensemble") % 3]},
                 {"seconds", 60},
                 {"keep", std::min(128u, std::max(64u, a.keep))},
                 {"evaluation", false}};
    }
    return corridor_job();
}

inline J Campaign::pair_job(unsigned available) {
    if (a.caps.empty())
        return nullptr;
    unsigned reservation = std::max(1u, unsigned(evalslots * a.pair_share)),
             nt = std::min(a.pair_threads ? a.pair_threads : std::min(8u, std::max(1u, reservation / 2)),
                           reservation),
             used = 0;
    std::set<std::string> busy;
    for (auto *j : running("pair")) {
        used += j->value("cpu_cores", 1u);
        busy.insert(j->at("endpoint"));
    }
    if (available < nt || used + nt > reservation)
        return nullptr;
    std::map<std::string, J> groups;
    for (auto &kv : cases) {
        auto &c = kv.second;
        auto e = c["endpoint"].get<std::string>();
        unsigned k = activity_cap(c);
        if (k > a.max_cap)
            continue;
        if (!groups.count(e) ||
            std::make_pair(k, number(c, "weight")) <
                std::make_pair(groups[e]["witness_cap"].get<unsigned>(), number(groups[e], "weight")))
            groups[e] = J{{"endpoint", e},
                          {"witness_cap", k},
                          {"weight", c["weight"]},
                          {"states", c["states"]},
                          {"F", c["weight"]}};
    }
    for (auto &kv : groups)
        if (m["corridor_certificates"].contains(kv.first))
            kv.second["F"] = std::min(number(kv.second, "F"),
                                      number(m["corridor_certificates"][kv.first]["mass"], "F", INFINITY));
    refresh_unions();
    for (auto &u : partial_cache)
        if (groups.count(u["endpoint"]))
            groups[u["endpoint"]]["F"] = u["mass"]["F"];
    std::map<std::string, double> served;
    for (auto &t : m["pair_tasks"])
        served[t["endpoint"]] += number(t, "cpu_seconds");
    std::vector<std::string> order;
    for (auto &kv : groups)
        if (!busy.count(kv.first))
            order.push_back(kv.first);
    bool coverage = count(m, "pair_turn") % 4 == 3;
    std::sort(order.begin(), order.end(), [&](const auto &x, const auto &y) {
        return std::make_tuple(coverage ? served[x] : 0, number(groups[x], "F"), x) <
               std::make_tuple(coverage ? served[y] : 0, number(groups[y], "F"), y);
    });
    std::size_t window =
        coverage ? a.pair_pending : std::min(a.pair_pending, std::max(16u, 2 * unsigned(busy.size()) + 4));
    if (order.size() > window)
        order.resize(window);
    J chosen;
    std::tuple<double, std::uint64_t, double, std::string> best;
    for (auto &e : order) {
        auto &g = groups[e];
        unsigned cap = std::max(g["witness_cap"].get<unsigned>(),
                                m["corridor_certificates"].contains(e)
                                    ? m["corridor_certificates"][e]["cap"].get<unsigned>() + 1
                                    : unsigned(a.caps.front()));
        if (cap > a.max_cap)
            continue;
        auto name = digest(J::array({cipher.R, e, cap, "CPP_PAIR_V1"}));
        auto t = m["pair_tasks"].value(name, J::object());
        if (admission_blocked("pair:" + name) || t.value("done", false) || number(t, "retry_at") > elapsed())
            continue;
        auto key = std::make_tuple(coverage ? served[e] : number(t, "cpu_seconds"), count(t, "last"),
                                   number(g, "F"), e);
        if (chosen.is_null() || key < best) {
            best = key;
            chosen = J{{"kind", "pair"},
                       {"pair_id", name},
                       {"endpoint", e},
                       {"states", g["states"]},
                       {"cap", cap},
                       {"cpu_cores", nt},
                       {"seconds", std::min(a.pair_max_seconds,
                                            a.pair_seconds * std::exp2(std::min(4., number(t, "stalls"))))},
                       {"work", (scratch / "pair" / name).string()},
                       {"evaluation", true}};
        }
    }
    return chosen;
}
}
