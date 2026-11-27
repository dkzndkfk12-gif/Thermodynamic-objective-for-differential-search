#pragma once
#include "counter.hpp"
#include "mass_screen.hpp"
#include "trail_record.hpp"
#include <list>
#include <unordered_map>

namespace twine {
class EvaluatedMassTree {
    size_t capacity=1;
    std::vector<double> tree=std::vector<double>(2,-INFINITY);
    static double add(double a,double b){if(a==-INFINITY)return b;if(b==-INFINITY)return a;double hi=std::max(a,b);return hi+std::log2(1+std::exp2(std::min(a,b)-hi));}
public:
    void set(size_t id,double logmass){
        if(id>=capacity){size_t next=capacity;while(id>=next)next*=2;std::vector<double> grown(2*next,-INFINITY);for(size_t i=0;i<capacity;++i)grown[next+i]=tree[capacity+i];capacity=next;tree=std::move(grown);for(size_t i=capacity;i-->1;)tree[i]=add(tree[2*i],tree[2*i+1]);}
        size_t i=capacity+id;tree[i]=logmass;while(i>1){i/=2;tree[i]=add(tree[2*i],tree[2*i+1]);}
    }
    bool empty()const{return tree[1]==-INFINITY;}
    size_t draw(RNG& rng)const{require(!empty(),"empty evaluated mass feedback");size_t i=1;while(i<capacity){double p=std::exp2(tree[2*i]-tree[i]);i=2*i+(unit(rng)>=p);}return i-capacity;}
};
struct Family {
    size_t id=0;
    std::shared_ptr<const Model> model;
    Values witness;
    Values feedback_witness;
    int best_weight=0;
    std::unique_ptr<FamilyCounter> counter;
    std::unique_ptr<MassScreen> screen;
    ScreenEvidence screen_evidence;
    bool screening=false,screen_queued=false,fresh_pending=false;
    ScreenQueue::iterator screen_position;
    std::list<size_t>::iterator fresh_position;
    double completed_elapsed=0;
    uint64_t first_generation=0,completed_generation=0;
    Dyadic lower;
    std::string endpoint,source;
    double cpu=0,first_elapsed=0;
    uint64_t nodes=0;
    bool complete=false,in_flight=false,pending=false;
    std::list<size_t>::iterator fifo;
    std::set<std::pair<double,size_t>>::iterator priority;
};
class Archive {
    mutable std::mutex mutex;
    std::unordered_map<std::string,std::shared_ptr<const Model>> models;
    std::unordered_map<std::string,size_t> index;
    std::vector<std::unique_ptr<Family>> families;
    std::unordered_map<std::string,Dyadic> endpoints;

