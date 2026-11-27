

bool service_batch=false;
double service_started=0;
unsigned service_changes=0;
static constexpr unsigned service_batch_jobs=16;
static constexpr double service_quantum=.025;
std::vector<Pool::Ready> service_launches;
std::vector<std::uint64_t> service_launch_ids;
std::vector<J> service_cleanup;
unsigned queue_capacity() const {return slots+std::min(slots,a.ready_ahead==UINT_MAX?slots:a.ready_ahead);}
bool admission_room() const {
    return used_cores(false)+used_cores(true)<queue_capacity() && active.size()<std::size_t(queue_capacity())*2;
}
unsigned pending_receipts() const {
    unsigned n=0;for(const auto& kv:active)n+=kv.second.state->phase.load()==4;return n;
}
unsigned drain_before_refill(){
    const auto pending=pending_receipts();
    if(!pending)return 0;
    auto& metrics=m["pipeline"];metrics["peak_pending_receipts"]=std::max<std::uint64_t>(pending,count(metrics,"peak_pending_receipts"));

    const bool pressure=pending>=slots||active.size()>=std::size_t(queue_capacity())*2;
    const auto drained=collect(pressure?std::min(pending,slots):service_batch_jobs,pressure);
    if(pressure){increment(metrics,"pressure_drains");increment(metrics,"pressure_receipts",drained);}
    return drained;
}
void begin_service() {
    require(!service_batch,"Nested pipeline transaction");db.exec("BEGIN IMMEDIATE");
    service_batch=true;service_started=now();service_changes=0;
}
void commit_service() {
    require(service_batch,"Missing pipeline transaction");const double t=now();
    if(service_changes||!dp.empty()||!dc.empty()) {
        for(const auto& id:dp)db.put("patterns",id,patterns.at(id));
        for(const auto& id:dc)db.put("cases",id,cases.at(id));
        m["elapsed"]=elapsed();db.put_campaign_state(m);
    }
    db.exec("COMMIT");service_batch=false;dp.clear();dc.clear();
    increment(m["pipeline"],"commits");
    m["pipeline"]["commit_wall_seconds"]=number(m["pipeline"],"commit_wall_seconds")+now()-t;
    auto launch=std::move(service_launches);service_launches.clear();service_launch_ids.clear();

    pool.enqueue_batch(std::move(launch));
    for(const auto& j:service_cleanup)cleanup_job(j);
    service_cleanup.clear();
}
void abort_service() {
    if(!service_batch)return;
    db.exec("ROLLBACK");service_batch=false;
    for(auto id:service_launch_ids){auto it=active.find(id);if(it!=active.end()){ram.release(it->second.state);active.erase(it);++ownership_revision;}}
    service_launch_ids.clear();service_launches.clear();service_cleanup.clear();

    patterns=db.load("patterns");cases=db.load("cases");jobs=db.load("jobs");m=db.load("meta").at("state");
    rebuild_endpoint_index();dp.clear();dc.clear();rank_index.clear();rank_keys.clear();rank_dirty.clear();
    search_quality.clear();search_quality_keys.clear();ranking_initialized=false;unions_dirty=true;
    unseen_initialized=false;unseen_fair_index.clear();
}
bool service_full() const {return service_changes>=service_batch_jobs || (service_changes>=4&&now()-service_started>=service_quantum);}
std::pair<Pool::Ready,std::future<J>> prepare_launch(const J& submitted,const std::shared_ptr<TaskState>& state) {
    auto job=std::make_shared<J>(submitted);
    auto failed=std::make_shared<std::exception_ptr>();
    auto blocked_version=std::make_shared<std::optional<std::uint64_t>>();
    const auto kind=submitted.at("kind").get<std::string>();
    const bool needs_table=(kind=="exact"||kind=="plan"||kind=="pair"||kind=="corridor"||
        submitted.value("segment_graph",false)||submitted.value("unseen_mass",false)) && submitted.value("partition_target",0)<=1;
    auto admit=[this,job,state,failed,blocked_version,needs_table]() {
        try {
            if(stopping || (number(*job,"campaign_deadline")>0 && now()>=number(*job,"campaign_deadline")))return true;
            if(!needs_table)return true;
            const auto revision=ram.admission_version();
            if(*blocked_version && **blocked_version==revision)return false;
            std::uint64_t table=0;
            if(!ram.native(state,count(*job,"table_desired"),count(*job,"table_required"),count(*job,"table_cap"),table)) {
                *blocked_version=revision;state->queued_ram_wait=true;return false;
            }
            state->queued_ram_wait=false;(*job)["ram_pre_admitted"]=true;(*job)["table_bytes"]=table;return true;
        } catch(...) {*failed=std::current_exception();return true;}
    };
    std::function<J()> fn=[this,job,state,failed]() {
        try {if(*failed)std::rethrow_exception(*failed);return workers.execute(*job,state);}
        catch(...) {ram.release(state);state->set(4);throw;}
    };
    auto task=std::make_shared<std::packaged_task<J()>>(std::move(fn));auto future=task->get_future();
    Pool::Ready ready{[task]{(*task)();},std::move(admit),submitted.value("cpu_cores",1u),submitted.at("evaluation").get<bool>()};
    return {std::move(ready),std::move(future)};
}
