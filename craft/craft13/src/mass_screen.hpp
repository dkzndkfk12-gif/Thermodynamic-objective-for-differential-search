#pragma once
#include "replica.hpp"

namespace craft {

struct ScreenSamples {
    uint64_t paths=0,positive=0;
    double log_sum=-INFINITY,log_sum_squared=-INFINITY;
    void add(double log_weight){
        require(!std::isnan(log_weight)&&log_weight!=INFINITY,"invalid AIS weight");
        require(paths<UINT64_MAX,"AIS sample count overflow");
        ++paths;if(std::isfinite(log_weight))++positive;
        log_sum=logadd(log_sum,log_weight);log_sum_squared=logadd(log_sum_squared,2*log_weight);
    }
    void merge(const ScreenSamples& other){
        require(other.paths<=UINT64_MAX-paths,"AIS sample count overflow");
        paths+=other.paths;positive+=other.positive;
        log_sum=logadd(log_sum,other.log_sum);log_sum_squared=logadd(log_sum_squared,other.log_sum_squared);
    }
    double log_mean()const{return positive?log_sum-std::log(double(paths)):-INFINITY;}
    double ess()const{return positive?std::min(double(paths),std::exp(2*log_sum-log_sum_squared)):0;}
    J samples_json()const{return {{"paths",paths},{"positive_paths",positive},
        {"log_sum",positive?J(log_sum):J()},{"log_sum_squared",positive?J(log_sum_squared):J()}};}
    void restore_samples(const J& j){
        paths=j.at("paths");positive=j.at("positive_paths");
        require(positive<=paths,"invalid AIS sample receipt");
        log_sum=positive?j.at("log_sum").get<double>():-INFINITY;
        log_sum_squared=positive?j.at("log_sum_squared").get<double>():-INFINITY;
        require(!positive||(std::isfinite(log_sum)&&std::isfinite(log_sum_squared)),"invalid AIS sums");
    }
};
struct ScreenEvidence:ScreenSamples {
    double cpu=0;
    uint64_t attempts=0,pending_target=0;

