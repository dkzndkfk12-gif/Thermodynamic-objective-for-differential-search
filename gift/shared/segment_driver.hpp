#pragma once
#include "segment_graph.hpp"
#include "mass_conditions.hpp"
#include "graph_certificate.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <csignal>
#include <memory>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace segment_mass {
using J=nlohmann::json;
namespace fs=std::filesystem;
inline volatile std::sig_atomic_t stop_signal=0;
inline void signal_stop(int){stop_signal=1;}
struct Hit {std::vector<W> path;double first,F,weight;};
struct Engine {
    Spec spec;unsigned in,cap,m,keep;std::uint64_t memory,work;
    std::mt19937_64 rng;double started=clock(),until;std::string output,cache_path;
    std::unique_ptr<Graph> g;std::map<std::string,Hit> hits;J diag=J::object();
    std::unique_ptr<MassConditions> chains;
    std::vector<Z> priority;std::vector<unsigned> depth;int current_a=-1,current_b=-1;Z previous=0;
    Engine(Spec s,unsigned i,unsigned k,unsigned copies,unsigned limit,std::uint64_t mem,std::uint64_t rowwork,
           std::uint64_t seed,double seconds,std::string out,std::string cache):spec(std::move(s)),in(i),cap(k),m(copies),keep(limit),memory(mem),work(rowwork),rng(seed),until(started+seconds),output(std::move(out)),cache_path(std::move(cache)){
        if(!in||!cap||in>spec.N||cap>spec.N||!m||m>8||!keep||!work||memory<1024*1024||seconds<=0)throw std::runtime_error("invalid segment job options");
        g=std::make_unique<Graph>(spec,in,cap,memory);
        diag["reservation_revision"]=2;
    }
    void tick(){if(stop_signal||clock()>=until)throw Stop();}
    void inc(const char* k,std::uint64_t n=1){diag[k]=diag.value(k,std::uint64_t(0))+n;}
    void enable_conditions(unsigned levels,unsigned bits,double strength){
        if(in!=spec.N||cap!=spec.N)throw std::runtime_error("mass conditions must use the full physical cell range");
        chains=std::make_unique<MassConditions>(spec,m,levels,bits,strength,rng());
    }
    void restore_conditions(){
        if(!chains||hits.empty())return;
        std::vector<std::vector<W>> seeds;for(const auto& kv:hits)seeds.push_back(kv.second.path);
        chains->initialize(seeds);
        if(!cache_path.empty()&&fs::exists(cache_path+".lanes.json")){

            try{std::ifstream f(cache_path+".lanes.json");J j;f>>j;
                diag["lanes_restored"]=chains->restore(j);
            }catch(const std::exception& e){diag["lane_cache_rejected"]=e.what();}
        }
    }
    void conditions_batch(){
        for(unsigned i=0;i<2*chains->levels;++i){tick();chains->step([&]{tick();});}

        for(const auto& lane:chains->lanes)for(const auto& p:lane.paths){tick();
            Z z=1;for(unsigned r=0;r<spec.R;++r)z*=spec.coefficient(p[r],p[r+1]);
            if(z<=0)throw std::runtime_error("mass sampler published illegal path");
            record(p,z);path(p);
        }
    }
    void path(const std::vector<W>& p){for(unsigned r=0;r+1<p.size();++r)g->add(p[r],p[r+1]);}
    void load(const J& seeds){for(const auto& row:seeds){tick();if(row.size()!=spec.R+1)throw std::runtime_error("segment initial trail length");std::vector<W> p;for(const auto& x:row)p.push_back(word(x.get<std::string>(),spec.N));
        if(active(p[0])>in)throw std::runtime_error("segment seed input outside domain");
        for(unsigned r=1;r<spec.R;++r)if(active(p[r])>cap)throw std::runtime_error("segment seed interior outside domain");
        Z z=1;for(unsigned r=0;r<spec.R;++r){Z c=spec.coefficient(p[r],p[r+1]);if(c<=0)throw std::runtime_error("illegal segment seed");z*=c;}

        path(p);record(p,z);inc("seeded_paths");}}
    static void putw(std::ostream& f,W w){std::uint64_t v[2]={std::uint64_t(w),std::uint64_t(w>>64)};for(auto a:v)for(unsigned j=0;j<8;++j)f.put(char(a>>(8*j)));}
    static bool getw(std::istream& f,W& w){w=0;for(unsigned i=0;i<16;++i){int c=f.get();if(c==EOF)return false;w|=W(unsigned(c))<<(8*i);}return true;}
    void restore(){
        if(cache_path.empty()||!fs::exists(cache_path))return;
        std::ifstream f(cache_path,std::ios::binary);std::string header;std::getline(f,header);
        if(header!=spec.identity()){inc("cache_identity_misses");return;}

        W u,v;try{while(getw(f,u)&&getw(f,v)){tick();if(u&&v){g->add(u,v);inc("restored_edges");}if(g->used+4*g->message_bytes()>memory/2)break;}}
        catch(const ResourceStop&){inc("cache_resource_truncations");}
        catch(const std::exception& e){diag["cache_read_error"]=e.what();}
    }
    void snapshot(){
        if(cache_path.empty())return;
        fs::create_directories(fs::path(cache_path).parent_path());auto tmp=cache_path+".tmp";
        std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f<<spec.identity()<<'\n';

        std::set<std::pair<W,W>> chosen;
        for(const auto& kv:hits)for(unsigned r=0;r<spec.R;++r)chosen.emplace(kv.second.path[r],kv.second.path[r+1]);
        std::vector<unsigned> order(g->edges.size());std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
        const auto limit=std::max<std::uint64_t>(chosen.size(),memory/(2*(832+4*(spec.R+1)*(48+(4*spec.N*spec.R+63)/64*8))));
        unsigned checks=0;const auto publication_deadline=until+(until-started)/19;
        for(unsigned i:order){if(chosen.size()>=limit)break;
            if((++checks&255)==0&&(stop_signal||clock()>publication_deadline)){f.close();fs::remove(tmp);inc("cache_checkpoint_deferred");return;}
            const auto&e=g->edges[i];chosen.emplace(g->nodes[e.u].w,g->nodes[e.v].w);
        }
        for(const auto& e:chosen){
            if((++checks&255)==0&&(stop_signal||clock()>publication_deadline)){f.close();fs::remove(tmp);inc("cache_checkpoint_deferred");return;}
            putw(f,e.first);putw(f,e.second);
        }f.flush();if(!f)throw std::runtime_error("segment cache write failed");f.close();fs::rename(tmp,cache_path);diag["cached_edges"]=chosen.size();
    }
    W anchor(){W x=0;unsigned n=1+unsigned(rng()%std::max(in,cap));std::vector<unsigned> pos(spec.N);std::iota(pos.begin(),pos.end(),0);std::shuffle(pos.begin(),pos.end(),rng);for(unsigned j=0;j<n;++j)x|=W(1+rng()%15)<<(4*pos[j]);return x;}
    Row row(W w,unsigned mode){
        bool inverse=mode==1;unsigned k=mode==2?spec.N:std::max(in,cap);
        auto key=spec.identity()+"/"+std::to_string(mode)+"/"+std::to_string(k)+"/"+hex(w,spec.N);
        std::string value;
        if(mass_messages::cache().get(key,value)){
            Row a;a.complete=true;std::istringstream f(value);std::string s;
            while(f>>s)a.neighbors.push_back(word(s,spec.N));
            inc("row_cache_hits");return a;
        }
        Row a=enumerate(spec,w,inverse,k,work,rng,[&]{tick();});inc("row_work",a.work);inc(a.complete?"complete_rows":"partial_rows");
        if(a.complete){std::ostringstream f;for(W v:a.neighbors)f<<hex(v,spec.N)<<'\n';mass_messages::cache().put(key,f.str());}
        return a;
    }
    void expand(){
        tick();auto& graph=*g;

        if(graph.nodes.empty()||rng()%std::max<std::size_t>(2,graph.nodes.size())==0)graph.node(anchor());
        unsigned mode=unsigned(rng()%3),chosen=0;bool have=false;std::vector<Z> scores;
        std::vector<unsigned> available;
        for(unsigned i=0;i<graph.nodes.size();++i){
            if(graph.nodes[i].complete[mode])continue;

            if(mode!=1&&active(graph.nodes[i].w)>std::max(in,cap))continue;
            if(mode==2&&(i>=depth.size()||depth[i]<spec.R-1))continue;
            available.push_back(i);
        }
        if(available.empty()){graph.node(anchor());return;}
        if((rng()&1)&&!priority.empty()){
            for(unsigned i:available)scores.push_back(i<priority.size()?Z(priority[i]/Z(1+graph.nodes[i].work[mode]/work)):Z(0));
            Z sum=0;for(const auto& z:scores)sum+=z;
            if(sum>0){chosen=available[draw(scores,rng)];have=true;}
        }
        if(!have){

            chosen=available[rng()%available.size()];
            if((rng()&1)&&!depth.empty()){
                unsigned other=available[rng()%available.size()];
                if(other<depth.size()&&chosen<depth.size()&&depth[other]>depth[chosen])chosen=other;
            }
        }
        W w=graph.nodes[chosen].w;Row a=row(w,mode);
        graph.nodes[chosen].work[mode]+=std::max<std::uint64_t>(1,a.work);
        for(W v:a.neighbors){tick();if(!v)continue;if(mode==1){if(graph.add(v,w))inc("new_edges");}else if(graph.add(w,v))inc("new_edges");}

        graph.nodes[chosen].complete[mode]=a.complete;inc("expanded_rows");
    }