    struct PatternFeedback{size_t best;};
    std::unordered_map<std::string,size_t> pattern_ids;
    std::vector<PatternFeedback> pattern_feedback;
    EvaluatedMassTree pattern_mass,joint_mass;
    struct EndpointMembers {
        size_t best=0,first=0,count=0;
        size_t evaluated_first=SIZE_MAX,evaluated_second=SIZE_MAX,joint_index=SIZE_MAX;
    };
    std::unordered_map<std::string,EndpointMembers> endpoint_members;
    std::vector<std::pair<size_t,size_t>> joint_pairs;
    struct Expansion {Dyadic baseline,added;uint64_t new_patterns=0;};
    std::unordered_map<std::string,Expansion> expansions;
    std::list<size_t> fresh;
    ScreenQueue screen_queue;
    uint64_t evaluation_turn=0,finished_screen_paths=0,positive_screen_paths=0,ais_ranked_exact_slices=0;
    uint64_t screen_batch_paths=32,completed_screen_batches=0,started_screen_batches=0;
    int running_screens=0;
    double total_evaluation_cpu=0,total_screen_cpu=0,screen_share=0,probe_seconds=.05,joint_min_relative=0;
    J source_totals=J::object();
    double best_certified_elapsed=0;
    uint64_t current_generation=0;
    void source_add(const std::string& source,const char* key,double value){
        if(restoring)return;
        auto& item=source_totals[source];if(item.is_null())item=J::object();item[key]=item.value(key,0.0)+value;
    }
    static double priority_score(const Family& f){
        return f.screen_evidence.priority(f.lower.neglog2());
    }
    void pattern_improved(const Family& f){
        auto [it,created]=pattern_ids.emplace(f.model->id,pattern_feedback.size());
        if(created)pattern_feedback.push_back({f.id});
        auto& current=pattern_feedback[it->second];
        if(f.lower.compare(families[current.best]->lower)>=0){current.best=f.id;pattern_mass.set(it->second,-f.lower.neglog2());}
    }
    void joint_evaluated(const Family& f){
        auto& ep=endpoint_members.at(f.endpoint);
        std::array<size_t,3> choices{ep.evaluated_first,ep.evaluated_second,f.id};
        std::sort(choices.begin(),choices.end(),[&](size_t a,size_t b){
            if(a==SIZE_MAX||b==SIZE_MAX)return a!=SIZE_MAX&&b==SIZE_MAX;
            int order=families[a]->lower.compare(families[b]->lower);return order?order>0:a<b;
        });
        ep.evaluated_first=choices[0];ep.evaluated_second=SIZE_MAX;
        for(size_t id:choices)if(id!=SIZE_MAX&&id!=ep.evaluated_first){ep.evaluated_second=id;break;}
        if(ep.evaluated_second==SIZE_MAX)return;
        const auto& first=*families[ep.evaluated_first];const auto& second=*families[ep.evaluated_second];
        double relative=std::exp2(first.lower.neglog2()-second.lower.neglog2());
        if(relative<joint_min_relative){if(ep.joint_index!=SIZE_MAX)joint_mass.set(ep.joint_index,-INFINITY);return;}
        if(ep.joint_index==SIZE_MAX){ep.joint_index=joint_pairs.size();joint_pairs.emplace_back();}
        joint_pairs[ep.joint_index]={first.id,second.id};Dyadic sum=first.lower;sum+=second.lower;
        joint_mass.set(ep.joint_index,2-first.lower.neglog2()-second.lower.neglog2()+sum.neglog2());
    }
    void queue_screen(Family& f){
        if(f.screen_queued){screen_queue.erase(f.screen_position);f.screen_queued=false;}
        int group=f.screen_evidence.queue_group();
        if(f.pending&&!f.complete&&f.cpu>=probe_seconds&&screen_share>0&&group>=0){
            f.screen_position=screen_queue.emplace(group,priority_score(f),f.id).first;f.screen_queued=true;
        }
    }
    ScreenQueue::iterator screen_choice(){
        auto chosen=screen_queue.end();
        auto consider=[&](int group){
            auto it=screen_queue.lower_bound({group,-INFINITY,0});
            if(it!=screen_queue.end()&&std::get<0>(*it)==group&&
               (chosen==screen_queue.end()||std::make_pair(std::get<1>(*it),std::get<2>(*it))<
                std::make_pair(std::get<1>(*chosen),std::get<2>(*chosen))))chosen=it;
        };

        consider(0);
        if(chosen==screen_queue.end()){
            if(evaluation_turn%3!=0)consider(1);
            if(evaluation_turn%8==0)consider(2);
        }
        return chosen;
    }

