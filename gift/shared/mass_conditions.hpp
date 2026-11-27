#pragma once

#include "segment_graph.hpp"
#include <deque>
#include <sstream>
namespace segment_mass {
struct MassConditions {
    using Paths=std::vector<std::vector<W>>;
    struct Stat {double log=0;unsigned zeros=0,total=0;std::vector<unsigned> row,hist;};
    struct State {Paths paths;std::vector<Stat> stats;};
    struct Var {unsigned copy,round,bit;};
    const Spec& s;unsigned m,levels,bits;double strength;
    std::vector<State> lanes;std::vector<Var> vars;
    std::vector<double> fields;
    std::vector<unsigned> exchange_order;
    std::mt19937_64 rng;std::uint64_t sequence=0;
    nlohmann::json metrics=nlohmann::json::object();
    MassConditions(const Spec& spec,unsigned copies,unsigned count,unsigned width,double scale,std::uint64_t seed)
        :s(spec),m(copies),levels(count),bits(width),strength(scale),rng(seed){
        if(!m||m>8||levels<3||levels>64||bits<1||bits>20||!std::isfinite(scale)||scale<0||
           !std::isfinite(scale*(double(m)*s.N*(s.R+.5))))
            throw std::runtime_error("invalid mass-conditions options");
        fields.push_back(0);
        for(unsigned h=1;h<levels;++h)fields.push_back(strength*(1.-2.*double(h-1)/double(levels-2)));
        exchange_order.resize(levels);std::iota(exchange_order.begin(),exchange_order.end(),0);
        std::stable_sort(exchange_order.begin(),exchange_order.end(),[&](unsigned h,unsigned k){return fields[h]<fields[k];});
        for(unsigned b=0;b<4*s.N;++b)vars.push_back({0,0,b});
        for(unsigned c=0;c<m;++c)for(unsigned r=1;r<s.R;++r)for(unsigned b=0;b<4*s.N;++b)vars.push_back({c,r,b});
        for(unsigned b=0;b<4*s.N;++b)vars.push_back({0,s.R,b});
    }
    unsigned id(unsigned c,unsigned r,unsigned b)const{
        if(r==0)return b;
        if(r==s.R)return unsigned(vars.size())-4*s.N+b;
        return 4*s.N+(c*(s.R-1)+r-1)*4*s.N+b;
    }
    unsigned factor(const std::vector<W>& p,unsigned r,unsigned cell)const{
        unsigned a=unsigned(p[r]>>(4*cell))&15,b=0;
        for(unsigned j=0;j<4;++j)b|=unsigned((p[r+1]>>s.perm[4*cell+j])&1)<<j;
        return s.ddt[a][b];
    }
    Stat stat(const std::vector<W>& p)const{
        if(p.size()!=s.R+1)throw std::runtime_error("mass path length");
        Stat st;st.hist.assign(s.N+1,0);
        for(unsigned r=0;r<=s.R;++r){if(s.N<32 && (p[r]>>(4*s.N)))throw std::runtime_error("mass state width");
            unsigned a=active(p[r]);st.row.push_back(a);if(r<s.R){st.total+=a;++st.hist[a];}}
        for(unsigned r=0;r<s.R;++r)for(unsigned c=0;c<s.N;++c){unsigned n=factor(p,r,c);if(n)st.log+=std::log2(double(n));else ++st.zeros;}
        return st;
    }
    State state(Paths p)const{
        if(p.size()!=m)throw std::runtime_error("mass replica count");
        State st;st.paths=std::move(p);
        for(const auto& q:st.paths)if(q.size()!=s.R+1)throw std::runtime_error("mass path length");
        for(const auto& q:st.paths){if(q.front()!=st.paths[0].front()||q.back()!=st.paths[0].back())throw std::runtime_error("mass shared endpoints");st.stats.push_back(stat(q));}
        return st;
    }
    static unsigned peak(const Stat& st){for(unsigned k=unsigned(st.hist.size());k-->0;)if(st.hist[k])return k;return 0;}
    double density(const State& st,unsigned h)const{
        double z=0;for(const auto& p:st.stats){if(p.zeros||!p.row.front()||!p.row.back())return -INFINITY;
            z+=p.log-fields.at(h)*(double(p.total)+.5*peak(p));}
        return z;
    }
    void change_factor(State& st,unsigned c,unsigned r,unsigned cell,int sign){
        auto& a=st.stats[c];unsigned n=factor(st.paths[c],r,cell);
        if(n)a.log+=sign*std::log2(double(n));else if(sign>0)++a.zeros;else --a.zeros;
    }
    void flip(State& st,unsigned variable){
        const auto v=vars.at(variable);bool shared=v.round==0||v.round==s.R;
        for(unsigned c=shared?0:v.copy;c<(shared?m:v.copy+1);++c){
            if(v.round<s.R)change_factor(st,c,v.round,v.bit/4,-1);
            if(v.round>0)change_factor(st,c,v.round-1,s.iperm[v.bit]/4,-1);
            auto& a=st.stats[c];unsigned old=a.row[v.round];
            st.paths[c][v.round]^=W(1)<<v.bit;unsigned next=active(st.paths[c][v.round]);a.row[v.round]=next;
            if(v.round<s.R){a.total=a.total-old+next;--a.hist[old];++a.hist[next];}
            if(v.round<s.R)change_factor(st,c,v.round,v.bit/4,1);
            if(v.round>0)change_factor(st,c,v.round-1,s.iperm[v.bit]/4,1);
        }
    }
    using Basis=std::vector<std::vector<unsigned>>;
    void toggle(State& st,const std::vector<unsigned>& column){for(unsigned v:column)flip(st,v);}
    std::vector<unsigned> neighbors(unsigned variable)const{
        auto v=vars.at(variable);std::vector<unsigned> out;bool shared=v.round==0||v.round==s.R;
        auto add=[&](unsigned c,unsigned r,unsigned cell){for(unsigned j=0;j<4;++j){out.push_back(id(c,r,4*cell+j));out.push_back(id(c,r+1,s.perm[4*cell+j]));}};
        for(unsigned c=shared?0:v.copy;c<(shared?m:v.copy+1);++c){
            if(v.round<s.R)add(c,v.round,v.bit/4);
            if(v.round>0)add(c,v.round-1,s.iperm[v.bit]/4);}
        std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());return out;
    }

