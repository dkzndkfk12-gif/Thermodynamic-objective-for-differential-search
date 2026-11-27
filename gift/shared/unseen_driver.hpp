#pragma once
#include "unseen_mass.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <csignal>
namespace unseen_mass {
using J=nlohmann::json;
namespace fs=std::filesystem;
inline volatile std::sig_atomic_t stop_requested=0;
inline void signal_handler(int){stop_requested=1;}
struct QuantumStop{};

inline std::string resume_sha256(const std::string &input) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<unsigned char> d(input.begin(), input.end());
    std::uint64_t bits = std::uint64_t(d.size()) * 8;
    d.push_back(0x80);
    while (d.size() % 64 != 56)
        d.push_back(0);
    for (int j = 7; j >= 0; --j)
        d.push_back(static_cast<unsigned char>(bits >> (8 * j)));
    auto rr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t off = 0; off < d.size(); off += 64) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 4; ++j)
                w[i] = (w[i] << 8) | d[off + 4 * i + j];
        for (int i = 16; i < 64; ++i) {
            auto a = w[i - 15], b = w[i - 2];
            w[i] = w[i - 16] + (rr(a, 7) ^ rr(a, 18) ^ (a >> 3)) + w[i - 7] +
                   (rr(b, 17) ^ rr(b, 19) ^ (b >> 10));
        }
        auto a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            auto t = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            auto u = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = dd + t;
            dd = c;
            c = b;
            b = a;
            a = t + u;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += dd;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    std::ostringstream s;
    for (auto x : h)
        s << std::hex << std::setw(8) << std::setfill('0') << x;
    return s.str();
}
inline std::string resume_digest(const J& j){J copy=j;copy.erase("checksum");return resume_sha256(copy.dump(-1, ' ', true));}
inline void seal_resume(J& j){j["checksum"]=resume_digest(j);}

