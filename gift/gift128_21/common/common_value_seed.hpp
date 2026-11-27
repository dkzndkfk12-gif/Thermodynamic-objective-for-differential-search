#pragma once
#include "model.hpp"
#include <random>
namespace thermo {

inline bool is_common_seed(const J &job) {
    return job.value("kind", std::string()) == "seed" &&
           job.value("seed_kind", std::string()) == "common_value";
}
inline std::uint64_t common_mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
inline std::pair<std::size_t,int> common_coordinate(std::uint64_t cursor, std::size_t supports,
                                                   int q, std::uint64_t seed) {
    require(supports > 0 && q >= 2, "Invalid common-value row domain");
    const auto index = std::size_t(cursor % supports), visit = cursor / supports;

    return {index, 1 + int((common_mix(seed) % (q-1) + index % (q-1) + visit % (q-1)) % (q-1))};
}
inline std::vector<int> common_masks(const Cipher &c, unsigned mask) {
    require(!c.present && !c.midori && mask > 0 && mask < 65536, "SKINNY common-value support required");
    std::vector<int> masks;
    for (int r = 0; r <= c.R; ++r) {
        masks.push_back(int(mask));
        if (r == c.R) break;
        std::vector<int> x(16);
        for (int i=0;i<16;++i) x[i] = (mask >> i) & 1;
        auto y = c.forward(x);
        mask = 0;
        for (int i=0;i<16;++i) {
            require(y[i] == 0 || y[i] == 1, "Linear layer does not preserve common-value supports");
            mask |= unsigned(y[i]) << i;
        }
        require(mask != 0, "Nonzero support mapped to zero");
    }
    return masks;
}
struct CommonCatalog {
    std::map<int,std::vector<unsigned>> by_active;
    CommonCatalog(const Cipher &c, std::uint64_t seed, const Deadline &dl) {
        for (unsigned m=1;m<65536;++m) {
            if (!(m & 255)) dl.check();
            auto masks = common_masks(c,m);
            int active=0;
            for (int r=0;r<c.R;++r) active += __builtin_popcount(unsigned(masks[r]));
            by_active[active].push_back(m);
        }
        for (auto &kv : by_active) {
            dl.check();
            std::sort(kv.second.begin(),kv.second.end(),[&](unsigned a,unsigned b) {
                return std::make_pair(common_mix(seed ^ a),a) < std::make_pair(common_mix(seed ^ b),b);
            });
        }
    }
};

struct CommonKernel {
    int q, db;
    std::vector<std::vector<std::pair<int,int>>> edges;
    std::vector<std::vector<int>> entries;
    std::vector<std::vector<Z>> powers;
    CommonKernel(std::vector<std::vector<int>> e, int denominator_bits, int max_active)
        : q(int(e.size())), db(denominator_bits), edges(q), entries(std::move(e)) {
        require(q >= 2 && db > 0 && max_active > 0, "Invalid common kernel");
        int maximum=0;
        for (int u=0;u<q;++u) {
            require(int(entries[u].size())==q,"Common kernel shape");
            for (int v=0;v<q;++v) {
                int n=entries[u][v];
                require(n>=0 && ((u && v) || n==0),"Invalid active DDT support");
                if (n) edges[u].push_back({v,n});
                maximum=std::max(maximum,n);
            }
        }
        powers.resize(max_active+1,std::vector<Z>(maximum+1));
        for (int a=1;a<=max_active;++a)
            for (int n=1;n<=maximum;++n) powers[a][n]=power(Z(n),a);
    }
    static CommonKernel skinny(const Cipher &c) {
        require(!c.present && !c.midori && c.Q==c.DIV*(1<<c.DB),"SKINNY DDT normalization mismatch");
        std::vector<std::vector<int>> e(c.Q,std::vector<int>(c.Q));
        for(int u=1;u<c.Q;++u) for(int v=1;v<c.Q;++v) {
            require(c.count(0,u,v)%c.DIV==0,"Nonintegral normalized DDT entry");
            e[u][v]=c.entry(0,u,v);
        }
        return CommonKernel(std::move(e),c.DB,16);
    }
};
inline Z common_random_below(const Z &n, std::mt19937_64 &rng, const Deadline &dl) {
    require(n>0,"Cannot sample an empty integer measure");
    if(n==1) return 0;
    Z top=n-1;
    const auto bits=mpz_sizeinbase(top.get_mpz_t(),2), words=(bits+63)/64;
    std::vector<std::uint64_t> data(words);
    for (;;) {
        dl.check();
        for(auto &x:data) x=rng();
        if(bits%64) data.back() &= (std::uint64_t(1)<<(bits%64))-1;
        Z x;
        mpz_import(x.get_mpz_t(),data.size(),-1,sizeof(std::uint64_t),0,0,data.data());
        if(x<n) return x;
    }
}
struct CommonRow {
    const CommonKernel &k;
    std::vector<int> active;
    int input;
    std::string identity;
    std::vector<std::vector<Z>> prefix;
    std::size_t next_output=0;
    CommonRow(const CommonKernel &kernel, std::vector<int> profile, int u,
              std::string id, const J &saved) : k(kernel),active(std::move(profile)),input(u),identity(std::move(id)) {
        require(input>0 && input<k.q && !active.empty(),"Invalid common-value transfer row");
        for(int a:active) require(a>0 && a<int(k.powers.size()),"Invalid common-value active count");
        prefix.emplace_back(k.q,0); prefix[0][input]=1;
        if(saved.is_null()) return;
        require(saved.at("schema")=="SKINNY_COMMON_ROW_V1" && saved.at("identity")==identity,
                "Common-value checkpoint identity changed");
        const auto &p=saved.at("prefix");
        require(p.is_array() && !p.empty() && p.size()<=active.size()+1,"Common-value checkpoint depth");
        std::vector<std::vector<Z>> loaded;
        for(const auto &row:p) {
            require(row.is_array() && row.size()==std::size_t(k.q),"Common-value checkpoint row shape");
            std::vector<Z> v;
            for(const auto &x:row) {v.push_back(integer(x));require(v.back()>=0,"Negative common mass");}
            require(v[0]==0,"Zero value outside active common family");
            loaded.push_back(std::move(v));
        }
        require(loaded[0]==prefix[0],"Common-value initial row mismatch");
        prefix=std::move(loaded);
        next_output=saved.at("next_output").get<std::size_t>();
        require(prefix.size()==active.size()+1 || next_output==0,"Output cursor before completed transfer");
    }
    bool complete() const {return prefix.size()==active.size()+1;}
    void step(const Deadline &dl) {
        require(!complete(),"Transfer row already completed");
        const int a=active[prefix.size()-1];
        std::vector<Z> next(k.q);
        for(int u=1;u<k.q;++u) {
            dl.check();
            if(prefix.back()[u]==0) continue;
            for(const auto &edge:k.edges[u]) next[edge.first] += prefix.back()[u]*k.powers[a][edge.second];
        }
        dl.check();
        prefix.push_back(std::move(next));
    }
    J checkpoint() const {
        J p=J::array();
        for(const auto &row:prefix) {
            J v=J::array();for(const auto &x:row)v.push_back(x.get_str());p.push_back(std::move(v));
        }
        return J{{"schema","SKINNY_COMMON_ROW_V1"},{"identity",identity},
                 {"prefix",p},{"next_output",next_output}};
    }
    std::vector<int> outputs(unsigned keep) const {
        require(complete() && keep>0,"Completed transfer required for endpoint ranking");
        std::vector<int> out;
        for(int v=1;v<k.q;++v) if(prefix.back()[v]>0) out.push_back(v);
        std::sort(out.begin(),out.end(),[&](int a,int b){
            return prefix.back()[a]!=prefix.back()[b] ? prefix.back()[a]>prefix.back()[b] : a<b;
        });
        if(out.size()>keep)out.resize(keep);
        return out;
    }
    int predecessor(int r,int output,Z ticket) const {
        require(r>=0 && r<int(active.size()) && std::size_t(r+1)<prefix.size() &&
            output>0 && output<k.q && ticket>=0 && ticket<prefix[r+1][output],"Invalid backward integer ticket");
        for(int u=1;u<k.q;++u) {
            const int n=k.entries[u][output];
            Z w=prefix[r][u]*k.powers[active[r]][n];
            if(ticket<w)return u;
            ticket-=w;
        }
        throw std::runtime_error("Backward common-value normalization mismatch");
    }
    std::vector<int> sample(int output, std::mt19937_64 &rng,const Deadline &dl) const {
        require(complete() && output>0 && output<k.q && prefix.back()[output]>0,"Empty common endpoint");
        std::vector<int> path(active.size()+1);path.back()=output;
        for(int r=int(active.size())-1;r>=0;--r) {
            Z ticket=common_random_below(prefix[r+1][path[r+1]],rng,dl);
            path[r]=predecessor(r,path[r+1],ticket);
        }
        require(path.front()==input,"Backward common-value endpoint mismatch");
        return path;
    }
};

inline double common_seed_F(const J &candidate) {
    if(!candidate.contains("common_value_seed"))return INFINITY;
    const auto &f=candidate.at("common_value_seed").at("mass").at("F");
    return f.is_number()?f.get<double>():INFINITY;
}
inline void merge_common_seed(J &candidate,const J &certificate) {
    require(certificate.at("schema")=="SKINNY_COMMON_FAMILY_V1" &&
        integer(certificate.at("mass").at("numerator"))>0,"Invalid common-family certificate");
    if(!candidate.contains("common_value_seed") ||
       compare_mass(certificate.at("mass"),candidate.at("common_value_seed").at("mass"))>0)
        candidate["common_value_seed"]=certificate;
}
}
