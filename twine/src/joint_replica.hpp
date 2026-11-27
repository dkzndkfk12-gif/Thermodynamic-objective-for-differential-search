#pragma once
#include "replica.hpp"

namespace twine {

class JointReplica {
    struct Lane{Copies copies;std::vector<size_t> labels;};
    std::shared_ptr<const Model> model;
    std::vector<Domains> patterns;
    std::vector<ThermalLevel> levels;
    std::vector<Lane> lanes;
    double activity_log(const Values& x,size_t label,const ThermalLevel& level)const{
        double log=0;for(size_t v=0;v<x.size();++v)if(!(patterns[label][v]&(1u<<x[v]))){
            if(level.alpha==0)return -INFINITY;
            log+=std::log(level.alpha);
        }
        return log;
    }
    double density(const Lane& lane,const ThermalLevel& level)const{
        double log=log_density(*model,lane.copies,level);
        for(size_t j=0;j<lane.copies.size();++j)log+=activity_log(lane.copies[j],lane.labels[j],level);
        return log;
    }
public:
    uint64_t steps=0,pattern_changes=0,mixed_cold_visits=0,endpoint_changes=0;
    JointReplica(std::shared_ptr<const Model> physical,const std::vector<Trail>& starts,int copies,RNG& rng):model(std::move(physical)){
        require(model->physical&&starts.size()>=2&&copies>=2,"joint search needs a witnessed union");
        std::set<std::string> unique;
        for(const auto& t:starts){
            require(t.front()==starts.front().front()&&t.back()==starts.front().back(),"joint starts have different endpoints");
            require(unique.insert(pattern_key(activity(t))).second,"duplicate joint pattern");
            auto x=model->encode(t);require(model->legal(x),"illegal joint witness");
            Domains support;for(auto value:x)support.push_back(value?0xfffe:1);patterns.push_back(std::move(support));
        }
        for(double alpha:{1.0,0.25,0.04,0.004,0.0001,0.0})levels.emplace_back(alpha);
        for(size_t k=0;k<levels.size();++k){Lane lane;
            for(int j=0;j<copies;++j){size_t label=size_t(draw(rng,int(starts.size())));lane.labels.push_back(label);lane.copies.push_back(model->encode(starts[label]));}
            lanes.push_back(std::move(lane));
        }
    }
    void step(RNG& rng){
        for(size_t k=0;k<lanes.size();++k){auto& lane=lanes[k];const auto& level=levels[k];
            std::vector<Domains> supports;for(auto p:lane.labels)supports.push_back(patterns[p]);
            internal_update(*model,lane.copies,level,rng,&supports);
            endpoint_changes+=collapsed_endpoint(*model,lane.copies,level,rng,&supports);
            for(size_t j=0;j<lane.copies.size();++j){std::vector<double> weights;
                for(size_t p=0;p<patterns.size();++p)weights.push_back(activity_log(lane.copies[j],p,level));
                size_t selected=size_t(draw_log(weights,rng));pattern_changes+=selected!=lane.labels[j];lane.labels[j]=selected;
            }
        }
        for(size_t k=steps%2;k+1<lanes.size();k+=2){
            double ratio=density(lanes[k],levels[k+1])+density(lanes[k+1],levels[k])
                        -density(lanes[k],levels[k])-density(lanes[k+1],levels[k+1]);
            if(ratio>=0||std::log(unit(rng))<ratio)std::swap(lanes[k],lanes[k+1]);
        }
        const auto& labels=lanes.back().labels;
        mixed_cold_visits+=std::any_of(labels.begin(),labels.end(),[&](size_t p){return p!=labels[0];});++steps;
    }
    const Copies& cold()const{return lanes.back().copies;}
    double cold_density()const{return density(lanes.back(),levels.back());}
};
}
