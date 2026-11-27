#pragma once
#include "model.hpp"
#include <fstream>
#include <list>
#include <unordered_map>

namespace craft {

inline uint64_t saturated_add(uint64_t a,uint64_t b){return UINT64_MAX-a<b?UINT64_MAX:a+b;}
inline uint64_t saturated_mul(uint64_t a,uint64_t b){return b&&a>UINT64_MAX/b?UINT64_MAX:a*b;}
inline uint64_t available_memory(){
    std::ifstream f("/proc/meminfo");std::string name,unit;uint64_t value=0,available=0;
    while(f>>name>>value>>unit)if(name=="MemAvailable:"){available=saturated_mul(value,1024);break;}
    std::ifstream maximum("/sys/fs/cgroup/memory.max"),current("/sys/fs/cgroup/memory.current");std::string cap;uint64_t used=0;
    if(maximum>>cap&&current>>used&&cap!="max")try{uint64_t n=std::stoull(cap);available=std::min(available,n>used?n-used:0);}catch(const std::exception&){}
    require(available>0,"cannot determine available memory for exact contraction");return available;
}
struct ContractionResources {
    uint64_t workspace_bytes=0,cache_bytes=0,retained_bytes=0,working_bytes=0;
    std::atomic<uint64_t> hits{0},misses{0},stores{0},evictions{0},rows{0},splits{0};
    std::atomic<uint64_t> cache_resident_bytes{0};
    void configure(int workers,uint64_t requested=0){
        constexpr uint64_t mib=1024*1024,gib=1024*mib;
        auto memory=available_memory();
        working_bytes=std::min(memory-memory/4,requested?requested:std::max(512*mib,uint64_t(workers)*512*mib));
        cache_bytes=std::min({16*gib,uint64_t(workers)*64*mib,working_bytes/8});
        retained_bytes=(working_bytes-cache_bytes)/2;
        auto compute=std::max(1,workers-1);
        workspace_bytes=std::max<uint64_t>(1,(working_bytes-cache_bytes-retained_bytes)/uint64_t((compute+1)/2));
    }
};
inline ContractionResources& contraction_resources(){static ContractionResources r;return r;}
struct SumTable;
struct TableSink {virtual ~TableSink()=default;virtual J reference(const SumTable&)=0;};
struct TableSource {virtual ~TableSource()=default;virtual std::shared_ptr<SumTable> load(uint64_t)=0;};
inline thread_local TableSink* checkpoint_table_sink=nullptr;
inline thread_local TableSource* checkpoint_table_source=nullptr;
struct SumTable {
    std::vector<int> scope;
    std::vector<uint16_t> domains;
    std::vector<mpz_class> data;
    std::string key;
    unsigned degree=0;
    unsigned coefficient_bits=0;
    uint64_t limb_bytes=0;
    bool payload_tracked=false;
    void track_payload(){
        limb_bytes=0;
        for(const auto& value:data)limb_bytes=saturated_add(limb_bytes,mpz_size(value.get_mpz_t())*sizeof(mp_limb_t));
        payload_tracked=true;
    }
    void append(mpz_class value){
        if(!payload_tracked)track_payload();
        limb_bytes=saturated_add(limb_bytes,mpz_size(value.get_mpz_t())*sizeof(mp_limb_t));
        data.push_back(std::move(value));
    }
    uint64_t bytes()const{
        uint64_t n=sizeof(*this)+key.capacity()+scope.capacity()*sizeof(int)+domains.capacity()*sizeof(uint16_t)+data.capacity()*sizeof(mpz_class);
        if(payload_tracked)return saturated_add(n,limb_bytes);
        for(const auto& value:data)n=saturated_add(n,mpz_size(value.get_mpz_t())*sizeof(mp_limb_t));
        return n;
    }
    const mpz_class& at(const Values& x)const{size_t i=0;for(size_t k=0;k<scope.size();++k){unsigned mask=domains[k];i=i*size_t(pop(mask))+size_t(pop(mask&((1u<<x[scope[k]])-1)));}return data[i];}
    J json()const{if(checkpoint_table_sink)return checkpoint_table_sink->reference(*this);J a=J::array();for(const auto& x:data)a.push_back(x.get_str(16));return {{"scope",scope},{"domains",domains},{"data",a},{"key",key},{"degree",degree},{"coefficient_bits",coefficient_bits}};}
    uint64_t cells()const{uint64_t n=1;for(auto d:domains)n=saturated_mul(n,pop(d));return n;}
    static std::shared_ptr<SumTable> read(const J& j,bool prefix=false){
        std::shared_ptr<SumTable> t;
        if(j.contains("message_ref")){require(checkpoint_table_source!=nullptr,"message reference without checkpoint index");t=checkpoint_table_source->load(j.at("message_ref").get<uint64_t>());if(prefix)t=std::make_shared<SumTable>(*t);}
        else{t=std::make_shared<SumTable>();t->scope=j.at("scope").get<std::vector<int>>();t->domains=j.at("domains").get<std::vector<uint16_t>>();t->key=j.at("key").get<std::string>();t->degree=j.at("degree").get<unsigned>();t->coefficient_bits=j.value("coefficient_bits",0u);if(!t->coefficient_bits&&t->key.rfind("SUM/",0)==0)t->coefficient_bits=unsigned(std::stoul(t->key.substr(4)));for(const auto& x:j.at("data"))t->data.emplace_back(x.get<std::string>(),16);}
        require(t->scope.size()==t->domains.size(),"bad message shape");for(auto d:t->domains)require(d!=0,"empty message domain");auto n=t->cells();require(prefix?n>=t->data.size():n==t->data.size(),"truncated message");for(const auto& x:t->data)require(x>=0,"negative message");t->track_payload();return t;
    }
};

class SumCache {
    struct Entry{std::shared_ptr<const SumTable> table;uint64_t bytes;std::list<const std::string*>::iterator recency;};
    struct Shard{std::mutex mutex;std::unordered_map<std::string,Entry> entries;std::list<const std::string*> lru;uint64_t bytes=0;};
    std::array<Shard,16> shards;
public:
    std::shared_ptr<const SumTable> get(const std::string& key){
        auto& r=contraction_resources();auto& s=shards[std::hash<std::string>{}(key)%shards.size()];std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);
        if(lock.owns_lock()){auto it=s.entries.find(key);if(it!=s.entries.end()){s.lru.splice(s.lru.end(),s.lru,it->second.recency);++r.hits;return it->second.table;}}
        ++r.misses;return {};
    }
    void put(std::shared_ptr<const SumTable> t){
        auto& r=contraction_resources();uint64_t bytes=saturated_add(t->bytes(),t->key.size()+sizeof(Entry)+64),budget=r.cache_bytes/shards.size();if(bytes>budget)return;
        auto& s=shards[std::hash<std::string>{}(t->key)%shards.size()];std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.entries.count(t->key))return;
        while(s.bytes>budget-bytes&&!s.lru.empty()){auto i=s.entries.find(*s.lru.front());s.bytes-=i->second.bytes;r.cache_resident_bytes-=i->second.bytes;s.entries.erase(i);s.lru.pop_front();++r.evictions;}
        auto key=t->key;s.lru.push_back(&t->key);auto position=std::prev(s.lru.end());s.entries.emplace(std::move(key),Entry{std::move(t),bytes,position});s.bytes+=bytes;r.cache_resident_bytes+=bytes;++r.stores;
    }
};
inline SumCache& sum_cache(){static SumCache c;return c;}
struct SumTerm {
    std::vector<int> scope;
    int factor=-1;
    std::shared_ptr<const SumTable> table;
    unsigned degree=0;
};
struct SumRegion {
    Domains domains;
    Domains base_domains;
    std::vector<SumTerm> live;
    int split=-1;
    uint16_t remaining=0;
    bool rebuild=false;
};
struct SumWork {
    int variable=-1;
    std::vector<size_t> bucket;
    std::shared_ptr<SumTable> output;
    size_t row=0;
};
class ResumableSum {
    const Model& model;
    const unsigned stride;
    uint64_t workspace_override=0;
    std::vector<int> original_factors;
    std::vector<SumRegion> stack;
    SumWork work;
    Values values;
    mpz_class accumulated=0;
    uint64_t visits=0,split_count=0,remaining_estimate=0,receipts=0;
    static uint64_t cells(const std::vector<int>& scope,const Domains& domains){uint64_t n=1;for(int v:scope)n=saturated_mul(n,unsigned(pop(domains[v])));return n;}
    void multiply(mpz_class& product,const SumTerm& term)const{
        if(term.table){product*=term.table->at(values);return;}
        const auto& f=model.factors[term.factor];int n=f.count(values);
        if(!n){product=0;return;}
        if(f.weighted){require(n==2||n==4,"non-normalized exact factor");if(n==4)product<<=stride;}
    }
    std::string atom_key(int id,const Domains& d)const{
        const auto& f=model.factors[id];std::string key="CRAFT-POLY-v1/"+std::to_string(stride)+"/"+model.id+"/"+std::to_string(id)+"/";
        for(int v:f.scope)key+=std::to_string(v)+":"+std::to_string(d[v])+",";
        return key;
    }
    std::string message_key(const SumRegion& region,int variable,const std::vector<size_t>& bucket)const{
        std::vector<std::string> ancestry;
        for(size_t id:bucket){const auto& term=region.live[id];std::string s=term.table?term.table->key:atom_key(term.factor,region.domains);
            if(term.table)for(int v:term.scope)if(pop(region.domains[v])==1)s+="/pin"+std::to_string(v)+"="+std::to_string(first(region.domains[v]));
            ancestry.push_back(std::move(s));}
        std::sort(ancestry.begin(),ancestry.end());std::string key="SUM/"+std::to_string(stride)+"/"+std::to_string(variable)+"/"+std::to_string(region.domains[variable])+"/";
        for(const auto& s:ancestry)key+=std::to_string(s.size())+":"+s;
        return key;
    }
    void install(std::shared_ptr<const SumTable> table){
        auto& region=stack.back();std::vector<SumTerm> next;size_t p=0;
        for(size_t i=0;i<region.live.size();++i){if(p<work.bucket.size()&&work.bucket[p]==i){++p;continue;}next.push_back(std::move(region.live[i]));}
        next.push_back({table->scope,-1,table,table->degree});region.live=std::move(next);region.domains[work.variable]=1;work=SumWork{};
    }
    bool prepare(uint64_t sibling_bytes){
        auto& region=stack.back();
        if(region.split>=0){
            if(!region.remaining){stack.pop_back();return false;}
            int a=first(region.remaining);region.remaining&=uint16_t(~(1u<<a));auto child=region;child.domains[region.split]=uint16_t(1u<<a);child.base_domains[region.split]=uint16_t(1u<<a);child.split=-1;child.remaining=0;stack.push_back(std::move(child));return false;
        }
        if(region.rebuild){
            region.domains=region.base_domains;
            for(int id:original_factors)region.live.push_back({model.factors[id].scope,id,{},unsigned(model.factors[id].weighted)});
            region.rebuild=false;
        }
        int best=-1;uint64_t best_cost=UINT64_MAX,best_size=UINT64_MAX;std::vector<int> best_scope;std::vector<size_t> best_bucket;
        std::set<int> variables;for(const auto& t:region.live)for(int v:t.scope)if(pop(region.domains[v])>1)variables.insert(v);
        for(int v:variables){std::set<int> merged;std::vector<size_t> bucket;
            for(size_t i=0;i<region.live.size();++i)if(std::binary_search(region.live[i].scope.begin(),region.live[i].scope.end(),v)){bucket.push_back(i);for(int u:region.live[i].scope)if(pop(region.domains[u])>1)merged.insert(u);}
            merged.erase(v);std::vector<int> scope(merged.begin(),merged.end());uint64_t size=cells(scope,region.domains),cost=saturated_mul(saturated_mul(size,pop(region.domains[v])),bucket.size());
            if(best<0||std::make_pair(cost,size)<std::make_pair(best_cost,best_size)){best=v;best_cost=cost;best_size=size;best_scope=std::move(scope);best_bucket=std::move(bucket);}
        }
        if(best<0){for(size_t v=0;v<values.size();++v)values[v]=uint8_t(first(region.domains[v]));mpz_class product=1;for(const auto& t:region.live){multiply(product,t);if(product==0)break;}accumulated+=product;stack.pop_back();++visits;++receipts;return false;}
        remaining_estimate=best_cost;
        unsigned degree=0;for(size_t i:best_bucket)degree+=region.live[i].degree;
        uint64_t bytes_per_cell=2*sizeof(mpz_class)+sizeof(mp_limb_t)+(uint64_t(degree)*stride+stride+7)/8;
        uint64_t bytes=saturated_mul(best_size,bytes_per_cell),resident=saturated_add(resident_bytes(),sibling_bytes);
        uint64_t budget=workspace_override?workspace_override:contraction_resources().workspace_bytes;
        if(bytes>budget||resident>budget-bytes){region.split=best;region.remaining=region.domains[best];++split_count;++contraction_resources().splits;return false;}
        work.variable=best;work.bucket=std::move(best_bucket);auto key=message_key(region,best,work.bucket);
        if(auto cached=sum_cache().get(key)){install(std::move(cached));return false;}
        auto table=std::make_shared<SumTable>();table->scope=std::move(best_scope);table->key=std::move(key);table->degree=degree;table->coefficient_bits=stride;for(int v:table->scope)table->domains.push_back(region.domains[v]);

        work.output=std::move(table);return true;
    }
public:
    ResumableSum(const Model& m,const Domains& root,const std::vector<int>& factors,uint64_t workspace=0)
        :model(m),stride(unsigned(4*(m.nv-m.ne)+1)),workspace_override(workspace),original_factors(factors),values(m.nv){
        if(!contraction_resources().workspace_bytes)contraction_resources().configure(1);
        SumRegion region;region.domains=region.base_domains=root;for(int id:factors)region.live.push_back({m.factors[id].scope,id,{},unsigned(m.factors[id].weighted)});stack.push_back(std::move(region));
    }
    bool done()const{return stack.empty();}
    const mpz_class& packed()const{return accumulated;}
    unsigned coefficient_bits()const{return stride;}
    uint64_t nodes()const{return visits;}
    uint64_t splits()const{return split_count;}
    uint64_t completed_regions()const{return receipts;}
    uint64_t remaining_work()const{return done()?0:remaining_estimate;}
    uint64_t resident_bytes()const{
        std::set<const SumTable*> tables;
        for(const auto& region:stack)for(const auto& term:region.live)if(term.table)tables.insert(term.table.get());
        if(work.output)tables.insert(work.output.get());
        uint64_t bytes=0;
        for(auto table:tables)bytes=saturated_add(bytes,table->bytes());
        return bytes;
    }
    void compact(){

        if(!stack.empty()&&stack.back().split<0){
            auto& region=stack.back();
            int variable=work.variable;
            if(variable<0||pop(region.base_domains[variable])<2){
                variable=-1;
                for(int v=model.ne;v<model.nv;++v)if(pop(region.base_domains[v])>1){variable=v;break;}
            }
            if(variable>=0){
                region.split=variable;region.remaining=region.base_domains[variable];
                ++split_count;++contraction_resources().splits;
            }
        }
        work=SumWork{};
        for(auto& region:stack){
            std::vector<SumTerm>().swap(region.live);
            region.domains=region.base_domains;
            region.rebuild=true;
        }
    }
    void completed_histogram(const std::vector<mpz_class>& histogram,uint64_t nodes){
        work=SumWork{};
        stack.clear();
        accumulated=0;
        for(size_t weak=0;weak<histogram.size();++weak)
            accumulated+=histogram[weak]<<(stride*(histogram.size()-1-weak));
        visits=nodes;receipts=1;remaining_estimate=0;
    }
    void step(uint64_t sibling_bytes=0){
        if(done())return;
        if(work.variable<0){prepare(sibling_bytes);return;}
        auto& region=stack.back();auto& table=*work.output;
        for(size_t v=0;v<values.size();++v)values[v]=uint8_t(first(region.domains[v]));
        size_t total=size_t(table.cells()),begin=work.row,end=std::min(total,work.row+32);
        for(;work.row<end;++work.row){
            size_t code=work.row;
            for(size_t k=table.scope.size();k>0;--k){unsigned d=table.domains[k-1];size_t pos=code%size_t(pop(d));code/=size_t(pop(d));while(pos--)d&=d-1;values[table.scope[k-1]]=uint8_t(first(d));}
            mpz_class sum=0;
            for(unsigned d=region.domains[work.variable];d;d&=d-1){values[work.variable]=uint8_t(first(d));mpz_class product=1;for(size_t id:work.bucket){multiply(product,region.live[id]);if(product==0)break;}sum+=product;++visits;}
            table.append(std::move(sum));
        }
        contraction_resources().rows.fetch_add(end-begin,std::memory_order_relaxed);
        remaining_estimate=saturated_mul(saturated_mul(total-work.row,pop(region.domains[work.variable])),work.bucket.size());
        if(work.row==total){std::shared_ptr<const SumTable> completed=work.output;sum_cache().put(completed);install(std::move(completed));}
    }
    J json()const{
        J regions=J::array();for(const auto& region:stack){J live=J::array();for(const auto& t:region.live){J q={{"scope",t.scope},{"factor",t.factor},{"degree",t.degree}};if(t.table)q["table"]=t.table->json();live.push_back(std::move(q));}regions.push_back({{"domains",region.domains},{"base_domains",region.base_domains},{"rebuild",region.rebuild},{"live",live},{"split",region.split},{"remaining",region.remaining}});}
        J j={{"stride",stride},{"regions",regions},{"accumulated",accumulated.get_str(16)},{"visits",visits},{"splits",split_count},{"remaining_work",remaining_estimate},{"receipts",receipts}};
        if(work.variable>=0)j["work"]={{"variable",work.variable},{"bucket",work.bucket},{"row",work.row},{"output",work.output->json()}};
        return j;
    }
    void restore(const J& j,const Domains& root){
        require(j.at("stride")==stride,"packed histogram stride mismatch");stack.clear();work=SumWork{};
        for(const auto& q:j.at("regions")){
            SumRegion region;
            region.domains=q.at("domains").get<Domains>();
            require(region.domains.size()==root.size(),"bad contraction domain length");
            for(size_t v=0;v<root.size();++v)
                require(region.domains[v]&&(region.domains[v]==1||(region.domains[v]&root[v])==region.domains[v]),
                        "contraction domain outside root");
            Domains expected=root;
            if(!stack.empty()){
                const auto& parent=stack.back();
                require(parent.split>=0,"contraction child has no conditioning variable");
                expected=parent.base_domains;
                unsigned visited=expected[parent.split]&~unsigned(parent.remaining);
                require(visited!=0,"contraction child has no visited branch");
                expected[parent.split]=uint16_t(1u<<(31-__builtin_clz(visited)));
            }
            region.base_domains=q.value("base_domains",expected);
            require(region.base_domains==expected,"contraction conditioning history changed");
            region.rebuild=q.value("rebuild",false);
            region.split=q.at("split").get<int>();
            region.remaining=q.at("remaining").get<uint16_t>();
            if(region.split>=0){
                require(size_t(region.split)<root.size()&&pop(region.base_domains[region.split])>1,
                        "bad contraction split");
                require((region.remaining&region.base_domains[region.split])==region.remaining,
                        "bad contraction branch mask");
            }else require(region.split==-1&&region.remaining==0,"bad unsplit contraction region");
            for(const auto& value:q.at("live")){
                SumTerm term;
                term.scope=value.at("scope").get<std::vector<int>>();
                term.factor=value.at("factor").get<int>();
                term.degree=value.at("degree").get<unsigned>();
                if(value.contains("table"))term.table=SumTable::read(value.at("table"));
                else require(term.factor>=0&&size_t(term.factor)<model.factors.size()&&
                             term.scope==model.factors[term.factor].scope,"invalid restored factor");
                region.live.push_back(std::move(term));
            }
            require(!region.rebuild||region.live.empty(),"compacted region contains stale messages");
            stack.push_back(std::move(region));
        }
        accumulated=mpz_class(j.at("accumulated").get<std::string>(),16);require(accumulated>=0,"negative accumulated mass");visits=j.at("visits").get<uint64_t>();split_count=j.at("splits").get<uint64_t>();remaining_estimate=j.at("remaining_work").get<uint64_t>();receipts=j.at("receipts").get<uint64_t>();
        if(j.contains("work")){const auto& w=j.at("work");require(!stack.empty()&&!stack.back().rebuild,"work without materialized region");work.variable=w.at("variable").get<int>();work.bucket=w.at("bucket").get<std::vector<size_t>>();work.row=w.at("row").get<size_t>();work.output=SumTable::read(w.at("output"),true);require(work.variable>=0&&size_t(work.variable)<root.size()&&work.row==work.output->data.size(),"invalid message cursor");for(auto id:work.bucket)require(id<stack.back().live.size(),"invalid message bucket");}
    }
};
}
