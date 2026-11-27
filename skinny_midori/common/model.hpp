#pragma once
#include "message_cache.hpp"
#include "core.hpp"

namespace thermo {
using Domain = std::vector<int>;
using Domains = std::vector<Domain>;
using Pattern = std::vector<std::string>;
struct Term {
    int v = 0, src = -1, dst = 0;
    bool operator<(const Term &o) const {
        return std::tie(v, src, dst) < std::tie(o.v, o.src, o.dst);
    }
};
struct Factor {
    int round = 0, cell = 0, type = 0, a = 0, b = 0;
    std::vector<Term> av, bv;
};
struct PairSet {
    std::vector<std::uint64_t> w;
    explicit PairSet(int bits = 256) : w((bits + 63) / 64) {}
    void set(unsigned x) {
        w.at(x >> 6) |= 1ULL << (x & 63);
    }
    bool intersects(const PairSet &o) const {
        for (std::size_t i = 0; i < w.size(); ++i)
            if (w[i] & o.w[i])
                return true;
        return false;
    }
    bool full() const {
        for (auto x : w)
            if (x != UINT64_MAX)
                return false;
        return true;
    }
    PairSet &operator|=(const PairSet &o) {
        for (std::size_t i = 0; i < w.size(); ++i)
            w[i] |= o.w[i];
        return *this;
    }
    PairSet shift(unsigned v) const {
        static constexpr std::uint64_t low[] = {0x5555555555555555ULL, 0x3333333333333333ULL,
                                                0x0F0F0F0F0F0F0F0FULL, 0x00FF00FF00FF00FFULL,
                                                0x0000FFFF0000FFFFULL, 0x00000000FFFFFFFFULL};
        PairSet out(int(w.size() * 64));
        for (unsigned i = 0; i < w.size(); ++i) {
            auto x = w[i];
            for (unsigned k = 0; k < 6; ++k)
                if (v & (1u << k)) {
                    auto n = 1u << k;
                    x = ((x & low[k]) << n) | ((x >> n) & low[k]);
                }
            out.w[i ^ (v >> 6)] = x;
        }
        return out;
    }
};
struct Model {
    Pattern pattern;
    std::string id;
    int nend = 0, nv = 0, active = 0;
    std::vector<std::array<int, 2>> labels;
    std::vector<Factor> factors;
    Domains domains;
    std::vector<std::vector<int>> blocks;
    std::vector<int> cuts;
};
struct Family {
    Model model;
    std::string endpoint_coordinates, internal_coordinates;
    std::vector<std::string> states;
    Domains domains;
    std::vector<Factor> factors;
    std::vector<int> witness;
    Z witness_num;
    double weight = 0;
    unsigned exponent = 0;
    J root_bound;
};

struct JointCatalog {
    std::string text;
    int nend = 0, dimension = 0;
    std::vector<std::vector<int>> internal_map;
};

class Cipher {
  public:
    J cfg;
    int R, B, Q, DIV, DB;
    bool present, midori;
    std::vector<int> sr, sbox;
    std::vector<std::vector<int>> ddt;
    std::vector<PairSet> allowed;
    std::vector<std::vector<std::pair<int, PairSet>>> levels;
    explicit Cipher(const J &c)
        : cfg(c), R(c.at("rounds")), B(c.at("cell_bits")), Q(1 << B), DIV(c.at("ddt_divisor")),
          DB(c.at("denominator_bits")), present(c.at("present")), midori(c.at("midori")),
          sr(c.at("SR").get<std::vector<int>>()), sbox(c.at("SBOX").get<std::vector<int>>()) {
        int types = midori && B == 8 ? 4 : 1;
        ddt.resize(types, std::vector<int>(Q * Q));
        if (types == 4) {
            auto small = cfg.at("SB1").get<std::vector<int>>();
            auto perms = cfg.at("BIT_PERM").get<std::vector<std::vector<int>>>();
            int st[16][16]{};
            for (int a = 0; a < 16; ++a)
                for (int x = 0; x < 16; ++x)
                    ++st[a][small[x] ^ small[x ^ a]];
            for (int t = 0; t < 4; ++t) {
                auto perm = [&](int x) {
                    int z = 0;
                    for (int j = 0; j < 8; ++j)
                        z |= ((x >> (7 - perms[t][j])) & 1) << (7 - j);
                    return z;
                };
                for (int a = 0; a < Q; ++a)
                    for (int b = 0; b < Q; ++b) {
                        int u = perm(a), v = perm(b);
                        ddt[t][a * Q + b] = st[u >> 4][v >> 4] * st[u & 15][v & 15];
                    }
            }
        } else {
            require(int(sbox.size()) == Q, "S-box size mismatch");
            for (int a = 0; a < Q; ++a)
                for (int x = 0; x < Q; ++x)
                    ++ddt[0][a * Q + (sbox[x] ^ sbox[x ^ a])];
        }
        for (int t = 0; t < types; ++t) {
            PairSet good(Q * Q);
            std::map<int, PairSet, std::greater<int>> ls;
            for (int a = 1; a < Q; ++a)
                for (int b = 1; b < Q; ++b) {
                    int n = ddt[t][a * Q + b];
                    require(n % DIV == 0, "DDT divisor mismatch");
                    if (n) {
                        good.set(a | (b << B));
                        auto it = ls.try_emplace(n / DIV, Q * Q).first;
                        it->second.set(a | (b << B));
                    }
                }
            allowed.push_back(good);
            levels.emplace_back();
            for (auto &kv : ls)
                levels.back().push_back(kv);
        }
    }
    int kind(int cell) const {
        return midori && B == 8 ? cell % 4 : 0;
    }
    int count(int t, int a, int b) const {
        return ddt.at(t).at(a * Q + b);
    }
    int entry(int t, int a, int b) const {
        return a && b ? count(t, a, b) / DIV : 0;
    }
    std::vector<int> parse(const std::string &text) const {
        auto s = upper(text);
        require(s.size() == std::size_t(16 * (B / 4)) &&
                    s.find_first_not_of("0123456789ABCDEF") == std::string::npos,
                "Malformed full state");
        std::vector<int> x(16);
        for (int i = 0; i < 16; ++i) {
            int j = present ? 15 - i : i;
            x[i] = std::stoi(s.substr(j * (B / 4), B / 4), nullptr, 16);
        }
        return x;
    }
    std::string encode(const std::vector<int> &x) const {
        require(x.size() == 16, "State size mismatch");
        std::ostringstream s;
        s << std::uppercase << std::hex << std::setfill('0');
        for (int i = 0; i < 16; ++i) {
            int j = present ? 15 - i : i;
            require(x[j] >= 0 && x[j] < Q, "State cell outside range");
            s << std::setw(B / 4) << x[j];
        }
        return s.str();
    }
    static int pbox(int i) {
        return i == 63 ? 63 : 16 * i % 63;
    }
    template <class T> std::vector<T> linear(const std::vector<T> &x, bool inverse = false) const {
        std::vector<T> y(16), z(16);
        if (midori) {
            if (inverse) {
                for (int c = 0; c < 4; ++c) {
                    T total = x[4 * c] ^ x[4 * c + 1] ^ x[4 * c + 2] ^ x[4 * c + 3];
                    for (int r = 0; r < 4; ++r)
                        y[sr[4 * c + r]] = total ^ x[4 * c + r];
                }
                return y;
            }
            for (int i = 0; i < 16; ++i)
                y[i] = x[sr[i]];
            for (int c = 0; c < 4; ++c) {
                T total = y[4 * c] ^ y[4 * c + 1] ^ y[4 * c + 2] ^ y[4 * c + 3];
                for (int r = 0; r < 4; ++r)
                    z[4 * c + r] = total ^ y[4 * c + r];
            }
            return z;
        }
        if (inverse) {
            for (int c = 0; c < 4; ++c) {
                y[sr[c]] = x[4 + c];
                y[sr[4 + c]] = x[8 + c] ^ x[12 + c] ^ x[4 + c];
                y[sr[8 + c]] = x[12 + c] ^ x[4 + c];
                y[sr[12 + c]] = x[c] ^ x[12 + c];
            }
            return y;
        }
        for (int i = 0; i < 16; ++i)
            y[i] = x[sr[i]];
        for (int c = 0; c < 4; ++c) {
            z[c] = y[c] ^ y[8 + c] ^ y[12 + c];
            z[4 + c] = y[c];
            z[8 + c] = y[4 + c] ^ y[8 + c];
            z[12 + c] = y[c] ^ y[8 + c];
        }
        return z;
    }
    std::vector<int> forward(const std::vector<int> &x) const {
        if (!present)
            return linear(x);
        std::vector<int> y(16);
        for (int i = 0; i < 64; ++i)
            y[pbox(i) / 4] ^= ((x[i / 4] >> (i % 4)) & 1) << (pbox(i) % 4);
        return y;
    }
    std::vector<int> inverse(const std::vector<int> &x) const {
        if (!present)
            return linear(x, true);
        std::vector<int> y(16);
        for (int i = 0; i < 64; ++i)
            y[i / 4] ^= ((x[pbox(i) / 4] >> (pbox(i) % 4)) & 1) << (i % 4);
        return y;
    }
    Pattern activity(const std::vector<std::string> &s) const {
        require(int(s.size()) == R + 1, "Wrong witness round count");
        Pattern p;
        for (int r = 0; r < R; ++r) {
            std::string row;
            for (auto x : parse(s[r]))
                row += x ? '1' : '0';
            require(row.find('1') != std::string::npos, "Zero trail endpoint/activity");
            p.push_back(row);
        }
        parse(s.back());
        return p;
    }
    static std::vector<int> bits(const Z &z) {
        std::vector<int> out;
        require(z >= 0, "Negative bit mask");
        for (mp_bitcnt_t b = mpz_scan1(z.get_mpz_t(), 0); b != std::numeric_limits<mp_bitcnt_t>::max();
             b = mpz_scan1(z.get_mpz_t(), b + 1))
            out.push_back(int(b));
        return out;
    }
    static int eval(const std::vector<Term> &t, const std::vector<int> &x) {
        int z = 0;
        for (auto e : t)
            z ^= e.src < 0 ? x.at(e.v) : ((x.at(e.v) >> e.src) & 1) << e.dst;
        return z;
    }
    int contribution(const Factor &f, int v, int value) const {
        int a = 0, b = 0;
        for (auto t : f.av)
            if (t.v == v)
                a ^= t.src < 0 ? value : ((value >> t.src) & 1) << t.dst;
        for (auto t : f.bv)
            if (t.v == v)
                b ^= t.src < 0 ? value : ((value >> t.src) & 1) << t.dst;
        return a | (b << B);
    }
    static std::vector<int> scope(const Factor &f) {
        std::set<int> s;
        for (auto t : f.av)
            s.insert(t.v);
        for (auto t : f.bv)
            s.insert(t.v);
        return {s.begin(), s.end()};
    }
    PairSet extend(const PairSet &in, const Domain &d, const Factor &f, int v, const Deadline &dl) const {
        if (in.full() && !d.empty())
            return in;
        PairSet out(Q * Q);
        int n = 0;
        for (int x : d) {
            if ((n++ & 31) == 0)
                dl.check();
            out |= in.shift(contribution(f, v, x));
            if (out.full())
                break;
        }
        return out;
    }
    bool propagate(Domains &ds, const std::vector<Factor> &fs, const Deadline &dl = {}) const {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &f : fs) {
                dl.check();
                auto vars = scope(f);
                std::vector<PairSet> pre(vars.size() + 1, PairSet(Q * Q));
                pre[0].set(0);
                for (std::size_t i = 0; i < vars.size(); ++i) {
                    if (ds[vars[i]].empty())
                        return false;
                    pre[i + 1] = extend(pre[i], ds[vars[i]], f, vars[i], dl);
                }
                auto back = allowed[f.type].shift(f.a | (f.b << B));
                if (!pre.back().intersects(back))
                    return false;
                for (std::size_t i = vars.size(); i-- > 0;) {
                    if (back.full())
                        break;
                    Domain keep;
                    PairSet prev(Q * Q);
                    int n = 0;
                    for (int x : ds[vars[i]]) {
                        if ((n++ & 31) == 0)
                            dl.check();
                        auto possible = back.shift(contribution(f, vars[i], x));
                        if (pre[i].intersects(possible)) {
                            keep.push_back(x);
                            prev |= possible;
                        }
                    }
                    if (keep.empty())
                        return false;
                    if (keep != ds[vars[i]]) {
                        ds[vars[i]] = std::move(keep);
                        changed = true;
                    }
                    back = std::move(prev);
                }
            }
        }
        return true;
    }
    Model compile(const Pattern &p, const Deadline &dl = {}) const {
        require(int(p.size()) == R, "Pattern round count mismatch");
        for (auto &s : p)
            require(s.size() == 16 && s.find_first_not_of("01") == std::string::npos &&
                        s.find('1') != std::string::npos,
                    "Malformed pattern");
        Model m;
        m.pattern = p;
        m.id = digest(p);
        std::vector<std::vector<Z>> forms;
        std::vector<Z> first(16), last(16);
        for (int side = 0; side < 2; ++side)
            for (int c = 0; c < 16; ++c)
                if (p[side ? R - 1 : 0][c] == '1') {
                    auto &row = side ? last : first;
                    row[c] = pow2(m.labels.size());
                    m.labels.push_back({side ? R : 0, c});
                }
        m.nend = int(m.labels.size());
        forms.push_back(first);
        if (!present) {
            for (int r = 1; r < R; ++r) {
                dl.check();
                std::vector<int> cells;
                for (int c = 0; c < 16; ++c)
                    if (p[r][c] == '1')
                        cells.push_back(c);
                std::vector<Z> sym(16);
                for (int j = 0; j < int(cells.size()); ++j)
                    sym[cells[j]] = pow2(j);
                auto inv = linear(sym, true);
                std::map<int, Z> piv;
                for (int c = 0; c < 16; ++c) {
                    Z eq = p[r - 1][c] == '0' ? inv[c] : Z(0);
                    while (eq != 0) {
                        int j = int(mpz_scan1(eq.get_mpz_t(), 0));
                        if (piv.count(j))
                            eq ^= piv.at(j);
                        else {
                            piv[j] = eq;
                            break;
                        }
                    }
                }
                std::map<int, int> free;
                for (int j = 0; j < int(cells.size()); ++j)
                    if (!piv.count(j)) {
                        free[j] = int(m.labels.size());
                        m.labels.push_back({r, cells[j]});
                    }
                m.cuts.push_back(int(free.size()));
                std::vector<Z> ex(cells.size());
                for (int j = int(cells.size()); j-- > 0;) {
                    if (free.count(j))
                        ex[j] = pow2(free.at(j));
                    else
                        for (int k : bits(Z(piv.at(j) ^ pow2(j))))
                            ex[j] ^= ex[k];
                }
                std::vector<Z> row(16);
                for (int j = 0; j < int(cells.size()); ++j) {
                    row[cells[j]] = ex[j];
                    require(ex[j] != 0, "Active cell forced zero");
                }
                forms.push_back(row);
            }
            for (int r = 0; r < R; ++r) {
                auto y = r == R - 1 ? last : linear(forms[r + 1], true);
                for (int c = 0; c < 16; ++c) {
                    if (p[r][c] == '0') {
                        require(forms[r][c] == 0 && y[c] == 0, "Inactive elimination mismatch");
                        continue;
                    }
                    require(forms[r][c] != 0 && y[c] != 0, "Active S-box forced zero");
                    Factor f;
                    f.round = r;
                    f.cell = c;
                    f.type = kind(c);
                    for (int v : bits(forms[r][c]))
                        f.av.push_back({v, -1, 0});
                    for (int v : bits(y[c]))
                        f.bv.push_back({v, -1, 0});
                    m.factors.push_back(f);
                }
            }
            Domain all;
            for (int x = 1; x < Q; ++x)
                all.push_back(x);
            m.domains.assign(m.labels.size(), all);
        } else {
            for (int r = 1; r < R; ++r) {
                int n = 0;
                for (int c = 0; c < 16; ++c)
                    if (p[r][c] == '1') {
                        m.labels.push_back({r, c});
                        ++n;
                    }
                m.cuts.push_back(n);
            }
            std::map<std::array<int, 2>, int> index;
            for (int i = 0; i < int(m.labels.size()); ++i)
                index[m.labels[i]] = i;
            for (auto lab : m.labels) {
                int mask = 15;
                if (lab[0] > 0 && lab[0] < R) {
                    mask = 0;
                    for (int i = 0; i < 64; ++i)
                        if (pbox(i) / 4 == lab[1] && p[lab[0] - 1][i / 4] == '1')
                            mask |= 1 << (pbox(i) % 4);
                }
                Domain d;
                for (int x = 1; x < 16; ++x)
                    if (!(x & ~mask))
                        d.push_back(x);
                require(!d.empty(), "Active nibble has no incoming bits");
                m.domains.push_back(d);
            }
            for (int r = 0; r < R; ++r)
                for (int c = 0; c < 16; ++c)
                    if (p[r][c] == '1') {
                        Factor f;
                        f.round = r;
                        f.cell = c;
                        for (int k = 0; k < 4; ++k) {
                            f.av.push_back({index.at({r, c}), k, k});
                            if (r == R - 1)
                                f.bv.push_back({index.at({R, c}), k, k});
                            else {
                                auto it = index.find({r + 1, pbox(4 * c + k) / 4});
                                if (it != index.end())
                                    f.bv.push_back({it->second, pbox(4 * c + k) % 4, k});
                            }
                        }
                        require(!f.bv.empty(), "Active output forced zero");
                        m.factors.push_back(f);
                    }
            require(propagate(m.domains, m.factors, dl), "Pattern has empty DDT support");
        }
        m.active = int(m.factors.size());
        m.nv = int(m.labels.size()) - m.nend;
        std::set<std::vector<int>> pairs, triples;
        for (const auto &f : m.factors) {
            auto vars = scope(f);
            vars.erase(std::remove_if(vars.begin(), vars.end(), [&](int v) { return v < m.nend; }),
                       vars.end());
            for (std::size_t i = 0; i < vars.size(); ++i)
                for (std::size_t j = i + 1; j < vars.size(); ++j) {
                    pairs.insert({vars[i], vars[j]});
                    for (std::size_t k = j + 1; k < vars.size(); ++k)
                        triples.insert({vars[i], vars[j], vars[k]});
                }
        }
        int n = 0;
        for (const auto &b : pairs) {
            if (n++ == 256)
                break;
            m.blocks.push_back(b);
        }
        n = 0;
        for (const auto &b : triples) {
            if (n++ == 16)
                break;
            m.blocks.push_back(b);
        }
        return m;
    }
    std::string coordinates(const std::vector<int> &x) const {
        std::ostringstream s;
        s << std::uppercase << std::hex << std::setfill('0');
        for (int a : x) {
            require(a > 0 && a < Q, "Invalid free coordinate");
            s << std::setw(B / 4) << a;
        }
        return s.str();
    }
    std::vector<int> digits(const std::string &s) const {
        require(s.size() % std::size_t(B / 4) == 0 &&
                    s.find_first_not_of("0123456789ABCDEF") == std::string::npos,
                "Bad coordinates");
        std::vector<int> x;
        for (std::size_t i = 0; i < s.size(); i += B / 4)
            x.push_back(std::stoi(s.substr(i, B / 4), nullptr, 16));
        return x;
    }
    J decode(const Model &m, const std::string &e, const std::string &in) const {
        auto x = digits(e + in);
        require(x.size() == m.labels.size() && e.size() == std::size_t(m.nend * (B / 4)),
                "Coordinate length mismatch");
        for (std::size_t i = 0; i < x.size(); ++i)
            require(std::find(m.domains[i].begin(), m.domains[i].end(), x[i]) != m.domains[i].end(),
                    "Coordinate outside exact domain");
        std::vector<std::vector<int>> xx(R, std::vector<int>(16)), yy = xx;
        Z n = 1;
        for (auto &f : m.factors) {
            int a = eval(f.av, x), b = eval(f.bv, x), v = entry(f.type, a, b);
            require(v > 0, "Illegal DDT witness");
            n *= v;
            xx[f.round][f.cell] = a;
            yy[f.round][f.cell] = b;
        }
        xx.push_back(forward(yy.back()));
        for (int r = 0; r < R; ++r)
            require(forward(yy[r]) == xx[r + 1], "Witness violates linear layer");
        std::vector<std::string> s;
        for (auto &row : xx)
            s.push_back(encode(row));
        return J{{"states", s},
                 {"weight", DB * m.active - log2z(n)},
                 {"witness_numerator", n.get_str()},
                 {"pattern", activity(s)}};
    }
    J encode_witness(const Model &m, const std::vector<std::string> &states) const {
        require(activity(states) == m.pattern, "Witness pattern differs");
        std::vector<std::vector<int>> xx;
        for (auto &s : states)
            xx.push_back(parse(s));
        auto yl = inverse(xx.back());
        std::vector<int> x;
        for (auto l : m.labels) {
            int v = l[0] == R ? yl[l[1]] : xx[l[0]][l[1]];
            require(v != 0, "Zero free active coordinate");
            x.push_back(v);
        }
        std::string es = coordinates({x.begin(), x.begin() + m.nend}),
                    is = coordinates({x.begin() + m.nend, x.end()});
        J checked = decode(m, es, is);
        require(checked["states"] == states, "Free-coordinate witness mismatch");
        checked["coordinates"] = es;
        checked["witness_internal"] = is;
        return checked;
    }
    Family family(const std::vector<std::string> &states, const Deadline &dl = {}) const {
        Family f;
        f.states = states;
        f.model = compile(activity(states), dl);
        auto w = encode_witness(f.model, states);
        f.endpoint_coordinates = w["coordinates"];
        f.internal_coordinates = w["witness_internal"];
        f.weight = w["weight"];
        f.witness_num = integer(w["witness_numerator"]);
        f.exponent = DB * f.model.active;
        auto x = digits(f.endpoint_coordinates + f.internal_coordinates);
        f.domains.assign(f.model.domains.begin() + f.model.nend, f.model.domains.end());
        f.witness.assign(x.begin() + f.model.nend, x.end());
        for (auto g : f.model.factors) {
            auto remap = [&](std::vector<Term> &ts, int &constant) {
                std::vector<Term> newts;
                for (auto t : ts) {
                    if (t.v < f.model.nend)
                        constant ^= t.src < 0 ? x[t.v] : ((x[t.v] >> t.src) & 1) << t.dst;
                    else {
                        t.v -= f.model.nend;
                        newts.push_back(t);
                    }
                }
                ts = std::move(newts);
            };
            remap(g.av, g.a);
            remap(g.bv, g.b);
            f.factors.push_back(g);
        }
        require(propagate(f.domains, f.factors, dl), "Support removed verified witness");
        for (int v = 0; v < int(f.witness.size()); ++v)
            require(std::find(f.domains[v].begin(), f.domains[v].end(), f.witness[v]) != f.domains[v].end(),
                    "Propagation excluded witness coordinate");
        f.root_bound = upper_bound(f, f.domains, dl);
        require(integer(f.root_bound["upper_numerator"]) >= f.witness_num, "Bound excluded witness mass");
        return f;
    }
    J upper_bound(const Family &fam, const Domains &ds, const Deadline &dl = {}) const {
        struct C {
            double ratio;
            int index;
            std::vector<int> factors;
            std::vector<int> vars;
            Z total;
        };
        std::vector<int> maxima;
        std::vector<C> cs;
        for (std::size_t i = 0; i < fam.factors.size(); ++i) {
            dl.check();
            const auto &f = fam.factors[i];
            auto vars = scope(f);
            PairSet reach(Q * Q);
            reach.set(0);
            for (int v : vars)
                reach = extend(reach, ds[v], f, v, dl);
            reach = reach.shift(f.a | (f.b << B));
            int mx = 0;
            for (auto &v : levels[f.type])
                if (reach.intersects(v.second)) {
                    mx = v.first;
                    break;
                }
            if (!mx)
                return J{{"upper_numerator", "0"},
                         {"factor_ceiling", "0"},
                         {"domain_volume", product(ds).get_str()},
                         {"method", "local_support_zero"}};
            maxima.push_back(mx);
            if (vars.size() <= 2) {
                Z sum = 0;
                std::uint64_t vol = 1;
                for (int v : vars)
                    vol *= ds[v].size();
                for (std::uint64_t j = 0; j < vol; ++j) {
                    if ((j & 1023) == 0)
                        dl.check();
                    auto ix = j;
                    int pair = f.a | (f.b << B);
                    for (int v : vars) {
                        int x = ds[v][ix % ds[v].size()];
                        ix /= ds[v].size();
                        pair ^= contribution(f, v, x);
                    }
                    sum += entry(f.type, pair & (Q - 1), pair >> B);
                }
                cs.push_back({sum.get_d() / (double(vol) * mx), int(i), {int(i)}, vars, sum});
            }
        }
        auto assemble=[&](std::vector<C> candidates){
        std::sort(candidates.begin(), candidates.end(), [](const C &a, const C &b) {
            return std::tie(a.ratio, a.index) < std::tie(b.ratio, b.index);
        });
        std::set<int> used, selected;
        Z bound = 1, ceiling = 1;
        for (const auto &c : candidates) {
            bool overlap = false;
            for (int v : c.vars)
                if (used.count(v))
                    overlap = true;
            for(int f:c.factors)overlap=overlap||selected.count(f);
            if (!overlap) {
                used.insert(c.vars.begin(), c.vars.end());
                selected.insert(c.factors.begin(),c.factors.end());
                bound *= c.total;
            }
        }
        for (std::size_t i = 0; i < maxima.size(); ++i) {
            ceiling *= maxima[i];
            if (!selected.count(int(i)))
                bound *= maxima[i];
        }
        for (std::size_t v = 0; v < ds.size(); ++v)
            if (!used.count(int(v)))
                bound *= ds[v].size();
        return std::min(bound,pow2(fam.exponent));};
        Z bound=assemble(cs),ceiling=1;for(int x:maxima)ceiling*=x;

        std::uint64_t work=0,cap=mass_messages::setting("THERMO_LOCAL_BOUND_WORK",65536),pairs=0;
        for(std::size_t i=0;i<fam.factors.size() && work<cap;++i)for(std::size_t j=i+1;j<fam.factors.size() && work<cap;++j){
            auto vi=scope(fam.factors[i]),vj=scope(fam.factors[j]);std::set<int> both(vi.begin(),vi.end());both.insert(vj.begin(),vj.end());
            if(both.size()>2 || vi.empty() || vj.empty() || both.size()==vi.size()+vj.size())continue;
            std::vector<int> vars(both.begin(),both.end());std::uint64_t volume=1;for(int v:vars)volume*=ds[v].size();
            if(volume>cap-work)continue;
            work+=volume;Z sum=0;
            for(std::uint64_t ix=0;ix<volume;++ix){if(!(ix&1023))dl.check();auto n=ix;int a=fam.factors[i].a|(fam.factors[i].b<<B),b=fam.factors[j].a|(fam.factors[j].b<<B);
                for(int v:vars){int x=ds[v][n%ds[v].size()];n/=ds[v].size();a^=contribution(fam.factors[i],v,x);b^=contribution(fam.factors[j],v,x);}
                sum+=entry(fam.factors[i].type,a&(Q-1),a>>B)*entry(fam.factors[j].type,b&(Q-1),b>>B);}
            cs.push_back({sum.get_d()/(double(volume)*maxima[i]*maxima[j]),int(i),{int(i),int(j)},vars,sum});++pairs;
        }
        if(pairs)bound=std::min(bound,assemble(cs));
        return J{{"upper_numerator", bound.get_str()},
                 {"factor_ceiling", ceiling.get_str()},
                 {"domain_volume", product(ds).get_str()},
                 {"method", "disjoint_single_or_pair_sums_and_remaining_factor_maxima"},
                 {"pair_tightening_evaluations",work},{"pair_tightening_candidates",pairs}};
    }
    void terms(std::ostream &o, const std::vector<Term> &ts) const {
        o << ts.size();
        for (auto t : ts) {
            o << ' ' << t.v;
            if (present)
                o << ' ' << t.src << ' ' << t.dst;
        }
    }
    std::string native_model(const Model &m) const {
        std::ostringstream o;
        o << cfg.at("model_magic").get<std::string>() << ' ' << m.nend << ' ' << m.nv << " 1\n";
        if (present)
            for (auto &d : m.domains) {
                o << d.size();
                for (int x : d)
                    o << ' ' << x;
                o << '\n';
            }
        o << m.id << ' ' << m.active << ' ' << m.nv << ' ' << m.factors.size() << ' ' << m.blocks.size()
          << '\n';
        for (int i = m.nend; i < int(m.labels.size()); ++i)
            o << i << ' ';
        o << '\n';
        for (auto &f : m.factors) {
            o << f.round << ' ' << f.cell << ' ';
            terms(o, f.av);
            o << ' ';
            terms(o, f.bv);
            o << '\n';
        }
        for (auto &b : m.blocks) {
            o << b.size();
            for (int x : b)
                o << ' ' << x;
            o << '\n';
        }
        return o.str();
    }
    JointCatalog native_catalog(const std::vector<std::shared_ptr<Model>> &members) const {
        require(!present && members.size() >= 2, "Joint catalog requires SKINNY/Midori members");
        JointCatalog out;
        out.nend = members.front()->nend;
        std::map<std::array<int, 2>, int> indices;
        std::set<std::string> ids;
        for (int v = 0; v < out.nend; ++v)
            indices.emplace(members.front()->labels.at(v), v);
        for (const auto &m : members) {
            require(ids.insert(m->id).second, "Repeated pattern would double-count catalog mass");
            require(m->nend == out.nend &&
                        std::equal(m->labels.begin(), m->labels.begin() + m->nend,
                                   members.front()->labels.begin()), "Incompatible endpoint coordinates");
            for (int v = m->nend; v < int(m->labels.size()); ++v)
                if (!indices.count(m->labels[v])) {
                    int next = int(indices.size());
                    indices.emplace(m->labels[v], next);
                }
        }
        out.dimension = int(indices.size()) - out.nend;
        std::ostringstream s;
        s << cfg.at("model_magic").get<std::string>() << ' ' << out.nend << ' '
          << out.dimension << ' ' << members.size() << '\n';
        for (const auto &m : members) {
            std::vector<int> map, used;
            for (auto lab : m->labels) map.push_back(indices.at(lab));
            for (int v = m->nend; v < int(map.size()); ++v) used.push_back(map[v]);
            out.internal_map.push_back(used);
            s << m->id << ' ' << m->active << ' ' << m->nv << ' ' << m->factors.size()
              << ' ' << m->blocks.size() << '\n';
            for (int v : used) s << v << ' ';
            s << '\n';
            for (const auto &f : m->factors) {
                s << f.round << ' ' << f.cell;
                for (const auto *side : {&f.av, &f.bv}) {
                    s << ' ' << side->size();
                    for (auto t : *side) s << ' ' << map.at(t.v);
                }
                s << '\n';
            }
            for (const auto &b : m->blocks) {
                s << b.size();
                for (int v : b) s << ' ' << map.at(v);
                s << '\n';
            }
        }
        out.text = s.str();
        return out;
    }
    std::string joint_internal(const JointCatalog &catalog, std::size_t member,
                               const std::string &padded_text) const {
        const auto padded=digits(padded_text);
        require(int(padded.size())==catalog.dimension,"Joint receipt padding size");
        for(int x:padded)require(x>0 && x<Q,"Joint receipt coordinate outside nonzero domain");
        std::vector<int> local;
        for(int v:catalog.internal_map.at(member))local.push_back(padded.at(v-catalog.nend));
        return coordinates(local);
    }
    std::string native_domains(const Family &f) const {
        std::ostringstream o;
        o << cfg.at("domains_magic").get<std::string>() << " 0 " << f.domains.size() << '\n';
        for (int v = 0; v < int(f.domains.size()); ++v) {
            o << v + f.model.nend << ' ' << f.domains[v].size();
            for (int x : f.domains[v])
                o << ' ' << x;
            o << '\n';
        }
        return o.str();
    }
    std::string exact_text(const Family &f, const J &boxes, const J &plan = nullptr) const {
        std::ostringstream o;
        o << cfg.at("exact_magic").get<std::string>() << ' ' << f.domains.size() << ' ' << f.model.active
          << ' ';
        if (plan.is_null())
            o << boxes.size() << '\n';
        else {
            auto pin = plan.at("pinned").get<std::vector<int>>(),
                 order = plan.at("order").get<std::vector<int>>();
            o << pin.size() << ' ' << order.size() << '\n';
            for (int v : pin)
                o << v << ' ';
            o << '\n';
            for (int v : order)
                o << v << ' ';
            o << '\n';
        }
        for (auto &d : f.domains) {
            o << d.size();
            for (int x : d)
                o << ' ' << x;
            o << '\n';
        }
        if (plan.is_null()) {
            for (int v : f.witness)
                o << v << ' ';
            o << '\n';
        }
        for (auto &g : f.factors) {
            if (midori && B == 8)
                o << g.type << ' ';
            o << g.a << ' ' << g.b << ' ';
            terms(o, g.av);
            o << ' ';
            terms(o, g.bv);
            o << '\n';
        }
        if (plan.is_null())
            for (auto &box : boxes) {
                o << box.size();
                for (auto &b : box)
                    o << ' ' << b[0] << ' ' << b[1].get<std::string>();
                o << '\n';
            }
        return o.str();
    }
};
}
