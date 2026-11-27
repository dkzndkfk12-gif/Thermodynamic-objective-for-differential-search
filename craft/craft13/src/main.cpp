#include "activity.hpp"
#include "archive.hpp"
#include "checkpoint.hpp"
#include "sqlite_ledger.hpp"
#include "replica.hpp"
#include "joint_replica.hpp"
#include <deque>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/file.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

using namespace craft;
namespace fs=std::filesystem;
static volatile std::sig_atomic_t interrupted=0;
static void on_signal(int) { interrupted=1; }
static uint64_t thread_ns() { timespec t{};require(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0,"thread CPU clock failed");return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec); }
static void atomic_json(const fs::path& path,const J& j,bool durable=false) {
    auto tmp=path;tmp+=".tmp";
    { std::ofstream f(tmp);f.exceptions(std::ios::failbit|std::ios::badbit);f<<std::setw(2)<<j<<'\n';f.close(); }
    if(durable)durable_file(tmp);
    fs::rename(tmp,path);
    if(durable)durable_file(path.parent_path());
}
struct Options { int rounds=0,cores=1;double duration=3600,report=5,quantum=.05,session=2;uint64_t seed=1;std::string out;bool resume=false;double ais_share=-1,exact_probe=-1,joint_relative=0;unsigned ais_bridges=256;uint64_t ais_paths=32;std::array<double,5> search_weights{};uint64_t exact_memory_bytes=0; };
static Options parse(int argc,char** argv) {
    Options o; std::set<std::string> seen;
    for(int i=1;i<argc;++i) {
        std::string key=argv[i];require(seen.insert(key).second,"Repeated option: "+key);
        if(key=="--help"||key=="-h") {
            std::cout<<"craft_mass --rounds 13 --out DIR [--cores N --hours H --seed N --resume]\n";
            std::exit(0);
        }
        if(key=="--resume"){o.resume=true;continue;}
        require(i+1<argc,"Missing value for "+key);std::string value=argv[++i];
        if(key=="--out"){o.out=value;continue;}
        if(key=="--hours"){size_t used=0;double hours=std::stod(value,&used);
            require(used==value.size()&&std::isfinite(hours)&&hours>0,"Invalid hours");o.duration=hours*3600;continue;}
        require(key=="--rounds"||key=="--cores"||key=="--seed","Unknown option: "+key);
        require(!value.empty()&&value.find_first_not_of("0123456789")==std::string::npos,"Expected unsigned integer: "+key);
        auto number=std::stoull(value);
        if(key=="--seed")o.seed=number;
        else{require(number<=unsigned(INT_MAX),"Integer option overflow");(key=="--rounds"?o.rounds:o.cores)=int(number);}
    }
    require((o.rounds==13)&&o.cores>=1&&!o.out.empty()&&std::isfinite(o.duration)
        &&o.duration<std::chrono::duration<double>(Clock::duration::max()).count()/2,"Invalid or missing options");
    o.search_weights=std::array<double,5>{2,5,1,1,1};
    o.ais_share=0.125;o.exact_probe=0;
    cpu_set_t cpus;CPU_ZERO(&cpus);
    if(sched_getaffinity(0,sizeof(cpus),&cpus)==0)require(o.cores<=CPU_COUNT(&cpus),"Requested cores exceed CPU affinity");
    return o;
}
struct Metrics {
    std::atomic<uint64_t> expansion_steps{0},joint_steps{0},joint_pattern_changes{0},joint_mixed_visits{0};
    std::atomic<uint64_t> search_ns{0},count_ns{0},search_steps{0},csp_nodes{0},unsat_patterns{0},endpoint_moves{0},global_steps{0},fixed_steps{0};
    std::atomic<uint64_t> cold_endpoint_moves{0},cold_swap_attempts{0},cold_swap_accepts{0};
    std::atomic<uint64_t> collective_updates{0},common_seeds{0},csp_yields{0};
    std::atomic<int> pending_csp{0};
    std::atomic<int> search_workers{0};
};
struct JoinWorkers {
    std::atomic<bool>& stop;
    std::vector<std::thread>& threads;
    ~JoinWorkers() { stop=true;for(auto& t:threads)if(t.joinable())t.join(); }
};
class LocalSearch {
    enum Action{Discover,Focus,Physical,Expand,Joint};
    int rounds;
    std::shared_ptr<const Model> physical;
    const std::vector<std::unique_ptr<ActivityGuide>>& guides;
    const CommonGuide& common;
    Archive& archive;
    Metrics& metrics;
    std::shared_ptr<const Model> pending_model;
    std::unique_ptr<WitnessFinder> finder;
    std::unique_ptr<ReplicaSearch> sampler;
    std::unique_ptr<JointReplica> joint;
    struct Pending{std::shared_ptr<const Model> model;std::unique_ptr<WitnessFinder> finder;};
    std::deque<Pending> paused;
    std::array<double,5> allocation,spent{};
    Action action=Discover,charged=Discover;
    unsigned generation=0;
    double used_cpu=0,session_budget;
    uint64_t old_csp_nodes=0;
    std::vector<std::string> last_collected;
    std::string source;
    State expansion_input{},expansion_output{};
    void start(RNG& rng,Clock::time_point started){
        ++generation;used_cpu=0;last_collected.clear();
        std::vector<size_t> enabled;for(size_t k=0;k<5;++k)if(allocation[k]>0)enabled.push_back(k);
        size_t next=enabled[size_t(draw(rng,int(enabled.size())))];
        for(size_t k:enabled)if(spent[k]/allocation[k]<spent[next]/allocation[next])next=k;
        charged=action=Action(next);
        int copies=draw(rng,2)?2:4;
        if(action==Joint){auto witnesses=archive.joint_seeds(rng);
            if(!witnesses.empty()){joint=std::make_unique<JointReplica>(physical,witnesses,copies,rng);source="joint-pattern-replica";return;}
            action=Focus;
        }
        Trail seed;
        if(action==Focus)seed=archive.pattern_seed(rng);
        else if(action==Expand)seed=archive.expansion_seed(rng);
        else if(action==Physical)seed=archive.feedback(rng,draw(rng,2)==0);
        if(seed.empty())action=Discover;
        if(action==Discover){
            source="activity-DP+CSP";
            if(!paused.empty()&&generation%2==0){pending_model=std::move(paused.front().model);finder=std::move(paused.front().finder);paused.pop_front();--metrics.pending_csp;old_csp_nodes=finder->nodes;return;}
            if(generation%2){auto t=common.sample(rng);archive.submit_common(t,seconds(started));++metrics.common_seeds;
                pending_model=std::make_shared<Model>(rounds,activity(t));sampler=std::make_unique<ReplicaSearch>(pending_model,pending_model->encode(t),copies,rng);source="common-value-transfer";return;}
            auto p=guides[size_t(draw(rng,int(guides.size())))]->sample(rng);
            pending_model=std::make_shared<Model>(rounds,std::move(p));finder=std::make_unique<WitnessFinder>(*pending_model);old_csp_nodes=0;return;
        }
        if(action==Focus){pending_model=std::make_shared<Model>(rounds,activity(seed));source="fixed-pattern-replica";}
        else {pending_model=physical;source=action==Expand?"same-endpoint-expansion":"all-pattern-replica";}
        if(action==Expand){expansion_input=seed.front();expansion_output=seed.back();}
        sampler=std::make_unique<ReplicaSearch>(pending_model,pending_model->encode(seed),copies,rng,action==Expand);
    }
    void collect(Clock::time_point started){
        const auto& copies=joint?joint->cold():sampler->cold();const auto& model=joint?physical:pending_model;
        if(last_collected.size()!=copies.size())last_collected.resize(copies.size());
        for(size_t j=0;j<copies.size();++j){const auto& x=copies[j];require(model->legal(x),"discovery cold trail is illegal");
            auto t=model->decode(x);if(action==Expand)require(t.front()==expansion_input&&t.back()==expansion_output,"expansion changed endpoints");
            auto key=hex(t.front())+hex(t.back())+pattern_key(activity(t))+std::to_string(trail_weight(t));
            if(last_collected[j]!=key){archive.submit(t,source,seconds(started));last_collected[j]=std::move(key);}
        }
    }
public:
    LocalSearch(int r,std::shared_ptr<const Model> full,const std::vector<std::unique_ptr<ActivityGuide>>& gs,const CommonGuide& cg, Archive& a,Metrics& ms,double session,std::array<double,5> weights)
        :rounds(r),physical(std::move(full)),guides(gs),common(cg), archive(a),metrics(ms),allocation(weights),session_budget(session){}
    void work(RNG& rng,Clock::time_point end,const std::atomic<bool>& stop,Clock::time_point started){
        uint64_t cpu_start=thread_ns();
        if(!finder&&!sampler&&!joint)start(rng,started);
        std::string work_source=source;
        while(Clock::now()<end&&!stop.load(std::memory_order_relaxed)&&!interrupted){
            if(finder){finder->step(rng);metrics.csp_nodes.fetch_add(finder->nodes-old_csp_nodes,std::memory_order_relaxed);old_csp_nodes=finder->nodes;
                if(finder->finished()){
                    if(finder->answer.empty()){++metrics.unsat_patterns;finder.reset();pending_model.reset();break;}
                    archive.submit(pending_model->decode(finder->answer),source,seconds(started));
                    sampler=std::make_unique<ReplicaSearch>(pending_model,finder->answer,draw(rng,2)?2:4,rng);finder.reset();
                }
            }else if(joint){
                auto switches=joint->pattern_changes,mixed=joint->mixed_cold_visits;
                joint->step(rng);++metrics.joint_steps;++metrics.search_steps;
                metrics.joint_pattern_changes+=joint->pattern_changes-switches;metrics.joint_mixed_visits+=joint->mixed_cold_visits-mixed;
                collect(started);
            }else{
                auto moves=sampler->endpoint_changes,cold=sampler->cold_endpoint_changes,attempts=sampler->cold_swap_attempts,accepts=sampler->cold_swap_accepts;
                auto collective=sampler->collective_updates;
                sampler->step(rng);++metrics.search_steps;metrics.endpoint_moves+=sampler->endpoint_changes-moves;
                metrics.cold_endpoint_moves+=sampler->cold_endpoint_changes-cold;metrics.cold_swap_attempts+=sampler->cold_swap_attempts-attempts;metrics.cold_swap_accepts+=sampler->cold_swap_accepts-accepts;
                metrics.collective_updates+=sampler->collective_updates-collective;
                if(action==Expand)++metrics.expansion_steps;
                else if(sampler->model->physical)++metrics.global_steps;else ++metrics.fixed_steps;
                if(action==Expand||cold!=sampler->cold_endpoint_changes)collect(started);
            }
        }
        if(sampler||joint)collect(started);
        double used=double(thread_ns()-cpu_start)/1e9;used_cpu+=used;spent[charged]+=used;archive.search_spent(work_source,used);
        if(used_cpu>=session_budget){
            sampler.reset();joint.reset();
            if(finder){paused.push_back({std::move(pending_model),std::move(finder)});++metrics.pending_csp;++metrics.csp_yields;}
            pending_model.reset();
        }
    }
};
int main(int argc,char** argv) {
    try {
        auto command_started=Clock::now();
        auto o=parse(argc,argv);
        std::signal(SIGINT,on_signal);std::signal(SIGTERM,on_signal);
        fs::create_directories(o.out);
        int lockfd=open((fs::path(o.out)/".run.lock").c_str(),O_CREAT|O_RDWR,0600);
        require(lockfd>=0&&flock(lockfd,LOCK_EX|LOCK_NB)==0,"output directory is already in use");
        contraction_resources().configure(o.cores,o.exact_memory_bytes);
        Archive archive(o.rounds);
        archive.configure_guidance(o.ais_share,o.exact_probe,o.joint_relative,o.ais_paths);
        uint64_t resume_generation=0;
        J recovery_origin=nullptr,previous_guidance;
        const auto storage=fs::path(o.out);
        const auto checkpoint_path=storage/"state.json";
        const auto database_path=storage/"state.sqlite3";
        std::unique_ptr<Ledger> ledger;
        if(o.resume){
            J saved;
            if(fs::exists(database_path)){
                ledger=std::make_unique<Ledger>(database_path);
                saved=ledger->restore(archive,o.seed);
            }else{
                std::ifstream in(checkpoint_path);
                require(bool(in),"--resume requires an existing SQLite or legacy checkpoint");
                in>>saved;
                require(saved.at("schema")=="craft-mass-v1"&&saved.at("rounds")==o.rounds&&saved.at("seed")==o.seed,
                        "incompatible legacy checkpoint");
                require(saved.at("clean_stop").get<bool>(),"legacy checkpoint was not saved after a clean stop");
                if(saved.contains("family_details_file"))restore_compact_checkpoint(archive,storage,saved);
                else if(saved.contains("family_files"))restore_shared_checkpoint(archive,storage,saved);
                else if(saved.contains("families_file")){
                    std::ifstream records(storage/safe_file(saved.at("families_file")));
                    require(bool(records),"missing legacy checkpoint family file");
                    size_t count=0;
                    for(std::string line;std::getline(records,line);++count)
                        archive.restore(J{{"schema","craft-mass-v1"},{"rounds",o.rounds},{"families",J::array({J::parse(line)})}});
                    require(!records.bad()&&count==saved.at("family_count").get<size_t>(),"truncated legacy checkpoint records");
                }else archive.restore(saved);
                if(saved.contains("guidance_state"))archive.restore_guidance(saved.at("guidance_state"));
            }
            resume_generation=saved.value("resume_generation",uint64_t(0))+1;
            if(saved.contains("guidance_state"))previous_guidance=saved.at("guidance_state");
            if(saved.contains("recovery_origin"))recovery_origin=saved.at("recovery_origin");
            else if(saved.value("recovered_prefix_only",false))
                recovery_origin={{"recovered_prefix_only",true},{"recovered_families",saved.at("family_count")},
                                 {"last_status_family_count",saved.at("last_status_family_count")},{"source_file",saved.at("source_file")}};
        }else{
            require(!fs::exists(database_path)&&!fs::exists(checkpoint_path)&&!fs::exists(storage/"STATUS.json"),
                    "output contains an earlier run; use --resume or a new path");
        }
        archive.set_generation(resume_generation);
        J protocol={{"schema","craft-mass-v1"},{"rounds",o.rounds},{"cores",o.cores},{"seed",o.seed},{"time_budget_seconds",o.duration},{"quantum_seconds",o.quantum},{"session_cpu_seconds",o.session},{"resume_generation",resume_generation},{"probability_model","independent-round-key DDT"},{"coordinates","x[0] leftmost; each round MC-AC-ATK-PN-SB; R complete S-box layers"},{"activity_biases",{0.5,1,2,4,8}},{"cold_objective","Z_pattern(endpoint)^k for fixed-pattern search; Z_all(endpoint)^k for physical search; k=2 or 4"},{"benchmark_injection",false},{"tweak_difference","0000000000000000"},{"key_difference","zero"},{"seed_sources",{"common-value transfer from cipher linear map and DDT","full-support activity DP with exact XOR cancellations"}},{"post_deadline_evaluation",false},{"resource_policy","up to half worker slots for counting; search uses all slots when no counter is ready"}};
        protocol["evaluation_engine"]="resumable-variable-elimination-v2";
        protocol["recovery_origin"]=recovery_origin;
        protocol["checkpoint_format"]=ledger_format;
        protocol["evaluation_scheduling"]="ranked initial AIS, one low-ESS retry, completed-batch ESS-adjusted mass order for exact; no arrival override or candidate exclusion";
        protocol["evaluation_workspace_bytes_per_counter"]=contraction_resources().workspace_bytes;
        protocol["volatile_message_cache_bytes"]=contraction_resources().cache_bytes;
        protocol["exact_working_memory_bytes"]=contraction_resources().working_bytes;
        protocol["paused_counter_memory_bytes"]=contraction_resources().retained_bytes;
        protocol["memory_policy"]="CPU-scaled shared cache and exact workspace leases; cold counters retain disjoint conditioning receipts";
        protocol["resource_slicing"]="exact conditioning; live row and split cursors stay in memory";
        protocol["restart_feedback"]="best certified family, mass-weighted completed families, and retained uniform exploration";
        protocol["time_budget_scope"]="search and evaluation wall time; preparation and final commit reported separately";
        protocol["shutdown_policy"]="SQLite dirty-case transactions during search; flush the remaining changes and summary at shutdown";
        protocol["guidance"]={{"revision","skinny-midori-ais-selection-v1"},{"search_cpu_weights",o.search_weights },
            {"search_actions",{"new-pattern","fixed-pattern","all-pattern","same-endpoint-expansion","joint"}},
            {"ais_share_of_evaluation_cpu",o.ais_share},{"exact_probe_cpu_seconds",o.exact_probe},{"ais_bridges",o.ais_bridges},{"ais_paths_per_batch",o.ais_paths},
            {"selection_score","min(certified_F, max(0, AIS_F + log2(max(1,N/max(1,ESS)))))"},
            {"ais_recheck","one more full batch if ESS<8; only full batches affect rank"},
            {"exact_order","all pending candidates by ESS-adjusted mass; retained order breaks ties"},
            {"joint_min_second_to_first_exact_mass",o.joint_relative},{"ais_estimates_are_certificates",false},{"source_cpu_scope","current resume generation"},{"timing_scope","first/completed_elapsed paired with their resume_generation"}};
        atomic_json(fs::path(o.out)/"PROTOCOL.json",protocol);
        J metadata={{"schema","craft-mass-v1"},{"checkpoint_format",ledger_format},{"rounds",o.rounds},
                    {"seed",o.seed},{"resume_generation",resume_generation},{"clean_stop",false},
                    {"recovery_origin",recovery_origin}};
        if(!previous_guidance.is_null())metadata["guidance_state"]=previous_guidance;
        if(!ledger){
            auto pattern=(storage/".craft-ledger-import-XXXXXX").string();
            int scratch=::mkstemp(pattern.data());
            require(scratch>=0,"cannot stage SQLite ledger");
            ::close(scratch);
            try{
                auto importing=std::make_unique<Ledger>(pattern);
                importing->flush(archive,metadata);
                importing->close();
                fs::rename(pattern,database_path);
                durable_file(storage);
            }catch(...){fs::remove(pattern);throw;}
            ledger=std::make_unique<Ledger>(database_path);
        }else ledger->flush(archive,metadata,Clock::now());
        std::cout<<"CRAFT "<<o.rounds<<"r"<<std::endl;
        ActivityGraph graph;CommonGuide common(o.rounds);std::vector<std::unique_ptr<ActivityGuide>> guides;
        for(double mu:{0.5,1.0,2.0,4.0,8.0})guides.push_back(std::make_unique<ActivityGuide>(graph,o.rounds,mu));
        const auto started=Clock::now();
        const double preparation_seconds=seconds(command_started);
        const auto deadline=started+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration));
        const int worker_count=o.cores>1?o.cores-1:1;
        protocol["computation_workers"]=worker_count;
        protocol["resource_policy"]="one coordinator persists dirty cases; computation workers share search and evaluation";
        atomic_json(storage/"PROTOCOL.json",protocol);
        auto physical=std::make_shared<Model>(o.rounds,Pattern{},true);Metrics metrics;std::atomic<bool> stop{false};std::mutex error_mutex;std::string error;
        std::vector<std::thread> workers;std::atomic<int> finished_workers{0};
        JoinWorkers join_guard{stop,workers};
        for(int id=0;id<worker_count;++id)workers.emplace_back([&,id] {
            try {
                RNG rng(mix(o.seed^mix(uint64_t(id)+0x435055ULL)^mix(resume_generation+0x524553554D45ULL)));LocalSearch local(o.rounds,physical,guides,common,archive,metrics,o.session,o.search_weights);
                while(Clock::now()<deadline&&!stop.load(std::memory_order_relaxed)&&!interrupted) {
                    Family* f=nullptr;
                    if(worker_count>1||metrics.count_ns.load()<=metrics.search_ns.load())f=archive.take((worker_count+1)/2);
                    auto end=std::min(deadline,Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.quantum)));uint64_t begin=thread_ns();
                    if(f&&f->screening){
                        if(!f->screen)f->screen=std::make_unique<MassScreen>(*f->model,f->witness,o.ais_bridges);
                        ScreenSamples samples;auto needed=f->screen_evidence.samples_left();
                        while(samples.paths<needed&&Clock::now()<end&&!stop.load(std::memory_order_relaxed)&&!interrupted){
                            double weight=0;if(f->screen->step(rng,weight))samples.add(weight);
                        }
                        auto used=thread_ns()-begin;archive.release_screen(*f,double(used)/1e9,samples);
                        metrics.count_ns.fetch_add(thread_ns()-begin,std::memory_order_relaxed);
                    }else if(f) {
                        if(!f->counter)f->counter=std::make_unique<FamilyCounter>(f->model,f->witness);
                        while(!f->counter->done()&&Clock::now()<end&&!stop.load(std::memory_order_relaxed)&&!interrupted)f->counter->step();
                        auto used=thread_ns()-begin;archive.release(*f,double(used)/1e9,seconds(started));
                        metrics.count_ns.fetch_add(thread_ns()-begin,std::memory_order_relaxed);
                    } else {
                        ++metrics.search_workers;local.work(rng,end,stop,started);--metrics.search_workers;metrics.search_ns.fetch_add(thread_ns()-begin,std::memory_order_relaxed);
                    }
                }
            } catch(const std::exception& e){std::lock_guard<std::mutex> lock(error_mutex);if(error.empty())error=e.what();stop=true;}
            ++finished_workers;
        });
        std::ofstream log(fs::path(o.out)/"progress.jsonl",std::ios::app);log.exceptions(std::ios::failbit|std::ios::badbit);
        double search_stopped_elapsed=0,storage_cpu=0,storage_wall=0;J saving=J::object();
        auto report=[&](const std::string& state) {
            J s=archive.status();double elapsed=seconds(started),sc=double(metrics.search_ns.load())/1e9,ec=double(metrics.count_ns.load())/1e9;
            s["state"]=state;s["saving"]=saving;s["live_computation_workers"]=worker_count-finished_workers.load();s["elapsed_seconds"]=elapsed;s["search_cpu_seconds"]=sc;s["count_cpu_seconds"]=ec;s["active_elapsed_seconds"]=search_stopped_elapsed>0?search_stopped_elapsed:elapsed;s["checkpoint_seconds"]=search_stopped_elapsed>0?elapsed-search_stopped_elapsed:0;s["average_worker_cpu_cores"]=(sc+ec+storage_cpu)/std::max(search_stopped_elapsed>0?search_stopped_elapsed:elapsed,1e-9);s["search_workers"]=metrics.search_workers.load();s["search_steps"]=metrics.search_steps.load();s["fixed_pattern_steps"]=metrics.fixed_steps.load();s["all_pattern_steps"]=metrics.global_steps.load();s["endpoint_changes"]=metrics.endpoint_moves.load();s["csp_nodes"]=metrics.csp_nodes.load();s["unsatisfiable_patterns"]=metrics.unsat_patterns.load();
            s["ais_cpu_seconds"]=s["guidance"]["ais_cpu_seconds"];s["exact_cpu_seconds"]=ec-s["ais_cpu_seconds"].get<double>();
            s["cold_endpoint_changes"]=metrics.cold_endpoint_moves.load();s["cold_swap_attempts"]=metrics.cold_swap_attempts.load();s["cold_swap_accepts"]=metrics.cold_swap_accepts.load();
            s["collective_updates"]=metrics.collective_updates.load();s["common_value_seeds"]=metrics.common_seeds.load();s["csp_yields"]=metrics.csp_yields.load();s["deferred_csp"]=metrics.pending_csp.load();
            auto& evaluation=contraction_resources();s["evaluation_engine"]="resumable-variable-elimination-v2";
            s["message_cache_hits"]=evaluation.hits.load();s["message_cache_misses"]=evaluation.misses.load();s["message_cache_stores"]=evaluation.stores.load();s["message_cache_evictions"]=evaluation.evictions.load();s["contraction_rows"]=evaluation.rows.load();s["exact_resource_splits"]=evaluation.splits.load();
            s["exact_working_memory_bytes"]=evaluation.working_bytes;
            s["paused_counter_memory_budget_bytes"]=evaluation.retained_bytes;
            s["message_cache_bytes"]=evaluation.cache_resident_bytes.load();
            s["message_cache_budget_bytes"]=evaluation.cache_bytes;
            uint64_t virtual_pages=0,resident_pages=0;
            std::ifstream resident("/proc/self/statm");resident>>virtual_pages>>resident_pages;
            s["resident_memory_bytes"]=resident_pages*uint64_t(::sysconf(_SC_PAGESIZE));
            s["budget_seconds"]=o.duration;
            s["resume_generation"]=resume_generation;
            s["remaining_search_seconds"]=std::max(0.0,o.duration-(search_stopped_elapsed>0?search_stopped_elapsed:elapsed));
            s["preparation_seconds"]=preparation_seconds;
            s["storage_cpu_seconds"]=storage_cpu;s["storage_wall_seconds"]=storage_wall;
            s["storage_pending_cases"]=archive.pending_storage();s["storage_committed_families"]=ledger->stored_families();
            s["expansion_steps"]=metrics.expansion_steps.load();s["joint_steps"]=metrics.joint_steps.load();
            s["joint_pattern_changes"]=metrics.joint_pattern_changes.load();s["joint_mixed_cold_visits"]=metrics.joint_mixed_visits.load();
            atomic_json(fs::path(o.out)/"STATUS.json",s);log<<s.dump()<<'\n';log.flush();
            std::cout<<std::fixed<<std::setprecision(1)<<"elapsed="<<elapsed/60<<" min | best=";
            if(s["best"].is_null())std::cout<<"pending";
            else std::cout<<std::setprecision(9)<<"2^-"<<s["best"]["mass"]["negative_log2"].get<double>()
                <<" | "<<s["best"]["input"].get<std::string>()<<" -> "<<s["best"]["output"].get<std::string>();
            if(state!="RUNNING")std::cout<<" | "<<state;
            std::cout<<std::endl;
        };
        auto next_report=Clock::now(),next_flush=Clock::now(),next_trim=Clock::now();
        uint64_t trimmed_compactions=0;
        auto flush=[&](Clock::time_point until){
            auto wall=Clock::now();auto cpu=thread_ns();
            ledger->flush(archive,metadata,until);
            storage_cpu+=double(thread_ns()-cpu)/1e9;
            storage_wall+=seconds(wall);
        };
        try{
            while(Clock::now()<deadline&&!stop.load()&&!interrupted){
                auto now=Clock::now();
#ifdef __GLIBC__
                if(now>=next_trim){
                    auto compacted=archive.status().at("counter_compactions").get<uint64_t>();
                    if(compacted!=trimmed_compactions){malloc_trim(0);trimmed_compactions=compacted;}
                    next_trim=now+std::chrono::seconds(5);
                }
#endif
                if(now>=next_report){report("RUNNING");next_report=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.report));}
                if(now>=next_flush){
                    flush(std::min(deadline,Clock::now()+std::chrono::milliseconds(50)));
                    next_flush=archive.pending_storage()?Clock::now():Clock::now()+std::chrono::seconds(2);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }catch(const std::exception& failure){
            std::lock_guard<std::mutex> guard(error_mutex);
            if(error.empty())error=failure.what();
        }
        stop=true;
        auto last_save_report=Clock::now();
        report("STOPPING_WORKERS");
        while(finished_workers.load()!=worker_count){
            if(seconds(last_save_report)>=o.report){report("STOPPING_WORKERS");last_save_report=Clock::now();}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for(auto& thread:workers)thread.join();
        search_stopped_elapsed=seconds(started);
        const auto final_status=archive.status();
        J result={{"schema","craft-mass-results-v1"},{"rounds",o.rounds},{"model","independent-round-key DDT"},
                  {"coordinates","x0 leftmost; R complete MC-AC-ATK-PN-SB rounds"},{"best",final_status.at("best")},
                  {"families",final_status.at("families")},{"endpoints",final_status.at("endpoints")},
                  {"complete_families",final_status.at("complete_families")},{"resume_generation",resume_generation},
                  {"database_file","state.sqlite3"},{"checkpoint_format",ledger_format},{"checkpoint_complete",false},
                  {"active_elapsed_seconds",search_stopped_elapsed},{"preparation_seconds",preparation_seconds}};
        try{
            saving={{"phase","remaining_changes"}};report("SAVING_CHECKPOINT");
            metadata["clean_stop"]=error.empty();
            metadata["family_count"]=final_status.at("families");
            metadata["complete_families"]=final_status.at("complete_families");
            metadata["guidance_state"]=archive.guidance_checkpoint();
            metadata["best"]=final_status.at("best");
            metadata["active_elapsed_seconds"]=search_stopped_elapsed;
            metadata["preparation_seconds"]=preparation_seconds;
            flush(Clock::time_point::max());
            require(archive.pending_storage()==0,"uncommitted SQLite changes after worker shutdown");
            metadata["database_file"]="state.sqlite3";
            atomic_json(checkpoint_path,metadata,true);
            result["checkpoint_complete"]=error.empty();
            result["database_bytes"]=fs::file_size(database_path);
            result["checkpoint_seconds"]=seconds(started)-search_stopped_elapsed;
            atomic_json(storage/"RESULTS.json",result,true);
        }catch(const std::exception& failure){
            if(error.empty())error=failure.what();
            result["save_error"]=failure.what();
            atomic_json(storage/"RESULTS.json",result,true);
        }
        const char* final_phase=!error.empty()?"ERROR":(interrupted?"INTERRUPTED":"TIME_BUDGET_COMPLETE");
        saving={{"phase","finished"},{"checkpoint_complete",result.at("checkpoint_complete")}};
        report(final_phase);
        log.flush();log.close();
        ledger->close();
        std::cout.flush();std::cerr.flush();
        if(!error.empty())std::cerr<<"ERROR: "<<error<<std::endl;

        std::_Exit(error.empty()?0:1);
    }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<'\n';return 1;}
}
