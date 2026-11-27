
class Pool {
  public:
    struct Ready {
        std::function<void()> run;
        std::function<bool()> admit;
        unsigned cores=1;
        bool evaluation=false;
    };
  private:
    std::mutex mu;
    std::condition_variable cv, completed_cv;
    std::deque<Ready> discovery, evaluation;
    std::vector<std::thread> threads;
    unsigned capacity, eval_target, search_busy=0, eval_busy=0;
    std::uint64_t completed=0, started=0;
    double accounted=now(), idle_core_seconds=0;
    bool stopping_pool=false;
    void account() {
        const auto t=now();idle_core_seconds+=(t-accounted)*(capacity-search_busy-eval_busy);accounted=t;
    }
    bool take(std::deque<Ready>& q, Ready& out) {
        const unsigned free=capacity-search_busy-eval_busy;
        for(auto it=q.begin();it!=q.end();++it) {
            if(it->cores>free || (it->admit && !it->admit()))continue;
            out=std::move(*it);q.erase(it);return true;
        }
        return false;
    }
    void worker() {
        for(;;) {
            Ready task;
            {
                std::unique_lock<std::mutex> lock(mu);
                for(;;) {
                    if(stopping_pool && discovery.empty() && evaluation.empty())return;
                    const bool eval_first=capacity==1 ? started%3==2 :
                        std::uint64_t(eval_busy)*(capacity-eval_target)<=std::uint64_t(search_busy)*eval_target;
                    if((eval_first && (take(evaluation,task)||take(discovery,task))) ||
                       (!eval_first && (take(discovery,task)||take(evaluation,task))))break;

                    cv.wait_for(lock,std::chrono::milliseconds(50));
                }
                account();(task.evaluation?eval_busy:search_busy)+=task.cores;++started;
            }
            task.run();
            {
                std::lock_guard<std::mutex> lock(mu);account();
                (task.evaluation?eval_busy:search_busy)-=task.cores;++completed;
            }
            cv.notify_all();completed_cv.notify_one();
        }
    }
  public:
    explicit Pool(unsigned n):capacity(n),eval_target(std::max(1u,n/3)) {
        require(n>0,"Worker pool requires a worker");
        try {for(unsigned i=0;i<n;++i)threads.emplace_back([this]{worker();});}
        catch(...) {{std::lock_guard<std::mutex> lock(mu);stopping_pool=true;}cv.notify_all();for(auto& t:threads)t.join();throw;}
    }
    void enqueue_batch(std::vector<Ready> tasks) {
        if(tasks.empty())return;
        {std::lock_guard<std::mutex> lock(mu);require(!stopping_pool,"Worker pool stopped");
         for(auto& t:tasks){require(t.cores>0&&t.cores<=capacity,"Task exceeds CPU budget");
             (t.evaluation?evaluation:discovery).push_back(std::move(t));}}
        cv.notify_all();
    }
    std::future<J> submit(std::function<J()> fn) {
        auto task=std::make_shared<std::packaged_task<J()>>(std::move(fn));auto future=task->get_future();
        enqueue_batch({Ready{[task]{(*task)();},{},1,false}});return future;
    }
    std::uint64_t revision(){std::lock_guard<std::mutex> lock(mu);return completed;}
    void wait(std::uint64_t old) {
        std::unique_lock<std::mutex> lock(mu);
        completed_cv.wait_for(lock,std::chrono::milliseconds(50),[&]{return completed!=old||stopping_pool;});
    }
    J status() {
        std::lock_guard<std::mutex> lock(mu);account();unsigned sq=0,eq=0;
        for(const auto& t:discovery)sq+=t.cores;
        for(const auto& t:evaluation)eq+=t.cores;
        return {{"capacity",capacity},{"search_busy_cores",search_busy},{"evaluation_busy_cores",eval_busy},
                {"busy_cores",search_busy+eval_busy},{"idle_cores",capacity-search_busy-eval_busy},
                {"ready_search_cores",sq},{"ready_evaluation_cores",eq},{"completed",completed},
                {"idle_core_seconds",idle_core_seconds},{"busy_is_measured_CPU",false}};
    }
    ~Pool() {

        {std::lock_guard<std::mutex> lock(mu);stopping_pool=true;discovery.clear();evaluation.clear();}cv.notify_all();
        for(auto& t:threads)t.join();
    }
};