    std::shared_ptr<const ScreenSamples> pending_batch;
    double priority(double certified_F)const{
        double value=certified_F;
        if(positive){
            double estimate=-log_mean()/std::log(2.0);
            double penalty=std::log2(std::max(1.,double(paths)/std::max(1.,ess())));

            value=std::min(certified_F,std::max(0.,estimate+penalty));
        }
        return value;
    }
    int queue_group()const{
        if(pending_batch)return 0;
        if(!attempts)return 1;

        return attempts<2&&ess()<8?2:-1;
    }
    void start_batch(uint64_t target){
        require(!pending_batch&&target>0&&attempts<UINT64_MAX,"invalid AIS batch start");
        ++attempts;pending_target=target;pending_batch=std::make_shared<ScreenSamples>();
    }
    uint64_t samples_left()const{
        require(bool(pending_batch)&&pending_batch->paths<pending_target,"invalid unfinished AIS batch");
        return pending_target-pending_batch->paths;
    }
    bool collect(const ScreenSamples& samples){
        require(bool(pending_batch)&&samples.paths<=samples_left(),"AIS batch overflow");
        auto next=std::make_shared<ScreenSamples>(*pending_batch);next->merge(samples);
        if(next->paths==pending_target){merge(*next);pending_batch.reset();pending_target=0;return true;}
        pending_batch=std::move(next);return false;
    }
    J json()const{
        J j=samples_json();j["estimated_negative_log2"]=positive?J(-log_mean()/std::log(2.0)):J();
        j["effective_sample_size"]=ess();j["cpu_seconds"]=cpu;j["certified"]=false;
        j["screen_attempts"]=attempts;
        if(pending_batch){j["pending_batch"]=pending_batch->samples_json();j["pending_batch_target"]=pending_target;}
        return j;
    }
    void restore(const J& j){
        restore_samples(j);cpu=j.at("cpu_seconds");require(std::isfinite(cpu)&&cpu>=0,"invalid AIS CPU receipt");
        attempts=j.value("screen_attempts",paths?uint64_t(1):uint64_t(0));
        pending_batch.reset();pending_target=0;
        if(j.contains("pending_batch")){
            auto batch=std::make_shared<ScreenSamples>();batch->restore_samples(j.at("pending_batch"));
            pending_target=j.at("pending_batch_target");
            require(attempts>0&&pending_target>batch->paths,"invalid restored AIS batch");
            pending_batch=std::move(batch);
        }
    }
};
using ScreenQueue=std::set<std::tuple<int,double,size_t>>;

class MassScreen {
    const Model& model;
    Domains domains;
    Values state;
    std::vector<int> variables;
    std::shared_ptr<const std::vector<ThermalLevel>> levels;
    static std::shared_ptr<const std::vector<ThermalLevel>> shared_levels(unsigned bridges){
        static std::mutex mutex;
        static std::map<unsigned,std::shared_ptr<const std::vector<ThermalLevel>>> cache;
        std::lock_guard<std::mutex> guard(mutex);
        auto found=cache.find(bridges);
        if(found!=cache.end())return found->second;
        auto result=std::make_shared<std::vector<ThermalLevel>>();
        for(unsigned k=0;k<bridges;++k)result->emplace_back(std::exp2(-16.0*k/bridges));
        result->emplace_back(0.0);
        cache.emplace(bridges,result);
        return result;
    }
    size_t stage=0,updates_left=0;bool ratio_pending=true;
    double log_base=0,log_weight=0;
    double density(const ThermalLevel& level)const{
        double result=0;for(const auto& f:model.factors)result+=level.log_factor[f.count(state)];return result;
    }
    void transition(RNG& rng){
        if(variables.empty())return;
        int pivot=variables[size_t(draw(rng,int(variables.size())))];
        std::vector<int> block{pivot};
        if(!model.incident[pivot].empty()&&draw(rng,4)==0){
            const auto& factor=model.factors[model.incident[pivot][size_t(draw(rng,int(model.incident[pivot].size())))]];
            std::vector<int> others;for(int v:factor.scope)if(v!=pivot&&v>=model.ne&&pop(domains[v])>1)others.push_back(v);
            std::shuffle(others.begin(),others.end(),rng);
            for(size_t i=0;i<std::min<size_t>(2,others.size());++i)block.push_back(others[i]);
        }
        auto factors=touched_factors(model,block);
        std::vector<std::vector<int>> allowed;size_t n=1;
        for(int v:block){allowed.push_back(domain_values(domains[v]));n*=allowed.back().size();}
        auto original=state;std::vector<double> weights(n,0);
        for(size_t code=0;code<n;++code){size_t z=code;
            for(size_t k=0;k<block.size();++k){state[block[k]]=uint8_t(allowed[k][z%allowed[k].size()]);z/=allowed[k].size();}
            for(int f:factors)weights[code]+=levels->at(stage).log_factor[model.factors[f].count(state)];
        }
        size_t chosen=size_t(draw_log(weights,rng));state=std::move(original);
        for(size_t k=0;k<block.size();++k){state[block[k]]=uint8_t(allowed[k][chosen%allowed[k].size()]);chosen/=allowed[k].size();}
    }
public:
    MassScreen(const Model& m,const Values& witness,unsigned bridges=64):model(m),domains(m.domains),state(witness),levels(shared_levels(bridges)){
        require(!m.physical&&m.legal(witness)&&bridges>=2,"invalid AIS family");
        for(int v=0;v<m.ne;++v)domains[v]=uint16_t(1u<<witness[v]);
        require(propagate(m,domains,m.all_factors()),"AIS rejected certified witness");
        log_base=-double(m.factors.size())*std::log(16.0);
        for(int v=m.ne;v<m.nv;++v){log_base+=std::log(double(pop(domains[v])));if(pop(domains[v])>1)variables.push_back(v);}
    }
    bool path_in_progress()const{return stage!=0;}

    bool step(RNG& rng,double& completed_log_weight){
        if(stage==0){for(int v:variables){auto values=domain_values(domains[v]);state[v]=uint8_t(values[size_t(draw(rng,int(values.size())))]);}
            log_weight=log_base;stage=1;updates_left=0;ratio_pending=true;}
        if(updates_left){transition(rng);if(--updates_left==0){++stage;ratio_pending=true;}return false;}
        require(ratio_pending,"AIS bridge state lost");
        log_weight+=density(levels->at(stage))-density(levels->at(stage-1));
        if(stage+1==levels->size()) {completed_log_weight=log_weight;stage=0;return true;}

        updates_left=std::max<size_t>(1,variables.size());ratio_pending=false;
        return false;
    }
};
}