    std::list<size_t> fifo;
    std::set<std::pair<double,size_t>> priority;
    std::string best;
    size_t best_family=0;
    int running_counters=0;
    bool restoring=false;
    void enqueue(Family& f) { require(!f.pending&&!f.in_flight,"duplicate counter scheduling");f.fifo=fifo.insert(fifo.end(),f.id);f.priority=priority.emplace(priority_score(f),f.id).first;f.pending=true;queue_screen(f); }
    void improve_endpoint(Family& f,const Dyadic& delta,bool created=false,double elapsed=0) {
        auto& sum=endpoints[f.endpoint];sum+=delta;require(sum.compare(Dyadic(1,0))<=0,"probability lower bound exceeds one");
        auto& members=endpoint_members[f.endpoint];
        if(!members.count){members.first=members.best=f.id;source_add(f.source,"new_endpoints",1);}
        if(created){++members.count;source_add(f.source,"new_families",1);}
        if(f.lower.compare(families[members.best]->lower)>0)members.best=f.id;
        auto expansion=expansions.find(f.endpoint);
        if(f.source=="same-endpoint-expansion"&&expansion!=expansions.end()){
            expansion->second.added+=delta;if(created)++expansion->second.new_patterns;
        }
        if(best.empty()||sum.compare(endpoints.at(best))>0)best=f.endpoint;
        if(best==f.endpoint){best_family=members.best;if(elapsed>0&&!restoring)best_certified_elapsed=elapsed;}
        pattern_improved(f);
    }
    void update_witness(Family& f,const Trail& t,int weight,double elapsed) {
        if(weight>=f.best_weight)return;
        f.best_weight=weight;f.feedback_witness=f.model->encode(t);Dyadic proof(1,unsigned(weight));
        if(proof.compare(f.lower)>0) {
            if(f.pending)priority.erase(f.priority);
            auto delta=proof.difference(f.lower);f.lower=std::move(proof);improve_endpoint(f,delta,false,elapsed);
            if(f.pending){f.priority=priority.emplace(priority_score(f),f.id).first;queue_screen(f);}
        }
    }
public:
    const int rounds;
    std::atomic<uint64_t> submissions{0},duplicates{0},completed{0};
    explicit Archive(int r):rounds(r) {}
    size_t submit(const Trail& t,const std::string& source,double elapsed) {
        int weight=trail_weight(t);require(int(t.size())==rounds+1&&nonzero(t.front())&&weight>=0,"invalid submitted trail");
        auto pattern=activity(t);auto id=pattern_key(pattern);std::string ep=hex(t.front())+":"+hex(t.back()),key=ep+"/"+id;
        std::shared_ptr<const Model> model;
        { std::lock_guard<std::mutex> lock(mutex);++submissions;auto found=index.find(key);if(found!=index.end()){++duplicates;update_witness(*families[found->second],t,weight,elapsed);return found->second;}auto it=models.find(id);if(it!=models.end())model=it->second; }
        if(!model)model=std::make_shared<Model>(rounds,pattern);
        auto f=std::make_unique<Family>();f->model=model;f->witness=model->encode(t);f->feedback_witness=f->witness;f->best_weight=weight;require(model->legal(f->witness),"pattern encoding changed witness");f->lower=Dyadic(1,unsigned(weight));f->endpoint=ep;f->source=source;f->first_elapsed=elapsed;f->first_generation=current_generation;
        std::lock_guard<std::mutex> lock(mutex);auto found=index.find(key);if(found!=index.end()){++duplicates;update_witness(*families[found->second],t,weight,elapsed);return found->second;}
        auto [it,inserted]=models.emplace(id,model);(void)inserted;f->model=it->second;f->id=families.size();size_t answer=f->id;
        index[key]=answer;families.push_back(std::move(f));enqueue(*families.back());improve_endpoint(*families.back(),families.back()->lower,true,elapsed);auto& added=*families.back();added.fresh_position=fresh.insert(fresh.end(),answer);added.fresh_pending=true;return answer;
    }
    Family* take(int max_parallel) {
        std::lock_guard<std::mutex> lock(mutex);if(fifo.empty()||running_counters>=max_parallel)return nullptr;
        ++evaluation_turn;
        auto choice=screen_choice();
        const int auxiliary=std::max(1,max_parallel/8);
        bool screen=screen_share>0&&choice!=screen_queue.end()&&running_screens<auxiliary&&
            total_screen_cpu<=screen_share*total_evaluation_cpu;
        size_t id=screen?std::get<2>(*choice):priority.begin()->second;
        auto& f=*families[id];
        if(screen){
            if(!f.screen_evidence.pending_batch){f.screen_evidence.start_batch(screen_batch_paths);++started_screen_batches;}
            ++running_screens;
        }else if(f.screen_evidence.positive)++ais_ranked_exact_slices;
        if(f.fresh_pending){fresh.erase(f.fresh_position);f.fresh_pending=false;}
        if(f.screen_queued){screen_queue.erase(f.screen_position);f.screen_queued=false;}
        fifo.erase(f.fifo);priority.erase(f.priority);f.pending=false;f.in_flight=true;f.screening=screen;++running_counters;return &f;
    }
    void release(Family& f,double cpu_seconds,double elapsed=0) {
        require(bool(f.counter),"missing family counter");auto lower=f.counter->mass();bool evaluated=lower.n>0;bool done=f.counter->done();auto nodes=f.counter->nodes();
        if(done)f.screen.reset();
        std::lock_guard<std::mutex> lock(mutex);require(f.in_flight,"counter ownership lost");f.in_flight=false;--running_counters;if(!restoring){total_evaluation_cpu+=cpu_seconds;source_add(f.source,"exact_cpu_seconds",cpu_seconds);}
        if(done)require(lower.compare(Dyadic(1,unsigned(f.best_weight)))>=0,"completed pattern excludes a legal witness");
        if(lower.compare(f.lower)>0){auto delta=lower.difference(f.lower);f.lower=std::move(lower);improve_endpoint(f,delta,false,elapsed);}
        f.cpu+=cpu_seconds;f.nodes=nodes;

        if(evaluated)joint_evaluated(f);
        if(done){f.complete=true;f.completed_elapsed=elapsed;f.completed_generation=current_generation;++completed;source_add(f.source,"completed_families",1);}else {enqueue(f);queue_screen(f);}
    }