    void connect(){
        tick();if(hits.empty())return;
        auto it=hits.begin();std::advance(it,rng()%hits.size());const auto p=it->second.path;
        const unsigned mode=unsigned(diag.value("connection_attempts",std::uint64_t(0))%3);inc("connection_attempts");
        auto admit=[&](std::vector<W> q){Z z=1;for(unsigned r=0;r<spec.R;++r)z*=spec.coefficient(q[r],q[r+1]);
            if(z<=0)throw std::runtime_error("segment connection validation");
            path(q);record(q,z);inc("legal_connections");};
        if(mode<2){

            const bool reverse=mode==0;auto a=enumerate(spec,p[reverse?1:spec.R-1],reverse,reverse?in:spec.N,work,rng,[&]{tick();});
            inc("connection_work",a.work);
            for(W v:a.neighbors){tick();if(!v)continue;auto q=p;q[reverse?0:spec.R]=v;admit(std::move(q));}
            return;
        }
        if(spec.R<2)return;
        const unsigned length=std::min(3u,spec.R),start=unsigned(rng()%(spec.R-length+1));
        std::uint64_t remaining=work;

        auto first=enumerate(spec,p[start],false,cap,std::max<std::uint64_t>(1,remaining/2),rng,[&]{tick();});
        remaining-=std::min(remaining,first.work);inc("connection_work",first.work);
        for(W v:first.neighbors){tick();
            if(length==2){if(v&&spec.coefficient(v,p[start+2])>0){auto q=p;q[start+1]=v;admit(std::move(q));}continue;}
            if(!remaining)break;
            auto second=enumerate(spec,v,false,cap,remaining,rng,[&]{tick();});
            remaining-=std::min(remaining,second.work);inc("connection_work",second.work);
            for(W w:second.neighbors){tick();if(!w||spec.coefficient(w,p[start+3])<=0)continue;
                auto q=p;q[start+1]=v;q[start+2]=w;admit(std::move(q));}
        }
    }
    void record(const std::vector<W>& p,const Z& total){
        std::string key=hex(p.front(),spec.N)+"/"+hex(p.back(),spec.N)+"/";
        for(unsigned r=0;r<spec.R;++r){for(unsigned i=0;i<spec.N;++i)key+=((p[r]>>(4*i))&15)?'1':'0';key+=',';}
        Z z=1;for(unsigned r=0;r<spec.R;++r)z*=spec.coefficient(p[r],p[r+1]);
        double F=double(4*spec.N*spec.R)-log2z(total),weight=double(4*spec.N*spec.R)-log2z(z);
        auto it=hits.find(key);if(it==hits.end())hits.emplace(key,Hit{p,clock()-started,F,weight});
        else {it->second.F=std::min(F,it->second.F);if(weight<it->second.weight){it->second.path=p;it->second.weight=weight;}}
        if(hits.size()>keep){auto worst=std::max_element(hits.begin(),hits.end(),[](const auto& a,const auto& b){return std::tie(a.second.F,a.second.weight,a.first)<std::tie(b.second.F,b.second.weight,b.first);});hits.erase(worst);}
    }
    void collapse(){
        tick();auto& graph=*g;if(graph.edges.empty())return;
        auto reach=graph.messages(false,-1,[&]{tick();});depth.assign(graph.nodes.size(),0);
        for(unsigned r=0;r<=spec.R;++r)for(unsigned i=0;i<graph.nodes.size();++i)if(reach[r][i]>0)depth[i]=r;
        Z any=0;for(const auto& z:reach[spec.R])any+=z;
        if(any==0){inc("unfinished_graphs");return;}
        std::vector<Z> proposal=std::move(reach[spec.R]);
        unsigned trial=draw(proposal,rng);
        unsigned b=current_b>=0&&proposal.at(current_b)>0?unsigned(current_b):trial;
        reach.clear();reach.shrink_to_fit();
        auto back=graph.messages(true,int(b),[&]{tick();});

        if(trial!=b){
            auto candidate=graph.messages(true,int(trial),[&]{tick();});Z old_target=0,new_target=0;
            for(const auto& z:back[0])old_target+=power(z,m);
            for(const auto& z:candidate[0])new_target+=power(z,m);
            Z numerator=new_target*proposal[b],denominator=old_target*proposal[trial];
            inc("component_move_attempts");
            if(numerator>=denominator||below(denominator,rng)<numerator){b=trial;back=std::move(candidate);inc("component_moves");}
        }
        proposal.clear();proposal.shrink_to_fit();

        unsigned a=draw(back[0],rng,m);back.clear();back.shrink_to_fit();
        auto front=graph.messages(false,int(a),[&]{tick();});b=draw(front[spec.R],rng,m);
        back=graph.messages(true,int(b),[&]{tick();});Z total=back[0][a];
        if(total!=front[spec.R][b]||total<=0)throw std::runtime_error("segment forward/backward mass mismatch");

        if(current_a>=0&&current_b>=0){
            Z old_end_mass;
            if(unsigned(current_a)==a)old_end_mass=front[spec.R][current_b];
            else {auto old=graph.messages(false,current_a,[&]{tick();});old_end_mass=old[spec.R][current_b];}
            if(old_end_mass<previous)throw std::runtime_error("segment graph mass decreased without compaction");
            if(old_end_mass>previous){
                inc("positive_union_gains");Z gain=old_end_mass-previous;
                double f=double(4*spec.N*spec.R)-log2z(gain);
                diag["best_gain_F"]=std::min(diag.value("best_gain_F",double(INFINITY)),f);
                diag["last_gain"]={{"numerator",gain.get_str()},{"denominator_power",4*spec.N*spec.R},
                    {"input",hex(graph.nodes[current_a].w,spec.N)},{"output",hex(graph.nodes[current_b].w,spec.N)}};
            }
        }
        current_a=int(a);current_b=int(b);previous=total;
        priority.assign(graph.nodes.size(),0);
        for(unsigned r=0;r<=spec.R;++r)for(unsigned i=0;i<graph.nodes.size();++i)priority[i]+=front[r][i]*back[r][i];
        for(unsigned j=0;j<m;++j){tick();record(graph.sample(a,b,back,rng),total);}
        inc("collapsed_endpoint_updates");diag["best_graph_F"]=std::min(diag.value("best_graph_F",double(INFINITY)),double(4*spec.N*spec.R)-log2z(total));
    }
    void compact(){
        auto old=std::move(g);g=std::make_unique<Graph>(spec,in,cap,memory);

        std::vector<const Hit*> paths;for(const auto& kv:hits)paths.push_back(&kv.second);
        std::sort(paths.begin(),paths.end(),[](const Hit* a,const Hit* b){return a->F<b->F;});
        auto fits=[&](const std::vector<W>& p,std::uint64_t budget){
            std::set<W> added;std::set<std::pair<W,W>> edges;
            for(W w:p)if(!g->index.count(w))added.insert(w);
            for(unsigned r=0;r+1<p.size();++r){auto a=g->index.find(p[r]),b=g->index.find(p[r+1]);
                if(a==g->index.end()||b==g->index.end()||!g->pairs.count({a->second,b->second}))edges.emplace(p[r],p[r+1]);}
            const auto edge_bytes=192+((4*spec.N+1+GMP_NUMB_BITS-1)/GMP_NUMB_BITS)*sizeof(mp_limb_t);
            auto structure=reserve_add(g->used,reserve_add(reserve_mul(added.size(),320),reserve_mul(edges.size(),edge_bytes)));
            return structure<=memory/2 && reserve_add(structure,reserve_mul(g->message_bytes_for_nodes(g->nodes.size()+added.size()),4))<=budget;
        };
        unsigned retained=0;
        for(const Hit* h:paths)if(fits(h->path,retained?memory/4:memory/2)){path(h->path);++retained;}
        if(!paths.empty()&&!retained)throw std::runtime_error("segment lease cannot retain one complete path");
        std::vector<unsigned> order(old->edges.size());std::iota(order.begin(),order.end(),0);std::shuffle(order.begin(),order.end(),rng);
        unsigned checks=0;for(unsigned k:order){if((++checks&255)==0)tick();if(g->used+4*g->message_bytes()>memory/4)break;const auto&e=old->edges[k];
            const std::vector<W> edge{old->nodes[e.u].w,old->nodes[e.v].w};if(fits(edge,memory/4))g->add(edge[0],edge[1]);}
        current_a=current_b=-1;previous=0;priority.clear();depth.clear();inc("resource_compactions");
        diag["retained_complete_paths"]=retained;
    }
    void run(){
        std::uint64_t epoch=0;

        if(hits.empty()){

            if(stop_signal||clock()>=until){inc("bootstrap_interrupted");return;}
            throw std::runtime_error("segment search requires a complete legal seed");
        }
        restore_conditions();
        try {try{collapse();}catch(const ResourceStop&){compact();collapse();}
        for(;;){tick();try{
            if(chains)conditions_batch();else if(epoch%2==0)connect();else expand();
            diag["peak_accounted_bytes"]=std::max(diag.value("peak_accounted_bytes",std::uint64_t(0)),g->used+4*g->message_bytes());
            ++epoch;collapse();
        }catch(const ResourceStop&){

            inc("allocation_rejections");
            compact();
        }}}
        catch(const Stop&){}
        diag["nodes"]=g->nodes.size();diag["edges"]=g->edges.size();diag["copies"]=m;diag["scope"]="finite legal graph; discovery score only; not certified ledger";
        if(chains)diag["mass_conditions"]=chains->metrics;
    }
    void publish(){
        auto tmp=output+".tmp";std::ofstream f(tmp);f<<"states\ttrail_weight\tfirst_seconds\tgraph_F\n"<<std::setprecision(17);
        for(const auto& kv:hits){const auto& h=kv.second;for(unsigned i=0;i<h.path.size();++i){if(i)f<<',';f<<hex(h.path[i],spec.N);}f<<'\t'<<h.weight<<'\t'<<h.first<<'\t'<<h.F<<'\n';}
        f.flush();if(!f)throw std::runtime_error("segment result write failed");f.close();fs::rename(tmp,output);
        if(chains&&!cache_path.empty()){
            try{std::ofstream f(cache_path+".lanes.json.tmp");f<<chains->checkpoint().dump();f.flush();
                if(!f)throw std::runtime_error("lane checkpoint write");
                f.close();fs::rename(cache_path+".lanes.json.tmp",cache_path+".lanes.json");
            }catch(const std::exception& e){diag["lane_checkpoint_error"]=e.what();}
        }
        priority.clear();priority.shrink_to_fit();depth.clear();depth.shrink_to_fit();

        try{J paths=J::array();for(const auto& kv:hits){J p=J::array();for(W x:kv.second.path)p.push_back(hex(x,spec.N));paths.push_back(p);}

            auto cert=graph_certificate(spec,paths,memory/2,[&]{if(stop_signal||clock()>until+(until-started)/19)throw Stop();});
            std::ofstream f(output+".mass.json.tmp");f<<cert.dump();f.flush();if(!f)throw std::runtime_error("graph certificate write");f.close();fs::rename(output+".mass.json.tmp",output+".mass.json");
            diag["certificate_endpoints"]=cert.at("records").size();
        }catch(const Stop&){inc("certificate_deferred");}catch(const ResourceStop&){inc("certificate_resource_deferred");}

        try{snapshot();}catch(const std::exception& e){diag["cache_error"]=e.what();}
        g.reset();
        diag["seconds"]=clock()-started;diag["returned_families"]=hits.size();
        std::ofstream d(output+".graph.json.tmp");d<<diag.dump(2)<<'\n';d.flush();
        if(!d)throw std::runtime_error("segment diagnostics write failed");
        d.close();fs::rename(output+".graph.json.tmp",output+".graph.json");
    }
};
inline int main(int argc,char** argv){try{
    std::map<std::string,std::string> o;for(int i=1;i<argc;++i){std::string k=argv[i];if(i+1>=argc)throw std::runtime_error("segment option value missing");o[k]=argv[++i];}
    std::ifstream f(o.at("--job"));J j;f>>j;auto c=j.at("config");unsigned n=c.value("cells",16),r=c.at("rounds");
    if(!j.contains("initial_trails")||!j.at("initial_trails").is_array()||j.at("initial_trails").empty())throw std::runtime_error("segment job has no complete initial trail");
    std::vector<unsigned> p;if(c.contains("PERM"))p=c.at("PERM").get<std::vector<unsigned>>();else{p.resize(4*n);for(unsigned i=0;i<4*n;++i)p[i]=i==4*n-1?i:n*i%(4*n-1);}
    Spec s(n,r,c.at("SBOX").get<std::vector<unsigned>>(),p);
    int lock=-1;std::string cache=j.value("cache",std::string());if(!cache.empty()){fs::create_directories(fs::path(cache).parent_path());lock=::open((cache+".lock").c_str(),O_CREAT|O_RDWR,0600);if(lock<0||flock(lock,LOCK_EX|LOCK_NB))throw std::runtime_error("segment cache shard busy");}
    std::signal(SIGTERM,signal_stop);std::signal(SIGINT,signal_stop);

    const double seconds=std::stod(o.at("--seconds"));
    Engine e(s,j.at("input_active"),j.at("cap"),j.at("copies"),j.at("keep"),j.at("memory"),j.at("row_work"),j.at("seed"),std::max(.001,seconds*.95),o.at("--out"),cache);
    if(j.value("mass_conditions",false))e.enable_conditions(j.at("mass_bias_levels"),j.at("mass_block_bits"),j.at("mass_bias_strength"));
    try{e.load(j.value("initial_trails",J::array()));e.restore();}catch(const Stop&){}catch(const ResourceStop&){e.compact();}
    e.run();e.publish();if(lock>=0){flock(lock,LOCK_UN);::close(lock);}return 0;
}catch(const std::exception& e){std::cerr<<"segment: "<<e.what()<<'\n';return 2;}}
}
