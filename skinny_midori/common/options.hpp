#pragma once
#include "worker.hpp"
#include <iostream>
#include <random>
namespace thermo {
struct Options {
    fs::path variant, out, scratch;
    unsigned cores = 100;
    double hours = 24, ram_fraction = .75, ram_gib = 0, job_ram_gib = 0;
    bool help = false;
    std::uint64_t message_cache_mib=UINT64_MAX,collapsed_every=16,collapsed_work=262144,branch_work=65536,fine_plan_bits=128,local_bound_work=65536;
    bool cost_priority=true;
    bool transfer_patterns=true, activity_replicas=true;
    bool mixed_pattern_generators=true;
    unsigned activity_levels=12;
    unsigned transfer_lookahead=0;
    std::optional<std::uint64_t> seed;
    std::uint64_t max_slices = INT64_MAX, leaf_operations = 5000000;
    std::vector<int> counts, caps = {2, 3, 4};
    std::vector<fs::path> reuse, imports;
    double pattern_seconds = 60, search_seconds = 120, screen_seconds = 120, exact_seconds = 20,
           plan_seconds = 10, expansion_seconds = 120;
    double max_exact_seconds = 3600, corridor_seconds = 60, corridor_probe_seconds = 15, pair_seconds = 90,
           pair_max_seconds = 900;
    unsigned paths = 32, steps = 256, keep = 12, chunk_frontier = 64, focus = 2;
    unsigned replica_focus_patterns = 0;
    bool adaptive_patterns = true;
    bool midori_binary_seeds = true;
    bool common_value_seeds = true;
    std::uint64_t common_seed_every = 3;
    double common_seed_seconds = 0;
    unsigned joint_patterns = 3, adaptive_window_jobs = 32;
    unsigned input_cap = 2, max_input = 16, max_cap = 16, adapt_every = 12, pair_threads = 0,
             pair_pending = 64;
    double expansion_share = .25, pair_share = .75;
    std::uint64_t corridor_nodes = 0, corridor_edges = 0;
    double build_seconds = 0;
    J command;
    static std::uint64_t natural(const std::string &s) {
        require(!s.empty() && s.find_first_not_of("0123456789") == std::string::npos,
                "Expected unsigned integer: " + s);
        return std::stoull(s);
    }
    static std::vector<int> list(const std::string &s) {
        std::vector<int> v;
        for (auto &x : split(s, ','))
            v.push_back(int(natural(x)));
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }
    Options(int argc, char **argv) {
        command=J::array(); std::set<std::string> seen;
        for(int i=0;i<argc;++i)command.push_back(argv[i]);
        for(int i=1;i<argc;++i) {
            std::string key=argv[i];
            require(seen.insert(key).second,"Repeated option: "+key);
            if(key=="--help"||key=="-h"){help=true;continue;}
            require(i+1<argc,"Missing value for "+key);
            std::string value=argv[++i];
            if(key=="--variant")variant=fs::absolute(value);
            else if(key=="--out")out=fs::absolute(value);
            else if(key=="--cores"){auto n=natural(value);require(n>=2&&n<=4096,"--cores must be 2..4096");cores=unsigned(n);}
            else if(key=="--seed")seed=natural(value);
            else if(key=="--hours"){size_t used=0;hours=std::stod(value,&used);require(used==value.size()&&std::isfinite(hours)&&hours>0,"Invalid hours");}
            else if(key=="--build-seconds"){size_t used=0;build_seconds=std::stod(value,&used);require(used==value.size()&&std::isfinite(build_seconds)&&build_seconds>=0,"Invalid build duration");}
            else throw std::runtime_error("Unknown argument: "+key);
        }
        if(help)return;
        require(!variant.empty()&&!out.empty(),"Variant and output directory required");
        counts=read_json(variant/"config.json").at("active_counts").get<std::vector<int>>();
    }
    J scientific() const {
        return J{{"counts", counts},
                 {"replicas", {1, 2, 4}},
                 {"screen_paths", paths},
                 {"screen_steps", steps},

                 {"scope", "independent-round-key DDT product; complete rounds including final linear layer"},
                 {"known_endpoints_injected", false},
                 {"present_initial_caps", caps},
                 {"present_initial_input_cap", input_cap},
                 {"present_max_internal_cap", max_cap},
                 {"present_max_input_cap", max_input}};
    }
    static void usage() {
        std::cout << "bash run.sh OUTPUT_DIR [--cores 100] [--hours 1] [--seed N]\n";
    }
};
inline unsigned available_cpus() {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        return std::max(1, CPU_COUNT(&set));
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}
inline double number(const J &j, const char *k, double fallback = 0) {
    return j.contains(k) && j[k].is_number() ? j[k].get<double>() : fallback;
}
inline std::uint64_t count(const J &j, const char *k, std::uint64_t fallback = 0) {
    return j.contains(k) && j[k].is_number_integer() ? j[k].get<std::uint64_t>() : fallback;
}
inline void increment(J &j, const std::string &k, std::uint64_t n = 1) {
    if (j.is_null())
        j = J::object();
    require(j.is_object(), "Counter container must be an object");
    auto previous = count(j, k.c_str());
    require(n <= UINT64_MAX - previous, "Counter overflow: " + k);
    j[k] = previous + n;
}
inline J combine_screen(const J &old, const J &fresh) {
    if (fresh.value("status", std::string()) != "COMPLETE")
        return old.is_null() ? fresh : old;
    if (old.is_null() || old.value("status", std::string()) != "COMPLETE")
        return fresh;
    J r = fresh;
    double f0 = number(old, "F", INFINITY), f1 = number(fresh, "F", INFINITY), f = std::min(f0, f1);
    auto n0 = count(old, "paths"), n1 = count(fresh, "paths");
    long double a = std::isfinite(f0) ? n0 * std::exp2((long double)f - f0) : 0,
                b = std::isfinite(f1) ? n1 * std::exp2((long double)f - f1) : 0;
    if (a + b > 0) {
        r["F"] = f - std::log2((a + b) / (n0 + n1));
        r["ess"] = double(
            (a + b) * (a + b) /
            (a * a / std::max(1e-300, number(old, "ess")) + b * b / std::max(1e-300, number(fresh, "ess"))));
        r["max_weight_fraction"] = double(
            std::max(a * number(old, "max_weight_fraction"), b * number(fresh, "max_weight_fraction")) /
            (a + b));
    }
    r["paths"] = n0 + n1;
    r["positive"] = count(old, "positive") + count(fresh, "positive");
    return r;
}
}