    void set_generation(uint64_t generation){current_generation=generation;}
    void configure_guidance(double share,double probe,double joint_relative,uint64_t batch_paths=32){
        require(share>=0&&share<1&&probe>=0&&joint_relative>=0&&joint_relative<=1&&batch_paths>0,
                "invalid guidance configuration");
        std::lock_guard<std::mutex> guard(mutex);
        screen_share=share;probe_seconds=probe;joint_min_relative=joint_relative;screen_batch_paths=batch_paths;
        for(const auto& family:families)queue_screen(*family);
    }
    Trail pattern_seed(RNG& rng)const{
        std::lock_guard<std::mutex> lock(mutex);if(pattern_feedback.empty())return {};
        size_t id=draw(rng,2)?best_family:pattern_feedback[pattern_mass.draw(rng)].best;
        return families[id]->model->decode(families[id]->feedback_witness);
    }
    Trail expansion_seed(RNG& rng){
        std::lock_guard<std::mutex> lock(mutex);if(families.empty())return {};
        size_t id=draw(rng,2)?best_family:pattern_feedback[pattern_mass.draw(rng)].best;
        const auto& f=*families[id];auto [it,created]=expansions.emplace(f.endpoint,Expansion{});
        if(created)it->second.baseline=endpoints.at(f.endpoint);
        return f.model->decode(f.feedback_witness);
    }
    std::vector<Trail> joint_seeds(RNG& rng)const{
        std::lock_guard<std::mutex> lock(mutex);if(joint_mass.empty())return {};
        auto pair=joint_pairs[joint_mass.draw(rng)];
        const auto& a=families[pair.first]->lower;const auto& b=families[pair.second]->lower;
        Dyadic sum=a;sum+=b;

        double log_accept=(2-a.neglog2()-b.neglog2()+2*sum.neglog2())*std::log(2.0);
        if(std::log(unit(rng))>log_accept)return {};
        std::vector<Trail> starts;
        for(size_t id:{pair.first,pair.second}){const auto& f=*families[id];starts.push_back(f.model->decode(f.feedback_witness));}
        return starts;
    }
    void search_spent(const std::string& action,double cpu){std::lock_guard<std::mutex> lock(mutex);source_add(action,"search_cpu_seconds",cpu);}
    J guidance_status()const{
        return {{"source_totals",source_totals},{"ais_cpu_seconds",total_screen_cpu},{"evaluation_cpu_seconds",total_evaluation_cpu},
                {"selection_policy","skinny-midori-ais-selection-v1"},{"ess_adjusted_priority",true},
                {"ais_batch_paths",screen_batch_paths},{"ais_batches_started",started_screen_batches},{"ais_batches_complete",completed_screen_batches},{"ais_running",running_screens},{"ais_paths",finished_screen_paths},{"ais_positive_paths",positive_screen_paths},{"ais_ranked_exact_slices",ais_ranked_exact_slices},{"ais_pending",screen_queue.size()},{"fresh_pending",fresh.size()},{"joint_pairs",joint_pairs.size()},
                {"expansion_targets",expansions.size()},{"best_certified_elapsed",best_certified_elapsed}};
    }
    J guidance_checkpoint()const{
        std::lock_guard<std::mutex> lock(mutex);J saved=J::array();
        for(const auto& [ep,e]:expansions)saved.push_back({{"endpoint",ep},{"baseline",e.baseline.json()},{"added",e.added.json()},{"new_patterns",e.new_patterns}});
        return {{"expansions",saved}};
    }
    void restore_guidance(const J& j){
        for(const auto& row:j.at("expansions")){
            auto ep=row.at("endpoint").get<std::string>();require(endpoints.count(ep)>0,"missing expansion endpoint");
            auto dyadic=[](const J& q){return Dyadic(mpz_class(q.at("numerator").get<std::string>()),q.at("denominator_power").get<unsigned>());};
            expansions[ep]={dyadic(row.at("baseline")),dyadic(row.at("added")),row.at("new_patterns").get<uint64_t>()};
        }
    }
    void release_screen(Family& f,double cpu,const ScreenSamples& samples){
        std::lock_guard<std::mutex> lock(mutex);require(f.in_flight&&f.screening,"AIS ownership lost");
        auto paths=f.screen_evidence.paths,positive=f.screen_evidence.positive;
        bool finished=f.screen_evidence.collect(samples);
        finished_screen_paths+=f.screen_evidence.paths-paths;
        positive_screen_paths+=f.screen_evidence.positive-positive;
        if(finished){++completed_screen_batches;f.screen.reset();}
        f.screen_evidence.cpu+=cpu;total_screen_cpu+=cpu;total_evaluation_cpu+=cpu;
        source_add(f.source,"ais_cpu_seconds",cpu);
        f.screening=false;f.in_flight=false;--running_counters;--running_screens;
        enqueue(f);
    }
    Trail feedback(RNG& rng,bool strongest) const {
        std::lock_guard<std::mutex> lock(mutex);if(families.empty())return {};size_t i=strongest?best_family:size_t(draw(rng,int(families.size())));return families[i]->model->decode(families[i]->feedback_witness);
    }
    J status() const {
        std::lock_guard<std::mutex> lock(mutex);J j={{"families",families.size()},{"patterns",models.size()},{"endpoints",endpoints.size()},{"pending",fifo.size()},{"count_workers",running_counters},{"complete_families",completed.load()},{"submissions",submissions.load()},{"duplicates",duplicates.load()}};
        if(!best.empty())j["best"]={{"input",best.substr(0,16)},{"output",best.substr(17)},{"mass",endpoints.at(best).json()},{"witness",trail_json(families[best_family]->model->decode(families[best_family]->feedback_witness))}};else j["best"]=nullptr;
        if(!best.empty()){
            const auto& member=endpoint_members.at(best);
            j["best"]["first_elapsed"]=families[member.first]->first_elapsed;
            j["best"]["first_resume_generation"]=families[member.first]->first_generation;
            j["best"]["registered_patterns"]=member.count;
            j["best"]["discovered_by"]=families[member.first]->source;
        }
        j["guidance"]=guidance_status();return j;
    }

