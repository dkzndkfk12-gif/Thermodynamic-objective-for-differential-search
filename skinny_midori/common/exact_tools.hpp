#pragma once
#include "model.hpp"
namespace thermo {
inline Domains box_domains(const Family &f, const J &box) {
    Domains d = f.domains;
    std::set<int> seen;
    for (const auto &p : box) {
        require(p.is_array() && p.size() == 2 && p[0].is_number_integer() && p[1].is_string(),
                "Malformed exact box");
        int v = p[0];
        require(v >= 0 && v < int(d.size()) && seen.insert(v).second, "Bad/repeated box variable");
        Z m(p[1].get<std::string>(), 16), base = 0;
        for (int x : d[v])
            mpz_setbit(base.get_mpz_t(), x);
        require(m > 0 && (m & base) == m, "Box escapes original domain");
        d[v].clear();
        for (int x = 0; x < 256; ++x)
            if (mpz_tstbit(m.get_mpz_t(), x))
                d[v].push_back(x);
    }
    return d;
}
inline std::string mask_text(const Domain &d) {
    Z m = 0;
    for (int x : d)
        mpz_setbit(m.get_mpz_t(), x);
    return m.get_str(16);
}
inline void verify_partition(const Family &f, const J &parent, const J &r) {
    Domains old = box_domains(f, parent);
    Z volume = product(old);
    std::vector<Domains> children;
    Z remain = 0;
    for (const auto &box : r.at("frontier")) {
        auto d = box_domains(f, box);
        for (int v = 0; v < int(d.size()); ++v)
            require(std::includes(old[v].begin(), old[v].end(), d[v].begin(), d[v].end()),
                    "Checkpoint escaped input box");
        for (const auto &other : children) {
            bool disjoint = false;
            for (int v = 0; v < int(d.size()); ++v) {
                std::vector<int> common;
                std::set_intersection(d[v].begin(), d[v].end(), other[v].begin(), other[v].end(),
                                      std::back_inserter(common));
                if (common.empty()) {
                    disjoint = true;
                    break;
                }
            }
            require(disjoint, "Overlapping exact checkpoint children");
        }
        remain += product(d);
        children.push_back(std::move(d));
    }
    Z closed = integer(r.at("closed_volume")), n = integer(r.at("numerator"));
    require(closed >= 0 && integer(r.at("initial_volume")) == volume && closed + remain == volume,
            "Exact assignment accounting failed");
    require(n >= 0 && r.at("denominator_power").get<unsigned>() == f.exponent,
            "Invalid exact numerator/denominator");
    require(closed != 0 || n == 0, "Mass without a closed region");
    require(r.at("status") != "EXACT_DONE" || children.empty(), "Incomplete exact marked done");
}
inline J plan_order(const Family &f, const std::vector<int> &pinned, const Deadline &dl) {
    std::set<int> pin(pinned.begin(), pinned.end()), remaining;
    for (int v = 0; v < int(f.domains.size()); ++v)
        if (!pin.count(v))
            remaining.insert(v);
    std::set<std::set<int>> unique;
    for (const auto &g : f.factors) {
        auto vv = Cipher::scope(g);
        std::set<int> s;
        for (int v : vv)
            if (!pin.count(v))
                s.insert(v);
        unique.insert(s);
    }
    std::vector<std::set<int>> scopes(unique.begin(), unique.end());
    auto size = [&](const std::set<int> &s) {
        Z n = 1;
        for (int v : s)
            n *= f.domains[v].size();
        return n;
    };
    Z current = 0;
    for (const auto &s : scopes)
        current += size(s);
    Z peak = current, visits = current;
    std::vector<int> order;
    while (!remaining.empty()) {
        dl.check();
        bool found = false;
        Z best;
        int degree = 0, var = -1;
        std::set<int> un;
        std::vector<int> bucket;
        for (int v : remaining) {
            std::set<int> u;
            std::vector<int> b;
            for (int i = 0; i < int(scopes.size()); ++i)
                if (scopes[i].count(v)) {
                    u.insert(scopes[i].begin(), scopes[i].end());
                    b.push_back(i);
                }
            require(!b.empty(), "Unconstrained exact coordinate");
            Z work = size(u);
            if (!found || work < best ||
                (work == best && std::pair<int, int>(int(b.size()), v) < std::pair<int, int>(degree, var))) {
                found = true;
                best = work;
                degree = int(b.size());
                var = v;
                un = u;
                bucket = b;
            }
        }
        un.erase(var);
        Z out = size(un), p = current + out;
        if (p > peak)
            peak = p;
        current += out;
        for (int i : bucket)
            current -= size(scopes[i]);
        visits += best * degree;
        std::vector<std::set<int>> next;
        for (const auto &s : scopes)
            if (!s.count(var))
                next.push_back(s);
        next.push_back(un);
        scopes = std::move(next);
        remaining.erase(var);
        order.push_back(var);
    }
    return J{
        {"order", order},
        {"peak_cells", peak.get_str()},
        {"operation_score_exact", visits.get_str()},
        {"operation_score", mpz_sizeinbase(visits.get_mpz_t(), 2) > 63 ? INT64_MAX : checked_u64(visits)}};
}
inline J make_plan(const Family &f, std::uint64_t table, std::uint64_t max_slices, const Deadline &dl) {
    std::vector<int> pin;
    for (int v = 0; v < int(f.domains.size()); ++v)
        if (f.domains[v].size() == 1)
            pin.push_back(v);
    while (true) {
        dl.check();
        J p = plan_order(f, pin, dl);
        Z slices = 1;
        for (int v : pin)
            slices *= f.domains[v].size();
        Z bytes = integer(p["peak_cells"]) * 256;
        if (bytes <= Z(std::to_string(table))) {
            p["pinned"] = pin;
            p["slices"] = checked_u64(slices);
            p["estimated_table_bytes"] = checked_u64(bytes);
            p["id"] = digest(p);
            return p;
        }
        bool found = false;
        Z best_peak, best_ops;
        int selected = -1;
        for (int v = 0; v < int(f.domains.size()); ++v)
            if (std::find(pin.begin(), pin.end(), v) == pin.end() &&
                slices * f.domains[v].size() <= Z(std::to_string(max_slices))) {
                auto trialpin = pin;
                trialpin.push_back(v);
                auto q = plan_order(f, trialpin, dl);
                Z pk = integer(q["peak_cells"]), op = integer(q["operation_score_exact"]);
                if (!found || pk < best_peak ||
                    (pk == best_peak && (op < best_ops || (op == best_ops && v < selected)))) {
                    found = true;
                    best_peak = pk;
                    best_ops = op;
                    selected = v;
                }
            }
        if (!found)
            return J{{"status", "RESOURCE_DEFERRED"},
                     {"reason", "TABLE_PLAN_LIMIT"},
                     {"estimated_table_bytes_exact", bytes.get_str()},
                     {"slices_exact", slices.get_str()}};
        pin.push_back(selected);
    }
}
inline Domains slice_domains(const Family &f, const J &plan, std::uint64_t ix) {
    auto d = f.domains;
    auto pinned = plan.at("pinned").get<std::vector<int>>();
    require(ix < plan.at("slices").get<std::uint64_t>(), "Invalid static slice");
    for (auto i = pinned.rbegin(); i != pinned.rend(); ++i) {
        int v = *i;
        auto n = d[v].size();
        int x = d[v][ix % n];
        ix /= n;
        d[v] = {x};
    }
    return d;
}
struct Symmetry {
    const Cipher &c;
    std::vector<std::vector<std::array<int, 16>>> paths;
    explicit Symmetry(const Cipher &cc) : c(cc) {
        int count = c.present || (c.midori && c.B == 8) ? 1 : c.midori ? 16 : 4;
        for (int g = 0; g < count; ++g) {
            std::vector<std::array<int, 16>> path;
            int t = g;
            for (int r = 0; r <= c.R; ++r) {
                std::array<int, 16> p{};
                for (int i = 0; i < 16; ++i)
                    p[i] = count == 1 ? i : c.midori ? i ^ t : 4 * (i / 4) + (i % 4 + g) % 4;
                path.push_back(p);
                if (c.midori && count > 1)
                    t = int(std::find(c.sr.begin(), c.sr.end(), t) - c.sr.begin());
            }
            paths.push_back(path);
        }
    }
    std::vector<std::string> transform(const std::vector<std::string> &rows,
                                       const std::vector<std::array<int, 16>> &p) const {
        auto out = rows;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            int w = int(rows[r].size() / 16);
            std::string s;
            for (int i : p[r])
                s += rows[r].substr(i * w, w);
            out[r] = s;
        }
        return out;
    }
    Pattern canonical_pattern(const Pattern &p) const {
        auto best = p;
        for (auto &path : paths)
            best = std::min(best, transform(p, path));
        return best;
    }
    std::pair<std::vector<std::string>, J> canonical(const std::vector<std::string> &s) const {
        std::vector<std::vector<std::string>> vv;
        std::set<std::pair<std::string, std::string>> ee;
        for (auto &p : paths) {
            auto x = transform(s, p);
            vv.push_back(x);
            ee.emplace(x.front(), x.back());
        }
        auto ep = *ee.begin();
        std::vector<std::vector<std::string>> at;
        std::set<Pattern> ps;
        for (auto &v : vv)
            if (std::make_pair(v.front(), v.back()) == ep) {
                at.push_back(v);
                ps.insert(c.activity(v));
            }
        auto best = *std::min_element(at.begin(), at.end(), [&](const auto &a, const auto &b) {
            auto pa = c.activity(a), pb = c.activity(b);
            return pa != pb ? pa < pb : a < b;
        });
        return {best, J{{"group_size", paths.size()},
                        {"endpoint_orbit_size", ee.size()},
                        {"multiplicity", ps.size()},
                        {"pattern_orbit", std::vector<Pattern>(ps.begin(), ps.end())},
                        {"original_endpoints", {s.front(), s.back()}}}};
    }
};
}