    Basis wired_block(){
        unsigned width=1+unsigned(rng()%bits),root=unsigned(rng()%vars.size());
        std::set<unsigned> selected{root};std::deque<unsigned> queue{root};
        while(!queue.empty()&&selected.size()<width){unsigned v=queue.front();queue.pop_front();auto ns=neighbors(v);std::shuffle(ns.begin(),ns.end(),rng);
            for(unsigned w:ns)if(selected.size()<width&&selected.insert(w).second)queue.push_back(w);}
        Basis b;for(unsigned v:selected)b.push_back({v});return b;
    }

    Basis long_block(){
        unsigned lo=unsigned(rng()%(s.R+1)),hi=unsigned(rng()%(s.R+1));if(lo>hi)std::swap(lo,hi);
        unsigned c=unsigned(rng()%m),cell=unsigned(rng()%s.N);bool inverse=bool(rng()&1),shared=lo==0||hi==s.R;
        Basis basis(4);
        for(unsigned j=0;j<4;++j){W delta=W(1)<<(4*cell+j);
            for(unsigned r=lo;r<=hi;++r){for(unsigned b=0;b<4*s.N;++b)if((delta>>b)&1){
                if(shared&&r>0&&r<s.R)for(unsigned k=0;k<m;++k)basis[j].push_back(id(k,r,b));
                else basis[j].push_back(id(c,r,b));}
                delta=s.route(delta,inverse);}}
        return basis;
    }
    static double logadd(double a,double b){if(!std::isfinite(a))return b;if(!std::isfinite(b))return a;double hi=std::max(a,b);return hi+std::log2(1+std::exp2(std::min(a,b)-hi));}
    bool heatbath(State& st,unsigned h,const Basis& basis,const std::function<void()>& check){
        if(basis.empty()||basis.size()>20)throw std::runtime_error("mass block width");
        State original=st;double norm=-INFINITY;unsigned chosen=0,gray=0;const unsigned count=1u<<basis.size();
        try{for(unsigned k=0;k<count;++k){if((k&63)==0)check();unsigned next=k^(k>>1);
                if(k)toggle(st,basis[unsigned(__builtin_ctz(gray^next))]);
                gray=next;
                double w=density(st,h),updated=logadd(norm,w);
                if(std::isfinite(w)&&(!std::isfinite(norm)||
                   std::log2(std::max(std::generate_canonical<double,53>(rng),std::numeric_limits<double>::min()))<w-updated))chosen=gray;
                norm=updated;
            }
            if(!std::isfinite(norm))throw std::runtime_error("legal mass orbit lost its current state");
        }catch(...){st=std::move(original);throw;}

        st=std::move(original);for(unsigned j=0;j<basis.size();++j)if((chosen>>j)&1)toggle(st,basis[j]);
        for(unsigned c=0;c<m;++c)st.stats[c]=stat(st.paths[c]);
        metrics["conditional_assignments"]=metrics.value("conditional_assignments",std::uint64_t(0))+count;
        return chosen!=0;
    }
    double swap_log(const State& x,const State& y,unsigned h,unsigned k)const{return density(y,h)+density(x,k)-density(x,h)-density(y,k);}
    void initialize(const std::vector<std::vector<W>>& seeds){if(seeds.empty())throw std::runtime_error("mass initializer empty");
        lanes.clear();for(unsigned h=0;h<levels;++h){const auto& p=seeds[h%seeds.size()];Paths ps(m,p);

            unsigned j=0;for(const auto& q:seeds)if(q.front()==p.front()&&q.back()==p.back())ps[(j++)%m]=q;
            auto st=state(std::move(ps));if(!std::isfinite(density(st,h)))throw std::runtime_error("mass initializer illegal");lanes.push_back(std::move(st));}}
    void step(const std::function<void()>& check){
        if(lanes.size()!=levels)throw std::runtime_error("mass lane count");
        unsigned h=unsigned(sequence%levels);bool wide=(sequence/levels)%2;auto basis=wide?long_block():wired_block();
        bool changed=heatbath(lanes[h],h,basis,check);auto key=h?"auxiliary_moves":"physical_moves";
        metrics[key]=metrics.value(key,std::uint64_t(0))+changed;
        metrics[wide?"long_blocks":"wired_blocks"]=metrics.value(wide?"long_blocks":"wired_blocks",std::uint64_t(0))+1;
        ++sequence;
        if(sequence%levels==0){unsigned parity=unsigned((sequence/levels)%2);

            for(unsigned edge=parity;edge+1<levels;edge+=2){check();
                unsigned i=exchange_order[edge],k=exchange_order[edge+1];
                double q=swap_log(lanes[i],lanes[k],i,k);
                bool accepted=q>=0||std::log2(std::max(std::generate_canonical<double,53>(rng),std::numeric_limits<double>::min()))<q;
                if(accepted)std::swap(lanes[i],lanes[k]);
                auto& d=metrics["exchanges"][std::to_string(i)+"_"+std::to_string(k)];if(d.is_null())d=nlohmann::json::object();
                d["attempts"]=d.value("attempts",std::uint64_t(0))+1;d["accepted"]=d.value("accepted",std::uint64_t(0))+accepted;}}
        unsigned mx=0;for(const auto& p:lanes[0].stats)mx=std::max(mx,peak(p));
        metrics["physical_max_activity"]=std::max(metrics.value("physical_max_activity",0u),mx);
        metrics["steps"]=sequence;
    }
    nlohmann::json checkpoint()const{
        using J=nlohmann::json;J paths=J::array();for(const auto& st:lanes){J ps=J::array();for(const auto& p:st.paths){J row=J::array();for(W x:p)row.push_back(hex(x,s.N));ps.push_back(row);}paths.push_back(ps);}
        std::ostringstream r;r<<rng;return {{"revision",2},{"identity",s.identity()},{"rounds",s.R},{"copies",m},{"levels",levels},{"bits",bits},{"strength",strength},{"sequence",sequence},{"rng",r.str()},{"paths",paths},{"metrics",metrics}};
    }
    bool restore(const nlohmann::json& j){
        if(j.value("revision",0)!=2||j.value("identity",std::string())!=s.identity()||j.value("rounds",0u)!=s.R||j.value("copies",0u)!=m||j.value("levels",0u)!=levels||j.value("bits",0u)!=bits||j.value("strength",-1.)!=strength)return false;
        if(!j.at("paths").is_array()||!j.at("metrics").is_object())throw std::runtime_error("mass checkpoint shape");
        std::vector<State> next;for(const auto& ps:j.at("paths")){Paths pp;for(const auto& p:ps){std::vector<W> row;for(const auto& x:p)row.push_back(word(x.get<std::string>(),s.N));pp.push_back(row);}next.push_back(state(std::move(pp)));}
        if(next.size()!=levels)throw std::runtime_error("mass checkpoint lanes");
        for(unsigned h=0;h<levels;++h)if(!std::isfinite(density(next[h],h)))throw std::runtime_error("mass checkpoint illegal");
        std::mt19937_64 rr;std::istringstream stream(j.at("rng").get<std::string>());if(!(stream>>rr))throw std::runtime_error("mass checkpoint RNG");
        lanes=std::move(next);rng=rr;sequence=j.at("sequence");metrics=j.at("metrics");return true;
    }
};
}
