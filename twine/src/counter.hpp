#pragma once
#include "model.hpp"

namespace twine {
struct ComponentCounter {
    std::vector<int> variables,factors;
    std::vector<SearchFrame> stack;
    std::vector<mpz_class> histogram;
    Values seed;
    uint64_t nodes=0;
    ComponentCounter(const Model& m,const Domains& root,std::vector<int> vs,std::vector<int> fs,const Values& witness)
        :variables(std::move(vs)),factors(std::move(fs)),stack{{root,false,-1,0}},histogram(factors.size()+1),seed(witness) {
        int weak=0;for(int id:factors){int n=m.factors[id].count(seed);require(n==2||n==4,"invalid fixed-pattern witness factor");weak+=n==2;}histogram[weak]=1;
    }
    bool done() const { return stack.empty(); }
    void step(const Model& m) {
        if(done())return;
        auto& f=stack.back();
        if(!f.prepared) {
            ++nodes;if(!propagate(m,f.dom,factors)){stack.pop_back();return;}
            f.variable=branch_variable(m,f.dom,variables);
            if(f.variable<0) {
                Values x(seed);bool same=true;for(int v:variables){x[v]=uint8_t(first(f.dom[v]));same&=x[v]==seed[v];}

                if(!same) { int weak=0;for(int id:factors){int n=m.factors[id].count(x);require(n==2||n==4,"nonlegal counter leaf");weak+=n==2;}++histogram[weak]; }
                stack.pop_back();return;
            }
            f.remaining=f.dom[f.variable];f.prepared=true;
        }
        if(!f.remaining){stack.pop_back();return;}
        int value=first(f.remaining);f.remaining&=uint16_t(~(1u<<value));Domains d=f.dom;d[f.variable]=uint16_t(1u<<value);stack.push_back({std::move(d),false,-1,0});
    }
    mpz_class numerator() const { mpz_class n=0;for(size_t k=0;k<histogram.size();++k)n+=histogram[k]<<(factors.size()-k);return n; }
    mpz_class paths() const { mpz_class n=0;for(const auto& z:histogram)n+=z;return n; }
    J json() const {
        J frames=J::array(),hist=J::array();for(const auto& z:histogram)hist.push_back(z.get_str());
        for(const auto& f:stack)frames.push_back({{"domains",f.dom},{"prepared",f.prepared},{"variable",f.variable},{"remaining",f.remaining}});
        return {{"variables",variables},{"factors",factors},{"histogram",hist},{"frames",frames},{"nodes",nodes}};
    }
    J compact_json(const Domains& root) const {
        J frames=J::array(),hist=J::array();
        for(size_t k=0;k<histogram.size();++k)
            if(histogram[k]!=0)hist.push_back({k,histogram[k].get_str()});
        const Domains* parent=&root;
        for(const auto& frame:stack){
            J row=J::array({frame.prepared?frame.variable:-1,frame.remaining});
            for(int v:variables){
                if(frame.dom[v]==(*parent)[v])continue;
                row.push_back(v);
                row.push_back(frame.dom[v]);
            }
            frames.push_back(std::move(row));
            parent=&frame.dom;
        }
        return {{"histogram",std::move(hist)},{"frames",std::move(frames)},{"nodes",nodes}};
    }
    void restore_compact(const Domains& root,const J& record){
        std::fill(histogram.begin(),histogram.end(),0);
        for(const auto& bin:record.at("histogram")){
            require(bin.is_array()&&bin.size()==2,"bad compact histogram bin");
            size_t k=bin[0].get<size_t>();
            require(k<histogram.size()&&histogram[k]==0,"bad compact histogram index");
            histogram[k]=mpz_class(bin[1].get<std::string>());
            require(histogram[k]>0,"bad compact histogram count");
        }
        require(paths()>0,"compact counter lost witness mass");
        const auto& frames=record.at("frames");
        require(frames.is_array()&&frames.size()<=variables.size()+1,"bad compact stack depth");
        stack.clear();
        for(const auto& row:frames){
            require(row.is_array()&&row.size()>=2&&row.size()%2==0,"bad compact stack frame");
            int branch=row[0].get<int>();
            unsigned remaining=row[1].get<unsigned>();
            require(branch>=-1&&remaining<=UINT16_MAX,"bad compact branch cursor");
            auto domains=stack.empty()?root:stack.back().dom;
            int previous=-1;
            for(size_t k=2;k<row.size();k+=2){
                int v=row[k].get<int>();
                unsigned mask=row[k+1].get<unsigned>();
                require(v>previous&&std::binary_search(variables.begin(),variables.end(),v),"bad compact domain index");
                require(mask&&(mask&domains[v])==mask&&mask!=domains[v],"bad compact domain delta");
                domains[v]=uint16_t(mask);
                previous=v;
            }
            if(!stack.empty()){
                const auto& parent=stack.back();
                require(parent.prepared,"unprepared stack frame has a child");
                unsigned visited=parent.dom[parent.variable]&~unsigned(parent.remaining);
                require(visited&&domains[parent.variable]==(1u<<(31-__builtin_clz(visited))),"bad compact child branch");
            }
            if(branch>=0){
                require(std::binary_search(variables.begin(),variables.end(),branch)&&pop(domains[branch])>1,"bad compact branch variable");
                require((remaining&domains[branch])==remaining,"bad compact remaining values");
            }else require(remaining==0,"unprepared compact branch has remaining values");
            stack.push_back({std::move(domains),branch>=0,branch,uint16_t(remaining)});
        }
        nodes=record.at("nodes").get<uint64_t>();
    }
    void restore(const Model& m,const Domains& root,const J& j) {
        require(j.at("variables").get<std::vector<int>>()==variables && j.at("factors").get<std::vector<int>>()==factors,"checkpoint component topology changed");
        auto h=j.at("histogram");require(h.size()==histogram.size(),"bad histogram dimension");
        for(size_t k=0;k<h.size();++k){histogram[k]=mpz_class(h[k].get<std::string>());require(histogram[k]>=0,"negative histogram");}
        require(paths()>0,"checkpoint lost witness mass");stack.clear();
        for(const auto& q:j.at("frames")) {
            SearchFrame f{q.at("domains").get<Domains>(),q.at("prepared").get<bool>(),q.at("variable").get<int>(),q.at("remaining").get<uint16_t>()};
            require(f.dom.size()==size_t(m.nv),"checkpoint domain length mismatch");
            for(int v=0;v<m.nv;++v)require(f.dom[v] && (f.dom[v]&root[v])==f.dom[v],"checkpoint domain outside root");
            if(f.prepared) { require(std::find(variables.begin(),variables.end(),f.variable)!=variables.end(),"bad checkpoint branch");require((f.remaining&f.dom[f.variable])==f.remaining,"bad remaining branch values"); }
            stack.push_back(std::move(f));
        }nodes=j.at("nodes").get<uint64_t>();
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
            if(free<0){++constant_active;constant_weak+=m->factors[i].count(witness)==2;}else fs[d.root(free)].push_back(int(i));
        }
        for(auto& kv:vs)components.emplace_back(*m,root,std::move(kv.second),std::move(fs[kv.first]),witness);
    }
    bool done() const { return std::all_of(components.begin(),components.end(),[](const auto& c){return c.done();}); }
    void step() {
        if(components.empty())return;
        for(size_t i=0;i<components.size();++i){size_t j=cursor++%components.size();if(!components[j].done()){components[j].step(*m);return;}}
    }
    Dyadic mass() const { mpz_class n=mpz_class(1)<<(constant_active-constant_weak);for(const auto& c:components)n*=c.numerator();return {n,unsigned(3*m->active)}; }
    mpz_class paths() const { mpz_class n=1;for(const auto& c:components)n*=c.paths();return n; }
    uint64_t nodes() const { uint64_t n=0;for(const auto& c:components)n+=c.nodes;return n; }
    J weight_histogram() const {
        std::vector<mpz_class> h(constant_weak+1);h[constant_weak]=1;
        for(const auto& c:components){std::vector<mpz_class> q(h.size()+c.histogram.size()-1);for(size_t i=0;i<h.size();++i)for(size_t j=0;j<c.histogram.size();++j)q[i+j]+=h[i]*c.histogram[j];h=std::move(q);}
        J a=J::array();for(size_t k=0;k<h.size();++k)if(h[k]!=0)a.push_back({{"weight",2*m->active+k},{"ddt2",k},{"paths",h[k].get_str()}});return a;
    }
    J json() const { J a=J::array();for(const auto& c:components)a.push_back(c.json());return {{"components",a},{"cursor",cursor},{"constant_active",constant_active},{"constant_weak",constant_weak}}; }
    J compact_json() const {
        J records=J::array();
        for(const auto& component:components)records.push_back(component.compact_json(root));
        return {{"components",std::move(records)},{"cursor",cursor},
                {"constant_active",constant_active},{"constant_weak",constant_weak}};
    }
    void restore_compact(const J& record){
        const auto& saved=record.at("components");
        require(record.at("constant_active")==constant_active&&record.at("constant_weak")==constant_weak&&
                saved.is_array()&&saved.size()==components.size(),"compact counter topology changed");
        for(size_t k=0;k<components.size();++k)components[k].restore_compact(root,saved[k]);
        cursor=record.at("cursor").get<size_t>();
    }
    void restore(const J& j) {
        require(j.at("constant_active")==constant_active&&j.at("constant_weak")==constant_weak&&j.at("components").size()==components.size(),"counter checkpoint mismatch");
        for(size_t i=0;i<components.size();++i)components[i].restore(*m,root,j.at("components")[i]);
        cursor=j.at("cursor").get<size_t>();
    }
};
}