    J checkpoint() const {
        std::lock_guard<std::mutex> lock(mutex);J a=J::array();
        for(const auto& f:families) {
            J v={{"witness",trail_json(f->model->decode(f->witness))},{"feedback_witness",trail_json(f->model->decode(f->feedback_witness))},{"source",f->source},{"first_elapsed",f->first_elapsed},{"cpu_seconds",f->cpu},{"lower",f->lower.json()},{"complete",f->complete}};
            v["screen"]=f->screen_evidence.json();v["completed_elapsed"]=f->completed_elapsed;v["first_resume_generation"]=f->first_generation;v["completed_resume_generation"]=f->completed_generation;
            if(f->counter)v["counter"]=f->counter->json();
            a.push_back(std::move(v));
        }return {{"schema","twine-mass-v1"},{"rounds",rounds},{"families",a}};
    }

    template<class Emit> void compact_records(Emit emit) const {
        for(const auto& f:families) {
            J row={{"trail",encode_trail_record(f->model->decode(f->feedback_witness))},
                   {"source",f->source},{"first_elapsed",f->first_elapsed}};
            if(f->first_generation)row["first_resume_generation"]=f->first_generation;
            if(f->screen_evidence.paths||f->screen_evidence.cpu>0)row["screen"]=f->screen_evidence.json();
            if(f->counter){
                row["counter"]=f->counter->compact_json();
                row["lower"]=J::array({f->lower.n.get_str(),f->lower.e});
                row["cpu_seconds"]=f->cpu;
                if(f->witness!=f->feedback_witness)
                    row["initial_trail"]=encode_trail_record(f->model->decode(f->witness));
                if(f->complete){
                    row["completed_elapsed"]=f->completed_elapsed;
                    if(f->completed_generation)row["completed_resume_generation"]=f->completed_generation;
                }
            }
            emit(row);
        }
    }
    template<class Emit> void endpoint_records(Emit emit) const {
        std::lock_guard<std::mutex> lock(mutex);
        for(const auto& kv:endpoints){
            const auto& member=endpoint_members.at(kv.first);
            J row={{"input",kv.first.substr(0,16)},{"output",kv.first.substr(17)},{"lower_bound",kv.second.json()},
                   {"registered_patterns",member.count},{"first_elapsed",families[member.first]->first_elapsed},{"first_resume_generation",families[member.first]->first_generation}};
            auto it=expansions.find(kv.first);
            if(it!=expansions.end())row["same_endpoint_expansion"]={{"baseline",it->second.baseline.json()},
                {"added_from_new_patterns",it->second.added.json()},{"new_patterns",it->second.new_patterns},
                {"relative_added",it->second.added.n>0?std::exp2(it->second.baseline.neglog2()-it->second.added.neglog2()):0.0}};
            emit(row);
        }
    }
private:
    void restore_family(const J& row,bool compact){
        auto source=row.at("source").get<std::string>();
        double elapsed=row.at("first_elapsed").get<double>();
        auto generation=row.value("first_resume_generation",uint64_t(0));
        auto old_size=families.size();
        Trail first,feedback;
        if(compact){
            feedback=decode_trail_record(row.at("trail").get<std::string>(),rounds);
            if(row.contains("initial_trail"))first=decode_trail_record(row.at("initial_trail").get<std::string>(),rounds);
            else first=std::move(feedback);
        }else{
            first=trail_from_json(row.at("witness"));
            if(row.contains("feedback_witness"))feedback=trail_from_json(row.at("feedback_witness"));
        }
        size_t i=submit(first,source,elapsed);
        require(i==old_size,"duplicate checkpoint family");
        auto& f=*families[i];
        f.first_generation=generation;
        if(row.contains("screen"))f.screen_evidence.restore(row.at("screen"));
        if(!feedback.empty()&&feedback!=first)
            require(submit(feedback,source,elapsed)==i,"feedback witness has different endpoints or pattern");

        priority.erase(f.priority);f.priority=priority.emplace(priority_score(f),f.id).first;queue_screen(f);
        if(!row.contains("counter")){
            require(!row.value("complete",false)&&(!compact||!row.contains("completed_elapsed")),
                    "finished checkpoint family lacks counter");
            return;
        }
        fresh.erase(f.fresh_position);
        f.fresh_pending=false;

        fifo.erase(f.fifo);
        priority.erase(f.priority);
        if(f.screen_queued){screen_queue.erase(f.screen_position);f.screen_queued=false;}
        f.pending=false;
        f.in_flight=true;
        ++running_counters;
        f.counter=std::make_unique<FamilyCounter>(f.model,f.witness);
        const auto& lower=row.at("lower");
        Dyadic recorded;
        if(compact){
            f.counter->restore_compact(row.at("counter"));
            recorded=Dyadic(mpz_class(lower.at(0).get<std::string>()),lower.at(1).get<unsigned>());
            require(f.counter->done()==row.contains("completed_elapsed"),"compact counter completion mismatch");
        }else{
            f.counter->restore(row.at("counter"));
            recorded=Dyadic(mpz_class(lower.at("numerator").get<std::string>()),lower.at("denominator_power").get<unsigned>());
            require(f.counter->done()==row.at("complete").get<bool>(),"legacy counter completion mismatch");
        }
        release(f,row.at("cpu_seconds").get<double>(),row.value("completed_elapsed",0.0));
        f.completed_generation=row.value("completed_resume_generation",uint64_t(0));
        require(f.lower.compare(recorded)==0,"checkpoint mass/frontier mismatch");
    }
public:
    void restore_record(const J& row,bool compact){
        restoring=true;
        restore_family(row,compact);
        restoring=false;
    }
    void restore(const J& j) {
        require(j.at("schema")=="twine-mass-v1"&&j.at("rounds")==rounds,"incompatible run checkpoint");
        for(const auto& row:j.at("families"))restore_record(row,false);
    }
    J results() const {
        std::lock_guard<std::mutex> lock(mutex);std::vector<std::pair<std::string,Dyadic>> order(endpoints.begin(),endpoints.end());
        std::sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.second.compare(b.second)>0;});J es=J::array(),fs=J::array();
        for(const auto& kv:order)es.push_back({{"input",kv.first.substr(0,16)},{"output",kv.first.substr(17)},{"lower_bound",kv.second.json()}});
        for(const auto& f:families) {
            J q={{"input",f->endpoint.substr(0,16)},{"output",f->endpoint.substr(17)},{"pattern_masks",f->model->pattern},{"active_sboxes",f->model->active},{"source",f->source},{"lower_bound",f->lower.json()},{"complete_for_this_pattern",f->complete},{"nodes",f->nodes},{"cpu_seconds",f->cpu},{"witness",trail_json(f->model->decode(f->feedback_witness))}};
            q["screen"]=f->screen_evidence.json();q["completed_elapsed"]=f->completed_elapsed;q["first_resume_generation"]=f->first_generation;q["completed_resume_generation"]=f->completed_generation;q["first_elapsed"]=f->first_elapsed;
            q["best_witness_weight"]=f->best_weight;
            if(f->counter){q["enumerated_lower_bound"]=f->counter->mass().json();q["covered_paths"]=f->counter->paths().get_str();q["weight_histogram"]=f->counter->weight_histogram();}fs.push_back(std::move(q));
        }
        return {{"schema","twine-mass-results-v1"},{"rounds",rounds},{"model","independent-round-key DDT"},{"coordinates","x0 leftmost; permutation after every round"},{"endpoint_lower_bounds",es},{"families",fs}};
    }
};
}
