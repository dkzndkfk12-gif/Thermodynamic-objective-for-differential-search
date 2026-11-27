#pragma once
#include "contraction.hpp"

namespace craft {
struct ComponentCounter {
    std::vector<int> variables,factors;
    std::vector<mpz_class> histogram;
    Values seed;
    size_t active=0;
    uint64_t nodes=0,last_receipt=0;
    ResumableSum sum;
    ComponentCounter(const Model& m,const Domains& root,std::vector<int> vs,std::vector<int> fs,const Values& witness)
        :variables(std::move(vs)),factors(std::move(fs)),seed(witness),sum(m,root,factors) {
        int weak=0;for(int id:factors){const auto& f=m.factors[id];int n=f.count(seed);require(f.weighted?(n==2||n==4):n==16,"invalid fixed-pattern witness factor");active+=f.weighted;weak+=f.weighted&&n==2;}histogram.resize(active+1);histogram[weak]=1;
    }
    bool done()const{return sum.done();}
    mpz_class numerator()const{mpz_class n=0;for(size_t k=0;k<histogram.size();++k)n+=histogram[k]<<(active-k);return n;}
    mpz_class paths()const{mpz_class n=0;for(const auto& z:histogram)n+=z;return n;}
    uint64_t remaining_work()const{return sum.remaining_work();}
    void step(const Model& m,uint64_t sibling_bytes=0){
        (void)m;if(done())return;sum.step(sibling_bytes);nodes=sum.nodes();
        if(last_receipt==sum.completed_regions()&&!done())return;
        last_receipt=sum.completed_regions();
        const unsigned bits=sum.coefficient_bits();mpz_class remaining=sum.packed(),mask=(mpz_class(1)<<bits)-1,new_num=0;
        std::vector<mpz_class> h(active+1);
        for(size_t strong=0;strong<=active;++strong){h[active-strong]=remaining&mask;remaining>>=bits;new_num+=h[active-strong]<<strong;}
        require(remaining==0,"packed path histogram exceeds active count");
        auto old=numerator();if(done())require(new_num>=old,"completed contraction lost certified mass");

        if(new_num>=old)histogram=std::move(h);
    }
    J json()const{J hist=J::array();for(const auto& z:histogram)hist.push_back(z.get_str());return {{"engine","resumable-variable-elimination-v2"},{"variables",variables},{"factors",factors},{"histogram",hist},{"nodes",nodes},{"sum",sum.json()}};}
    void restore(const Model& m,const Domains& root,const J& j){
        (void)m;require(j.at("variables").get<std::vector<int>>()==variables&&j.at("factors").get<std::vector<int>>()==factors,"checkpoint component topology changed");
        const auto& h=j.at("histogram");require(h.size()==histogram.size(),"bad histogram dimension");
        for(size_t k=0;k<h.size();++k){histogram[k]=mpz_class(h[k].get<std::string>());require(histogram[k]>=0,"negative histogram");}require(paths()>0,"checkpoint lost witness mass");
        if(j.contains("sum")){require(j.at("engine")=="resumable-variable-elimination-v2","unknown contraction engine");sum.restore(j.at("sum"),root);last_receipt=sum.completed_regions();nodes=sum.nodes();}

    }
};

class FamilyCounter {
    std::shared_ptr<const Model> m;
    Values witness;
    Domains root;
    int constant_active=0,constant_weak=0;
    size_t cursor=0;
public:
    std::vector<ComponentCounter> components;
    explicit FamilyCounter(std::shared_ptr<const Model> model,Values seed):m(std::move(model)),witness(std::move(seed)),root(m->domains) {
        require(!m->physical && m->legal(witness),"counter needs a legal fixed-pattern witness");
        for(int v=0;v<m->ne;++v)root[v]=uint16_t(1u<<witness[v]);
        require(propagate(*m,root,m->all_factors()),"witness rejected by propagation");
        for(int v=0;v<m->nv;++v)require(root[v]&(1u<<witness[v]),"propagation removed witness");
        DSU d(m->nv);
        for(const auto& f:m->factors) { int prev=-1;for(int v:f.scope)if(pop(root[v])>1){if(prev>=0)d.join(prev,v);prev=v;} }
        std::map<int,std::vector<int>> vs,fs;
        for(int v=m->ne;v<m->nv;++v)if(pop(root[v])>1)vs[d.root(v)].push_back(v);
        for(size_t i=0;i<m->factors.size();++i) {
            int free=-1;for(int v:m->factors[i].scope)if(pop(root[v])>1){free=v;break;}
            if(free<0){constant_active+=m->factors[i].weighted;constant_weak+=m->factors[i].weighted&&m->factors[i].count(witness)==2;}else fs[d.root(free)].push_back(int(i));
        }
        for(auto& kv:vs)components.emplace_back(*m,root,std::move(kv.second),std::move(fs[kv.first]),witness);
    }
    bool done() const { return std::all_of(components.begin(),components.end(),[](const auto& c){return c.done();}); }
    void step() {
        if(components.empty())return;
        for(size_t i=0;i<components.size();++i){
            size_t j=cursor++%components.size();
            if(components[j].done())continue;
            uint64_t siblings=0;
            for(size_t k=0;k<components.size();++k)if(k!=j)siblings=saturated_add(siblings,components[k].sum.resident_bytes());
            components[j].step(*m,siblings);
            return;
        }
    }
    uint64_t resident_bytes()const{
        uint64_t bytes=0;
        for(const auto& component:components)bytes=saturated_add(bytes,component.sum.resident_bytes());
        return bytes;
    }
    void compact(){for(auto& component:components)component.sum.compact();}
    Dyadic mass() const { mpz_class n=mpz_class(1)<<(constant_active-constant_weak);for(const auto& c:components)n*=c.numerator();return {n,unsigned(3*m->active)}; }
    mpz_class paths() const { mpz_class n=1;for(const auto& c:components)n*=c.paths();return n; }
    uint64_t nodes() const { uint64_t n=0;for(const auto& c:components)n+=c.nodes;return n; }
    uint64_t remaining_work() const {uint64_t n=0;for(const auto& c:components)if(!c.done())n=saturated_add(n,std::max<uint64_t>(1,c.remaining_work()));return n;}
    J weight_histogram() const {
        std::vector<mpz_class> h(constant_weak+1);h[constant_weak]=1;
        for(const auto& c:components){std::vector<mpz_class> q(h.size()+c.histogram.size()-1);for(size_t i=0;i<h.size();++i)for(size_t j=0;j<c.histogram.size();++j)q[i+j]+=h[i]*c.histogram[j];h=std::move(q);}
        J a=J::array();for(size_t k=0;k<h.size();++k)if(h[k]!=0)a.push_back({{"weight",2*m->active+k},{"ddt2",k},{"paths",h[k].get_str()}});return a;
    }
    J json() const { J a=J::array();for(const auto& c:components)a.push_back(c.json());return {{"components",a},{"cursor",cursor},{"constant_active",constant_active},{"constant_weak",constant_weak}}; }
    J completed_record()const{
        require(done(),"incomplete family cannot produce a completed receipt");
        J records=J::array();
        for(const auto& component:components){
            J bins=J::array();
            for(size_t k=0;k<component.histogram.size();++k)
                if(component.histogram[k]!=0)bins.push_back({k,component.histogram[k].get_str()});
            records.push_back({{"histogram",std::move(bins)},{"active",component.active},{"nodes",component.nodes}});
        }
        return {{"terminal",true},{"components",std::move(records)},{"cursor",cursor},
                {"constant_active",constant_active},{"constant_weak",constant_weak}};
    }
    static J completed_statistics(const J& record){
        std::vector<mpz_class> histogram(record.at("constant_weak").get<size_t>()+1);
        histogram.back()=1;
        size_t active=record.at("constant_active").get<size_t>();
        for(const auto& component:record.at("components")){
            std::vector<mpz_class> bins;
            if(record.value("terminal",false)){
                bins.resize(component.at("active").get<size_t>()+1);
                for(const auto& bin:component.at("histogram"))bins.at(bin.at(0).get<size_t>())=mpz_class(bin.at(1).get<std::string>());
            }else for(const auto& bin:component.at("histogram"))bins.emplace_back(bin.get<std::string>());
            active+=bins.size()-1;
            std::vector<mpz_class> product(histogram.size()+bins.size()-1);
            for(size_t k=0;k<histogram.size();++k)for(size_t j=0;j<bins.size();++j)product[k+j]+=histogram[k]*bins[j];
            histogram=std::move(product);
        }
        Dyadic mass;mpz_class paths=0;J bins=J::array();
        for(size_t k=0;k<histogram.size();++k)if(histogram[k]!=0){
            paths+=histogram[k];mass+=Dyadic(histogram[k],unsigned(2*active+k));
            bins.push_back({{"weight",2*active+k},{"ddt2",k},{"paths",histogram[k].get_str()}});
        }
        return {{"enumerated_lower_bound",mass.json()},{"covered_paths",paths.get_str()},{"weight_histogram",std::move(bins)}};
    }
    void restore(const J& j) {
        require(j.at("constant_active")==constant_active&&j.at("constant_weak")==constant_weak&&j.at("components").size()==components.size(),"counter checkpoint mismatch");
        for(size_t i=0;i<components.size();++i){
            if(!j.value("terminal",false)){components[i].restore(*m,root,j.at("components")[i]);continue;}
            auto& component=components[i];const auto& saved=j.at("components")[i];
            require(saved.at("active")==component.active,"completed component active count changed");
            std::fill(component.histogram.begin(),component.histogram.end(),0);
            for(const auto& bin:saved.at("histogram")){
                auto index=bin.at(0).get<size_t>();
                require(index<component.histogram.size()&&component.histogram[index]==0,"invalid completed histogram index");
                component.histogram[index]=mpz_class(bin.at(1).get<std::string>());
                require(component.histogram[index]>0,"invalid completed path count");
            }
            require(component.paths()>0,"completed receipt lost every path");
            component.nodes=saved.at("nodes").get<uint64_t>();
            component.sum.completed_histogram(component.histogram,component.nodes);
            component.last_receipt=component.sum.completed_regions();
        }
        cursor=j.at("cursor").get<size_t>();
    }
};
}