inline Q fraction(const J& j,const char* n,const char* d){
    segment_mass::Z num(j.at(n).get<std::string>()),den(j.at(d).get<std::string>());
    if(num<0||den<=0)throw std::runtime_error("unseen checkpoint invalid fraction");
    Q q(num,den);q.canonicalize();return q;
}
inline std::string identity(const segment_mass::Spec&s,const Region&r){
    return "UNSEEN_FULL_DDT_SUM_MAX_V1/"+s.identity()+"/R="+std::to_string(s.R)+"/"+r.id+"/"+
        segment_mass::hex(r.in_mask,s.N)+"/"+segment_mass::hex(r.in_value,s.N)+"/"+
        segment_mass::hex(r.out_mask,s.N)+"/"+segment_mass::hex(r.out_value,s.N);
}
inline void restore_solver(Solver& s,const J& r,const std::string& id){
    if(!r.contains("checksum")||r.at("checksum").get<std::string>()!=resume_digest(r))throw std::runtime_error("unseen resume checksum mismatch");
    if(r.at("schema").get<int>()!=1||r.at("identity").get<std::string>()!=id)throw std::runtime_error("unseen resume context mismatch");
    std::string ds=r.at("domains").get<std::string>();if(ds.size()!=s.d.size())throw std::runtime_error("unseen resume domain length");
    for(unsigned i=0;i<ds.size();++i){if(ds[i]<'1'||ds[i]>'3')throw std::runtime_error("unseen resume domain value");s.d[i]=static_cast<unsigned char>(ds[i]-'0');}
    for(const auto& a:r.at("undo")){unsigned v=a.at(0).get<unsigned>(),old=a.at(1).get<unsigned>();if(v>=s.d.size()||old<1||old>3)throw std::runtime_error("unseen resume undo entry");s.undo.push_back({v,static_cast<unsigned char>(old)});}
    std::size_t previous=0;for(const auto& a:r.at("stack")){unsigned v=a.at(0).get<unsigned>(),remaining=a.at(1).get<unsigned>();auto mark=a.at(2).get<std::size_t>();
        if(v>=s.d.size()||(remaining!=0&&remaining!=2)||mark>s.undo.size()||mark<previous)throw std::runtime_error("unseen resume frontier entry");
        s.stack.push_back({v,static_cast<unsigned char>(remaining),mark});previous=mark;}

    auto base=s.d;for(auto i=s.undo.rbegin();i!=s.undo.rend();++i)base[i->var]=i->old;
    if(base!=domains(s.spec,s.region))throw std::runtime_error("unseen resume undo root mismatch");
    s.nodes=r.at("nodes").get<std::uint64_t>();s.total_trails=r.at("total_trails").get<std::uint64_t>();s.exhausted=r.at("exhausted").get<bool>();
    if(s.exhausted&&!s.stack.empty())throw std::runtime_error("unseen exhausted frontier not empty");
    s.fixed_mass=fraction(r,"fixed_mass_numerator","fixed_mass_denominator");
    if(s.fixed_mass>1||(!fixed_endpoints(s.spec,s.region)&&s.fixed_mass!=0))throw std::runtime_error("unseen resume mass scope");
    std::istringstream random(r.at("rng").get<std::string>());random>>s.rng;if(!random)throw std::runtime_error("unseen resume RNG");
}
inline J save_solver(const Solver&s,const std::string&id){
    J j={{"schema",1},{"identity",id},{"undo",J::array()},{"stack",J::array()},
         {"nodes",s.nodes},{"total_trails",s.total_trails},{"exhausted",s.exhausted},
         {"fixed_mass_numerator",s.fixed_mass.get_num().get_str()},{"fixed_mass_denominator",s.fixed_mass.get_den().get_str()}};
    std::string ds;ds.reserve(s.d.size());for(auto d:s.d)ds+=char('0'+d);j["domains"]=ds;
    for(const auto&c:s.undo)j["undo"].push_back(J::array({c.var,unsigned(c.old)}));
    for(const auto&f:s.stack)j["stack"].push_back(J::array({f.var,unsigned(f.remaining),f.mark}));
    std::ostringstream random;random<<s.rng;j["rng"]=random.str();seal_resume(j);return j;
}
inline void atomic_json(const std::string&path,const J&j){std::ofstream f(path+".tmp");f<<j.dump()<<'\n';f.flush();if(!f)throw std::runtime_error("unseen checkpoint write");f.close();fs::rename(path+".tmp",path);}
inline int main(int argc,char** argv){try{
    std::map<std::string,std::string>options;for(int i=1;i<argc;++i){std::string k=argv[i];if(i+1>=argc)throw std::runtime_error("unseen missing CLI value");options[k]=argv[++i];}
    std::ifstream input(options.at("--job"));J job;input>>job;const auto&c=job.at("config");
    unsigned n=c.value("cells",16u),rounds=c.at("rounds").get<unsigned>();std::vector<unsigned>perm;
    if(c.contains("PERM"))perm=c.at("PERM").get<std::vector<unsigned>>();else{perm.resize(4*n);for(unsigned i=0;i<4*n;++i)perm[i]=i==4*n-1?i:n*i%(4*n-1);}
    segment_mass::Spec spec(n,rounds,c.at("SBOX").get<std::vector<unsigned>>(),perm);
    const auto&jr=job.at("region");Region region{jr.at("id").get<std::string>(),segment_mass::word(jr.at("input_mask"),n),segment_mass::word(jr.at("input_value"),n),segment_mass::word(jr.at("output_mask"),n),segment_mass::word(jr.at("output_value"),n)};
    validate_region(spec,region);std::string id=identity(spec,region),output=options.at("--out");
    double seconds=std::stod(options.at("--seconds"));if(!std::isfinite(seconds)||seconds<=0)throw std::runtime_error("unseen invalid quantum");
    auto memory=job.value("memory",std::uint64_t(256)*1024*1024);unsigned keep=job.value("keep",16u),requested_width=job.value("bound_width",12u);
    if(memory<1024*1024||!keep||requested_width<7||requested_width>24)throw std::runtime_error("unseen invalid memory/keep/bound_width");
    std::signal(SIGTERM,signal_handler);std::signal(SIGINT,signal_handler);double started=segment_mass::clock(),deadline=started+seconds*.90;
    auto check=[&]{if(stop_requested||segment_mass::clock()>=deadline)throw QuantumStop();};
    Solver solver(spec,region,job.value("seed",std::uint64_t(1)));J old=job.value("resume",J::object());
    if(!old.empty())restore_solver(solver,old,id);
    auto nodes_before=solver.nodes,total_before=solver.total_trails;
    Q upper=1;bool bound_complete=false,relaxation_exact=false,bound_pending=true;unsigned next_width=requested_width,completed_width=0;
    std::string reason="",bound_reason="not_started",upper_kind="stochastic_row_sum_cap";std::uint64_t split_buckets=0,peak=0;
    if(!old.empty()){
        upper=fraction(old,"upper_numerator","upper_denominator");if(upper>1)throw std::runtime_error("unseen resume upper exceeds one");
        bound_complete=old.value("bound_complete",false);relaxation_exact=old.value("relaxation_exact",false);
        next_width=old.value("next_bound_width",requested_width);upper_kind=old.value("upper_kind",upper_kind);bound_reason=old.value("bound_reason",std::string("cached"));
        completed_width=old.value("completed_bound_width",bound_complete?requested_width:0u);bound_pending=old.value("bound_pending",!bound_complete&&next_width!=0);
        if((next_width&&(next_width<7||next_width>24))||(completed_width&&(completed_width<7||completed_width>24)))throw std::runtime_error("unseen resume bound width");
        unsigned previous_requested=old.value("requested_bound_width",requested_width);

        if(!relaxation_exact&&(requested_width>previous_requested||(!bound_complete&&requested_width!=previous_requested))){next_width=requested_width;bound_pending=true;}
    }
    double bound_start=segment_mass::clock();bool bound_attempted=false,bound_attempt_complete=false;unsigned attempt_width=0;
    if(bound_pending&&next_width&&!stop_requested){
        bound_attempted=true;attempt_width=next_width;
        double bound_deadline=std::min(deadline,started+seconds*.25);
        auto bound_check=[&]{if(stop_requested||segment_mass::clock()>=bound_deadline)throw QuantumStop();};

        std::uint64_t overhead=std::uint64_t(solver.d.size())*128+std::uint64_t(n)*rounds*512+1024*1024;
        try{if(memory<=overhead)throw BoundBudget("lease reserved for CSP frontier");auto b=bound(spec,region,next_width,memory-overhead,bound_check);
            upper=std::min(upper,b.upper);bound_complete=bound_complete||b.complete;relaxation_exact=relaxation_exact||b.relaxation_exact;split_buckets=b.split_buckets;peak=b.peak_bytes+overhead;
            completed_width=std::max(completed_width,next_width);bound_pending=false;next_width=0;bound_attempt_complete=true;
            bound_reason="complete_sum_before_max_relaxation";upper_kind="full_ddt_mini_bucket";
        }catch(const QuantumStop&){bound_reason="bound_quantum_exhausted; certified_previous_or_one_retained";next_width=next_width>7?next_width-1:0;}
        catch(const BoundBudget&e){bound_reason=std::string(e.what())+"; certified_previous_or_one_retained";next_width=next_width>7?next_width-1:0;}
        catch(const std::bad_alloc&){bound_reason="bound_allocation_failed; certified_previous_or_one_retained";next_width=next_width>7?next_width-1:0;}
        if(next_width<=completed_width)next_width=0;
        if(!next_width)bound_pending=false;
    }
    double bound_seconds=segment_mass::clock()-bound_start;
    if(upper==0){if(solver.total_trails)throw std::runtime_error("unseen zero upper contradicts prior witness");solver.exhausted=true;solver.stack.clear();}
    std::ofstream tsv(output+".tmp");if(!tsv)throw std::runtime_error("unseen TSV create");tsv<<"states\ttrail_weight\tfirst_seconds\n"<<std::setprecision(17);
    unsigned found=0;double csp_start=segment_mass::clock();
    try{while(found<keep&&!solver.exhausted){std::vector<W>path;if(!solver.leaf(path,check))break;
            segment_mass::Z numerator=1;for(unsigned r=0;r<rounds;++r)numerator*=spec.coefficient(path[r],path[r+1]);
            double weight=double(4*n*rounds)-segment_mass::log2z(numerator);
            for(unsigned r=0;r<path.size();++r){if(r)tsv<<',';tsv<<segment_mass::hex(path[r],n);}tsv<<'\t'<<weight<<'\t'<<segment_mass::clock()-started<<'\n';tsv.flush();if(!tsv)throw std::runtime_error("unseen witness write");++found;
        }
        reason=solver.exhausted?"DFS_frontier_exhausted":"witness_batch_complete; frontier_preserved";
    }catch(const QuantumStop&){reason=stop_requested?"signal; frontier_preserved":"quantum; frontier_preserved";}
    double csp_seconds=segment_mass::clock()-csp_start;
    if(solver.exhausted&&solver.total_trails==0){upper=0;bound_complete=true;relaxation_exact=true;bound_pending=false;next_width=0;upper_kind="full_region_unsatisfiable";}
    if(solver.exhausted&&fixed_endpoints(spec,region)){
        if(solver.fixed_mass>upper)throw std::runtime_error("unseen exact leaf mass exceeds certified upper");
        upper=solver.fixed_mass;bound_complete=true;relaxation_exact=true;bound_pending=false;next_width=0;upper_kind="exhaustive_fixed_endpoint_sum";
    }
    if(fixed_endpoints(spec,region)&&solver.fixed_mass>upper)throw std::runtime_error("unseen partial leaf mass exceeds certified upper");
    tsv.close();fs::rename(output+".tmp",output);
    J resume=save_solver(solver,id);resume["upper_numerator"]=upper.get_num().get_str();resume["upper_denominator"]=upper.get_den().get_str();
    resume["bound_complete"]=bound_complete;resume["relaxation_exact"]=relaxation_exact;resume["upper_kind"]=upper_kind;resume["bound_reason"]=bound_reason;
    resume["next_bound_width"]=next_width;resume["requested_bound_width"]=requested_width;
    resume["completed_bound_width"]=completed_width;resume["bound_pending"]=bound_pending;seal_resume(resume);
    J side={{"schema",1},{"region_id",region.id},{"identity",id},{"upper_numerator",upper.get_num().get_str()},{"upper_denominator",upper.get_den().get_str()},
        {"bound_complete",bound_complete},{"bound_pending",bound_pending},{"bound_attempted",bound_attempted},{"bound_attempt_complete",bound_attempt_complete},{"bound_attempt_width",attempt_width},
        {"completed_bound_width",completed_width},{"relaxation_exact",relaxation_exact},{"upper_kind",upper_kind},{"bound_reason",bound_reason},
        {"bound_seconds",bound_seconds},{"csp_seconds",csp_seconds},{"nodes",solver.nodes-nodes_before},{"nodes_total",solver.nodes},
        {"solver_exhausted",solver.exhausted},{"region_infeasible",solver.exhausted&&solver.total_trails==0},{"trails",found},{"total_trails_seen",solver.total_trails},
        {"new_leaves",solver.total_trails-total_before},{"resume",resume},{"reason",reason},{"split_buckets",split_buckets},{"peak_accounted_bytes",peak},
        {"scope","all nonzero-input legal DDT trails in endpoint region; no activity cap; upper is max endpoints of summed internal mass"},
        {"nonzero_input_relaxed_in_bound",region.in_value==0&&region.in_mask!=block_mask(4*n)},
        {"witness_selection","complete resumable legal CSP; heuristic branching, not samples from Z^m"}};
    if(job.contains("controller_context"))side["controller_context"]=job["controller_context"];
    atomic_json(output+".unseen.json",side);return 0;
}catch(const std::exception&e){std::cerr<<"unseen: "<<e.what()<<'\n';return 2;}}
}
