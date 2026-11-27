#pragma once
#include "core.hpp"
#include <list>
#include <iostream>
#include <sstream>

namespace present_native {
using namespace thermo;
inline bool equivalent(const J &a, const J &b) { return nlohmann::json(a) == nlohmann::json(b); }
inline const J &get(const J &j, std::string k) {
    static const J empty;
    if (!j.is_object())
        return empty;
    auto p = j.find(k);
    if (p == j.end())
        return empty;
    return *p;
}
inline double num(const J &j, const std::string &k, double d = 0) {
    const auto &v = get(j, k);
    return v.is_null() ? d : v.is_string() ? std::stod(v.get<std::string>()) : v.get<double>();
}
inline long long iv(const J &j, const std::string &k, long long d = 0) {
    const auto &v = get(j, k);
    return v.is_null() ? d : v.is_string() ? std::stoll(v.get<std::string>()) : v.get<long long>();
}
inline bool truth(const J &j) {
    return !j.is_null() && (j.is_boolean() ? j.get<bool>() : j.is_number() ? j != 0 : !j.empty());
}
inline J &def(J &j, const std::string &k, const J &v) {
    if (!j.contains(k))
        j[k] = v;
    return j[k];
}
inline void inc(J &j, const std::string &k, double n = 1) { j[k] = num(j, k) + n; }
inline void inci(J &j, const std::string &k, long long n = 1) { j[k] = iv(j, k) + n; }
inline std::string str(const J &j) { return j.is_string() ? j.get<std::string>() : j.dump(); }
inline J slice(const J &x, std::size_t n) {
    J r = J::array();
    for (std::size_t i = 0; i < std::min(n, x.size()); ++i)
        r.push_back(x[i]);
    return r;
}
inline void tail(J &x, std::size_t n) {
    while (x.size() > n)
        x.erase(x.begin());
}
inline bool has(const J &list, const J &value) {
    for (const auto &item : list)
        if (item.is_object() && value.is_object() ? equivalent(item, value) : item == value)
            return true;
    return false;
}
inline Z zi(const J &j) {
    if (j.is_null())
        return 0;
    return Z(str(j), 10);
}
inline mpq_class probability(const J &m) {
    if (m.is_null())
        return 0;
    mpq_class q(zi(m.at("numerator")), pow2(m.at("denominator_power").get<unsigned>()));
    q.canonicalize();
    return q;
}
inline J sum_mass(const J &rows) {
    unsigned e = 0;
    for (const auto &m : rows)
        e = std::max(e, m.at("denominator_power").get<unsigned>());
    Z n = 0;
    for (const auto &m : rows)
        n += zi(m.at("numerator")) << unsigned(e - m.at("denominator_power").get<unsigned>());
    return dyadic(n, e);
}
inline int nibble(char c) {
    auto p = std::string("0123456789ABCDEF").find(c);
    require(p != std::string::npos, "Expected uppercase hexadecimal state");
    return int(p);
}
inline int activity(const std::string &x) {
    return int(std::count_if(x.begin(), x.end(), [](char c) { return c != '0'; }));
}
inline J select_fields(const J &x, std::initializer_list<const char *> keys) {
    J r = J::object();
    for (auto k : keys)
        if (x.contains(k))
            r[k] = x[k];
    return r;
}

class Model {
    mutable std::mutex mutex;
    mutable std::list<std::pair<std::string, J>> cats, families;
    mutable std::map<std::string, std::vector<std::vector<std::pair<int, unsigned>>>> relation_cache;

  public:
    int R;
    int dt[16][16]{};
    explicit Model(int r) : R(r) {
        int s[] = {12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2};
        for (int a = 0; a < 16; ++a)
            for (int x = 0; x < 16; ++x)
                ++dt[a][s[x] ^ s[x ^ a]];
    }
    static int perm(int i) { return i == 63 ? 63 : 16 * i % 63; }
    static std::vector<int> parse(const std::string &s) {
        require(s.size() == 16, "Expected 16 uppercase hexadecimal digits");
        std::vector<int> x(16);
        for (int c = 0; c < 16; ++c)
            x[c] = nibble(s[15 - c]);
        return x;
    }
    static std::string encode(const std::vector<int> &x) {
        require(x.size() == 16, "Invalid PRESENT state");
        std::string s(16, '0');
        for (int c = 0; c < 16; ++c) {
            require(x[c] >= 0 && x[c] < 16, "Invalid PRESENT nibble");
            s[15 - c] = "0123456789ABCDEF"[x[c]];
        }
        return s;
    }
    static std::vector<int> linear(const std::vector<int> &x, bool inverse = false) {
        std::vector<int> y(16);
        for (int i = 0; i < 64; ++i) {
            int dst = perm(i);
            if (inverse)
                y[i / 4] ^= ((x[dst / 4] >> (dst % 4)) & 1) << (i % 4);
            else
                y[dst / 4] ^= ((x[i / 4] >> (i % 4)) & 1) << (dst % 4);
        }
        return y;
    }
    J pattern(const J &states) const {
        require(int(states.size()) == R + 1, "Wrong number of complete-round states");
        J p = J::array();
        for (int r = 0; r < R; ++r) {
            auto x = parse(states[r]);
            std::string s;
            for (int v : x)
                s += v ? '1' : '0';
            p.push_back(s);
        }
        return p;
    }
    static int expression(const J &terms, const std::vector<int> &x) {
        int v = 0;
        for (const auto &t : terms)
            v ^= ((x.at(t[0].get<int>()) >> t[1].get<int>()) & 1) << t[2].get<int>();
        return v;
    }
    J prepare(const J &p) const {
        require(p.is_array() && int(p.size()) == R, "Pattern round count mismatch");
        for (const auto &row : p) {
            std::string s = row;
            require(s.size() == 16 && s.find_first_not_of("01") == std::string::npos &&
                        s.find('1') != std::string::npos,
                    "Malformed activity pattern");
        }
        J labels = J::array(), domains = J::array(), factors = J::array(), cuts = J::array();
        for (int side = 0; side < 2; ++side)
            for (int c = 0; c < 16; ++c)
                if (p[side ? R - 1 : 0].get<std::string>()[c] == '1')
                    labels.push_back(J::array({side ? R : 0, c}));
        int ne = int(labels.size());
        for (int r = 1; r < R; ++r) {
            int n = 0;
            for (int c = 0; c < 16; ++c)
                if (p[r].get<std::string>()[c] == '1') {
                    labels.push_back(J::array({r, c}));
                    ++n;
                }
            cuts.push_back(n);
        }
        std::map<std::pair<int, int>, int> index;
        for (int i = 0; i < int(labels.size()); ++i)
            index[{labels[i][0], labels[i][1]}] = i;
        for (const auto &l : labels) {
            int r = l[0], c = l[1], allowed = 15;
            if (r > 0 && r < R) {
                allowed = 0;
                for (int src = 0; src < 64; ++src)
                    if (perm(src) / 4 == c && p[r - 1].get<std::string>()[src / 4] == '1')
                        allowed |= 1 << (perm(src) % 4);
            }
            J d = J::array();
            for (int v = 1; v < 16; ++v)
                if (!(v & ~allowed))
                    d.push_back(v);
            require(!d.empty(), "Active nibble has no incoming bit");
            domains.push_back(d);
        }
        for (int r = 0; r < R; ++r)
            for (int c = 0; c < 16; ++c)
                if (p[r].get<std::string>()[c] == '1') {
                    J a = J::array(), b = J::array();
                    for (int k = 0; k < 4; ++k) {
                        a.push_back(J::array({index.at({r, c}), k, k}));
                        if (r == R - 1)
                            b.push_back(J::array({index.at({R, c}), k, k}));
                        else {
                            auto it = index.find({r + 1, perm(4 * c + k) / 4});
                            if (it != index.end())
                                b.push_back(J::array({it->second, perm(4 * c + k) % 4, k}));
                        }
                    }
                    require(!b.empty(), "Active S-box output forced zero");
                    factors.push_back(J{{"round", r}, {"cell", c}, {"a", a}, {"b", b}});
                }
        return J{{"id", digest(p)},          {"activity", p},
                 {"labels", labels},         {"nend", ne},
                 {"domains", domains},       {"d", int(labels.size()) - ne},
                 {"active", factors.size()}, {"factors", factors},
                 {"cut_dimensions", cuts}};
    }
    std::vector<std::vector<std::pair<int, unsigned>>> relations(const J &f) const {
        std::string key = digest(J::array({f["a"], f["b"]}));
        {
            std::lock_guard<std::mutex> l(mutex);
            auto it = relation_cache.find(key);
            if (it != relation_cache.end())
                return it->second;
        }
        std::vector<std::vector<std::pair<int, unsigned>>> rows;
        int am = 0, bm = 0;
        for (const auto &t : f["a"])
            am += 1 << t[2].get<int>();
        for (const auto &t : f["b"])
            bm += 1 << t[2].get<int>();
        for (int a = 1; a < 16; ++a)
            if (!(a & ~am))
                for (int b = 1; b < 16; ++b)
                    if (!(b & ~bm) && dt[a][b]) {
                        std::map<int, std::pair<int, int>> req;
                        bool valid = true;
                        for (auto side : {std::make_pair("a", a), std::make_pair("b", b)})
                            for (const auto &t : f[side.first]) {
                                int var = t[0], src = t[1], dst = t[2],
                                    bit = ((side.second >> dst) & 1) << src;
                                auto [mask, bits] = req[var];
                                if ((mask & (1 << src)) && ((bits & (1 << src)) != bit))
                                    valid = false;
                                req[var] = {mask | (1 << src), bits | bit};
                            }
                        if (valid) {
                            std::vector<std::pair<int, unsigned>> row;
                            for (auto [v, mb] : req) {
                                unsigned bits = 0;
                                for (int x = 1; x < 16; ++x)
                                    if ((x & mb.first) == mb.second)
                                        bits |= 1u << x;
                                row.push_back({v, bits});
                            }
                            rows.push_back(row);
                        }
                    }
        {
            std::lock_guard<std::mutex> l(mutex);
            if (relation_cache.size() >= 2048)
                relation_cache.clear();
            relation_cache[key] = rows;
        }
        return rows;
    }
    J support(const J &cat, const std::string &endpoint = "", const Deadline &dl = {}) const {
        std::vector<unsigned> ds;
        for (const auto &d : cat["domains"]) {
            unsigned m = 0;
            for (int v : d)
                m += 1u << v;
            ds.push_back(m);
        }
        if (!endpoint.empty()) {
            require(int(endpoint.size()) == iv(cat, "nend"), "Wrong endpoint length");
            for (int i = 0; i < int(endpoint.size()); ++i)
                ds[i] &= 1u << nibble(endpoint[i]);
        }
        for (auto m : ds)
            if (!m)
                return nullptr;
        std::vector<std::vector<std::vector<std::pair<int, unsigned>>>> rs;
        for (const auto &f : cat["patterns"][0]["factors"])
            rs.push_back(relations(f));
        bool changed = true;
        while (changed) {
            dl.check();
            changed = false;
            for (const auto &rows : rs) {
                if (rows.empty())
                    return nullptr;
                std::map<int, unsigned> supported;
                for (auto [v, m] : rows[0]) {
                    (void)m;
                    supported[v] = 0;
                }
                for (const auto &row : rows) {
                    bool good = true;
                    for (auto [v, m] : row)
                        if (!(ds[v] & m)) {
                            good = false;
                            break;
                        }
                    if (good)
                        for (auto [v, m] : row)
                            supported[v] |= ds[v] & m;
                }
                for (auto [v, m] : supported) {
                    if (!m)
                        return nullptr;
                    if (ds[v] != m) {
                        ds[v] = m;
                        changed = true;
                    }
                }
            }
        }
        J out = J::array();
        for (auto mask : ds) {
            J d = J::array();
            for (int x = 1; x < 16; ++x)
                if (mask & (1u << x))
                    d.push_back(x);
            out.push_back(d);
        }
        return out;
    }
    J catalog(const J &patterns) const {
        std::set<J> unique(patterns.begin(), patterns.end());
        require(unique.size() == 1, "CPU proposal catalog requires one activity pattern");
        J p = *unique.begin();
        std::string key = digest(p);
        {
            std::lock_guard<std::mutex> l(mutex);
            for (auto it = cats.begin(); it != cats.end(); ++it)
                if (it->first == key) {
                    J out = it->second;
                    cats.splice(cats.begin(), cats, it);
                    return out;
                }
        }
        J m = prepare(p);
        int ne = m["nend"], n = int(m["labels"].size());
        std::set<std::vector<int>> pairs, triples;
        for (const auto &f : m["factors"]) {
            std::set<int> vars;
            for (auto side : {"a", "b"})
                for (const auto &t : f[side])
                    if (t[0].get<int>() >= ne)
                        vars.insert(t[0].get<int>());
            std::vector<int> v(vars.begin(), vars.end());
            for (std::size_t i = 0; i < v.size(); ++i)
                for (std::size_t j = i + 1; j < v.size(); ++j) {
                    pairs.insert({v[i], v[j]});
                    for (std::size_t k = j + 1; k < v.size(); ++k)
                        triples.insert({v[i], v[j], v[k]});
                }
        }
        J mm = select_fields(m, {"id", "activity", "active", "d", "factors", "cut_dimensions"}),
          used = J::array(), blocks = J::array();
        for (int i = ne; i < n; ++i)
            used.push_back(i);
        for (const auto &b : pairs) {
            if (blocks.size() >= 256)
                break;
            blocks.push_back(b);
        }
        int k = 0;
        for (const auto &b : triples) {
            if (k++ == 16)
                break;
            blocks.push_back(b);
        }
        mm["used"] = used;
        mm["blocks"] = blocks;
        J cat = {{"schema", "PRESENT_MF" + std::to_string(R) + "_V2"},
                 {"rounds", R},
                 {"nend", ne},
                 {"padded_dimension", n - ne},
                 {"labels", m["labels"]},
                 {"domains", m["domains"]},
                 {"patterns", J::array({mm})},
                 {"model", "independent_round_key_DDT_product"},
                 {"output_activity_fixed_during_search", false},
                 {"original_DDT_factors_preserved", true}};
        J ds = support(cat);
        require(!ds.is_null(), "Activity pattern has no exact local DDT support");
        cat["domains"] = ds;
        cat["support_propagation"] = "full-factor generalized arc consistency; legal target unchanged";
        cat["id"] = digest(cat);
        {
            std::lock_guard<std::mutex> l(mutex);
            cats.push_front({key, cat});
            if (cats.size() > 128)
                cats.pop_back();
        }
        return cat;
    }
    J endpoints(const J &cat, const std::string &text) const {
        require(int(text.size()) == iv(cat, "nend") &&
                    text.find_first_not_of("123456789ABCDEF") == std::string::npos,
                "Invalid endpoint coordinates");
        std::vector<int> x(16), y(16);
        for (int i = 0; i < int(text.size()); ++i) {
            auto l = cat["labels"][i];
            (l[0] == 0 ? x : y)[l[1].get<int>()] = nibble(text[i]);
        }
        return J{{"input", encode(x)}, {"output", encode(linear(y))}, {"last_sbox_output", encode(y)}};
    }
    J terminal_weight(const J &cat, const std::string &e, const std::string &in) const {
        std::vector<int> x;
        for (char c : e + in)
            x.push_back(nibble(c));
        require(x.size() == cat["domains"].size(), "Malformed terminal coordinates");
        for (std::size_t i = 0; i < x.size(); ++i)
            require(has(cat["domains"][i], x[i]), "Forced-zero-bit violation");
        int w = 0;
        for (const auto &f : cat["patterns"][0]["factors"]) {
            int a = expression(f["a"], x), b = expression(f["b"], x), count = a && b ? dt[a][b] : 0;
            if (!count)
                return nullptr;
            require(count == 2 || count == 4, "Unexpected active DDT count");
            w += count == 4 ? 2 : 3;
        }
        return w;
    }
    J decode(const J &cat, const std::string &e, const std::string &in) const {
        J w = terminal_weight(cat, e, in);
        require(!w.is_null(), "Expected legal DDT witness");
        std::vector<int> x;
        for (char c : e + in)
            x.push_back(nibble(c));
        std::vector<std::vector<int>> xx(R, std::vector<int>(16)), yy = xx;
        for (const auto &f : cat["patterns"][0]["factors"]) {
            xx[f["round"].get<int>()][f["cell"].get<int>()] = expression(f["a"], x);
            yy[f["round"].get<int>()][f["cell"].get<int>()] = expression(f["b"], x);
        }
        xx.push_back(linear(yy.back()));
        for (int r = 0; r < R; ++r)
            require(linear(yy[r]) == xx[r + 1], "Trail violates PRESENT permutation");
        J states = J::array();
        for (const auto &row : xx)
            states.push_back(encode(row));
        J ep = endpoints(cat, e);
        require(states[0] == ep["input"] && states.back() == ep["output"], "Endpoint mismatch");
        return J{{"states", states}, {"weight", w}, {"pattern", pattern(states)}};
    }
    J encode_trail(const J &cat, const J &states) const {
        require(int(states.size()) == R + 1, "Wrong-round witness");
        require(pattern(states) == cat["patterns"][0]["activity"], "Witness has a different pattern");
        std::vector<std::vector<int>> xx;
        for (const auto &s : states)
            xx.push_back(parse(s));
        auto last = linear(xx.back(), true);
        std::string text;
        for (const auto &l : cat["labels"]) {
            int r = l[0], c = l[1];
            text += "0123456789ABCDEF"[(r == R ? last : xx[r])[c]];
        }
        for (std::size_t i = 0; i < text.size(); ++i)
            require(has(cat["domains"][i], nibble(text[i])), "Witness outside exact bit support");
        int ne = cat["nend"];
        auto e = text.substr(0, ne), in = text.substr(ne);
        J checked = decode(cat, e, in);
        require(checked["states"] == states, "Coordinate encoding changed trail");
        int w = checked["weight"];
        return J{{"coordinates", e},
                 {"witness_internal", in},
                 {"witness_pattern", 0},
                 {"witness_weight", w},
                 {"endpoints", endpoints(cat, e)},
                 {"witness_numerator", pow2(3 * iv(cat["patterns"][0], "active") - w).get_str()}};
    }
    J reduction(const J &cat, const std::string &e, const Deadline &dl = {}) const {
        endpoints(cat, e);
        J ds = support(cat, e, dl);
        if (ds.is_null())
            return J{{"proven_zero", true}, {"reason", "empty exact DDT support relation"}};
        J d = J::object();
        for (int v : cat["patterns"][0]["used"])
            d[std::to_string(v)] = ds[v];
        return J{{"proven_zero", false},
                 {"domains", d},
                 {"catalog_id", cat["id"]},
                 {"pattern", 0},
                 {"endpoint", e},
                 {"reason", "full-factor DDT support propagation; all original factors retained"}};
    }
    std::string native_text(const J &cat) const {
        std::ostringstream o;
        o << "PRESENT_MF" << R << "_V2 " << cat["nend"] << ' ' << cat["padded_dimension"] << " 1\n";
        for (const auto &d : cat["domains"]) {
            o << d.size();
            for (int x : d)
                o << ' ' << x;
            o << '\n';
        }
        const auto &m = cat["patterns"][0];
        o << m["id"].get<std::string>() << ' ' << m["active"] << ' ' << m["d"] << ' ' << m["factors"].size()
          << ' ' << m["blocks"].size() << '\n';
        bool first = true;
        for (int x : m["used"]) {
            if (!first)
                o << ' ';
            o << x;
            first = false;
        }
        o << '\n';
        for (const auto &f : m["factors"]) {
            o << f["round"] << ' ' << f["cell"];
            for (auto side : {"a", "b"}) {
                o << ' ' << f[side].size();
                for (const auto &t : f[side])
                    for (int x : t)
                        o << ' ' << x;
            }
            o << '\n';
        }
        for (const auto &b : m["blocks"]) {
            o << b.size();
            for (int x : b)
                o << ' ' << x;
            o << '\n';
        }
        return o.str();
    }
    J compile(const J &states, const Deadline &dl = {}) const {
        std::string key = digest(states);
        {
            std::lock_guard<std::mutex> l(mutex);
            for (auto it = families.begin(); it != families.end(); ++it)
                if (it->first == key) {
                    J out = it->second;
                    families.splice(families.begin(), families, it);
                    return out;
                }
        }
        J cat = catalog(J::array({pattern(states)})), w = encode_trail(cat, states),
          reduced = reduction(cat, w["coordinates"], dl);
        require(!truth(get(reduced, "proven_zero")), "Witnessed family proved empty");
        J used = cat["patterns"][0]["used"], ds = J::array(), factors = J::array(), witness = J::array();
        std::map<int, int> remap;
        std::vector<int> digits;
        std::string all = w["coordinates"].get<std::string>() + w["witness_internal"].get<std::string>();
        for (char ch : all)
            digits.push_back(nibble(ch));
        for (int v : used) {
            remap[v] = int(ds.size());
            ds.push_back(reduced["domains"][std::to_string(v)]);
            require(has(ds.back(), digits[v]), "Domain propagation excluded a witness");
            witness.push_back(digits[v]);
        }
        for (const auto &f : cat["patterns"][0]["factors"]) {
            J g = J::object();
            for (auto side : {"a", "b"}) {
                int constant = 0;
                std::vector<std::array<int, 3>> terms;
                for (const auto &t : f[side]) {
                    int v = t[0], src = t[1], dst = t[2];
                    if (v < iv(cat, "nend"))
                        constant ^= ((digits[v] >> src) & 1) << dst;
                    else
                        terms.push_back({remap.at(v), src, dst});
                }
                std::sort(terms.begin(), terms.end());
                g[side] = constant;
                g[std::string(side) + "v"] = terms;
            }
            factors.push_back(g);
        }
        J out = {{"nv", ds.size()},
                 {"active", cat["patterns"][0]["active"]},
                 {"denominator_power", 3 * iv(cat["patterns"][0], "active")},
                 {"domains", ds},
                 {"factors", factors},
                 {"witness", witness},
                 {"witness_weight", w["witness_weight"]},
                 {"witness_numerator", w["witness_numerator"]}};
        {
            std::lock_guard<std::mutex> l(mutex);
            families.push_front({key, out});
            if (families.size() > 96)
                families.pop_back();
        }
        return out;
    }
    std::string adaptive_text(const J &model, const J &boxes) const {
        std::ostringstream o;
        o << "PRESENT_ADAPTIVE" << R << "_V1 " << model["nv"] << ' ' << model["active"] << ' ' << boxes.size()
          << '\n';
        for (const auto &d : model["domains"]) {
            o << d.size();
            for (int x : d)
                o << ' ' << x;
            o << '\n';
        }
        bool first = true;
        for (int v : model["witness"]) {
            if (!first)
                o << ' ';
            o << v;
            first = false;
        }
        o << '\n';
        for (const auto &f : model["factors"]) {
            o << f["a"] << ' ' << f["b"];
            for (auto side : {"av", "bv"}) {
                o << ' ' << f[side].size();
                for (const auto &t : f[side])
                    for (int x : t)
                        o << ' ' << x;
            }
            o << '\n';
        }
        for (const auto &box : boxes) {
            o << box.size();
            for (const auto &p : box)
                o << ' ' << p[0] << ' ' << p[1].get<std::string>();
            o << '\n';
        }
        return o.str();
    }
    static std::vector<unsigned> box_domains(const J &model, const J &box) {
        std::vector<unsigned> ds;
        for (const auto &d : model["domains"]) {
            unsigned mask = 0;
            for (int v : d)
                mask += 1u << v;
            ds.push_back(mask);
        }
        std::set<int> seen;
        for (const auto &p : box) {
            require(p.is_array() && p.size() == 2 && p[0].is_number_integer() && p[1].is_string(),
                    "Malformed adaptive variable");
            int v = p[0];
            require(v >= 0 && v < int(ds.size()) && seen.insert(v).second,
                    "Invalid/repeated adaptive variable");
            auto text = p[1].get<std::string>();
            require(!text.empty() && text.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos,
                    "Invalid adaptive subdomain");
            Z mask(text, 16);
            require(mask > 0 && mask <= 65535, "Invalid adaptive subdomain");
            unsigned m = mask.get_ui();
            require(!(m & ~ds[v]), "Adaptive subdomain escaped its parent");
            ds[v] = m;
        }
        return ds;
    }
    static Z volume(const std::vector<unsigned> &ds) {
        Z n = 1;
        for (auto m : ds)
            n *= __builtin_popcount(m);
        return n;
    }
    static void verify(const J &model, const J &parent, const J &result) {
        auto old = box_domains(model, parent);
        Z total = volume(old), remain = 0;
        std::vector<std::vector<unsigned>> children;
        for (const auto &box : result["frontier"]) {
            auto d = box_domains(model, box);
            for (std::size_t v = 0; v < d.size(); ++v)
                require(!(d[v] & ~old[v]), "Checkpoint escaped input box");
            for (const auto &other : children) {
                bool disjoint = false;
                for (std::size_t v = 0; v < d.size(); ++v)
                    if (!(d[v] & other[v])) {
                        disjoint = true;
                        break;
                    }
                require(disjoint, "Overlapping adaptive checkpoints");
            }
            remain += volume(d);
            children.push_back(std::move(d));
        }
        Z closed = zi(result["closed_volume"]), n = zi(result["numerator"]);
        require(closed >= 0 && zi(result["initial_volume"]) == total && closed + remain == total,
                "Checkpoint assignment accounting failed");
        require(n >= 0 && result["denominator_power"] == model["denominator_power"],
                "Invalid exact checkpoint mass");
        require(result["status"] != "EXACT_DONE" || children.empty(),
                "Incomplete checkpoint marked complete");
        require(closed != 0 || n == 0, "Positive mass without closed assignment");
    }
    J singleton(const J &m) const {
        for (const auto &d : m["domains"])
            if (d.size() != 1)
                return nullptr;
        std::vector<int> values;
        for (const auto &d : m["domains"])
            values.push_back(d[0]);
        Z n = 1;
        for (const auto &f : m["factors"]) {
            int a = f["a"], b = f["b"];
            a ^= expression(f["av"], values);
            b ^= expression(f["bv"], values);
            int k = dt[a][b];
            require(a && b && (k == 2 || k == 4), "Singleton domain contradicts witness");
            n *= k / 2;
        }
        require(n == zi(m["witness_numerator"]), "Singleton product disagrees with witness");
        J proof = {{"status", "EXACT_DONE"},
                   {"numerator", n.get_str()},
                   {"denominator_power", m["denominator_power"]},
                   {"initial_volume", "1"},
                   {"closed_volume", "1"},
                   {"frontier", J::array()},
                   {"solved_nodes", 1},
                   {"zero_nodes", 0},
                   {"component_cache_hits", 0},
                   {"reason", "SINGLE_ASSIGNMENT"}};
        verify(m, J::array(), proof);
        return J{{"status", "PLAN"},
                 {"operation_score", 0},
                 {"estimated_table_bytes", 0},
                 {"initial_volume", "1"},
                 {"cell_bytes", 0},
                 {"branch", -1},
                 {"components", 0},
                 {"fits_leaf_budget", true},
                 {"unit_exact", proof},
                 {"witness_weight", m["witness_weight"]}};
    }
    J corridor_record(const J &row) const {
        int cap = std::stoi(str(row["cap"]));
        J states = row["states"].is_array() ? row["states"] : J(split(row["states"], ','));
        require(cap >= 1 && cap <= 16 && int(states.size()) == R + 1, "Bad transfer scope");
        require(states[0] == row["input"] && states.back() == row["output"],
                "Transfer witness endpoints mismatch");
        std::vector<std::vector<int>> xx;
        for (const auto &s : states)
            xx.push_back(parse(s));
        require(activity(states[0]) > 0 && activity(states.back()) > 0, "Zero transfer endpoint");
        for (int r = 1; r < R; ++r)
            require(activity(states[r]) <= cap, "Transfer witness outside corridor");
        int w = 0;
        for (int r = 0; r < R; ++r) {
            auto y = linear(xx[r + 1], true);
            for (int c = 0; c < 16; ++c) {
                int n = dt[xx[r][c]][y[c]];
                require(n == 2 || n == 4 || n == 16, "Illegal transfer transition");
                w += n == 2 ? 3 : n == 4 ? 2 : 0;
            }
        }
        int den = std::stoi(str(row["denominator_power"]));
        Z n = zi(row["numerator"]);
        require(den == 3 * activity(states[0]) + 3 * cap * (R - 1) && den >= w && n >= pow2(den - w) &&
                    n <= pow2(den),
                "Transfer integer mass/denominator failure");
        require(std::stoi(str(row["trail_weight"])) == w, "Transfer witness weight mismatch");
        return J{{"input", states[0]},
                 {"output", states.back()},
                 {"cap", cap},
                 {"states", states},
                 {"mass", dyadic(n, den)},
                 {"witness_weight", w},
                 {"scope", "all trails with ASN(X_r)<=cap for r=1..R-1; final P included"},
                 {"exact", true},
                 {"first_seconds", std::stod(str(row["first_seconds"]))}};
    }
};
}
