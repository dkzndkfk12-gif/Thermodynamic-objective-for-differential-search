#pragma once

namespace present_ensemble {
using Trail=std::array<Nibbles,R+1>;
struct Path {
    Trail x{};
    std::array<unsigned char,CELLS> b{},n{};
    std::array<int,17> count{};
    int active=0,first=0,last=0;
};
struct Tuple {std::vector<Path> paths;};
struct Level {std::array<Real,17> log{};bool hard=false;Real beta=1;};
struct Move {
    struct Coordinate {int r=0,c=0;std::array<unsigned char,16> delta{};};
    struct Effect {int f=0;std::array<unsigned char,16> a{},b{};};
    std::vector<Coordinate> coordinates;
    std::vector<Effect> effects;
    bool shared=false;
};
struct Statistics {
    uint64_t sweeps=0,hard_observations=0,soft_observations=0;
    uint64_t hard_move_attempts=0,hard_moves_changed=0;
    uint64_t hard_pattern_changes=0,hard_endpoint_changes=0;
    uint64_t hard_pattern_transitions=0,hard_endpoint_transitions=0;
    uint64_t recombination_attempts=0,recombinations=0;
    uint64_t block_attempts=0,block_moves=0,block_pattern_changes=0;
    uint64_t boundary_block_attempts=0,boundary_block_moves=0;
    uint64_t boundary_block_pattern_changes=0,boundary_block_endpoint_changes=0;
    uint64_t collective_attempts=0,collective_moves=0,collective_pattern_changes=0;
    std::vector<uint64_t> swap_attempts,swap_accepts;
};
struct System {
    std::array<int,256> ddt{};int cap;
    explicit System(int a):cap(a){
        constexpr int sb[16]={1,10,4,12,6,15,3,9,2,13,11,7,5,0,8,14};
        for(int u=0;u<16;++u)for(int x=0;x<16;++x)++ddt[16*u+(sb[x]^sb[x^u])];
    }
    Path path(const Trail& x)const{
        Path p;p.x=x;
        for(int c=0;c<NC;++c){p.first+=x[0][c]!=0;p.last+=x[R][c]!=0;}
        for(int r=0;r<R;++r){auto b=inverse_linear(x[r+1]);for(int c=0;c<NC;++c){
            int f=NC*r+c;p.b[f]=b[c];p.n[f]=static_cast<unsigned char>(ddt[16*x[r][c]+b[c]]);
            ++p.count[p.n[f]];p.active+=x[r][c]!=0;
        }}return p;
    }
    bool legal(const Path& p)const{return p.first&&p.last&&p.active<=cap&&!p.count[0];}
    Real weight(const Path& p)const{return legal(p)?2*p.count[4]+3*p.count[2]+(3-std::log2(Real(3)))*p.count[6]:-1;}
    Level level(Real t,bool hard=false,Real beta=1)const{
        Level l;l.hard=hard;l.beta=beta;l.log.fill(-INF);Real eps=std::exp(-t);
        for(int n:{0,2,4,6,16})l.log[n]=hard?(n?beta*std::log(Real(n)/16):-INF):
            (n?std::log(eps+(1-eps)*Real(n)/16):-t);
        return l;
    }
    Real density(const Tuple& s,const Level& l)const{
        Real value=0;for(const auto& p:s.paths){
            if(!p.first||!p.last||p.active>cap)return -INF;
            for(int n:{0,2,4,6,16})if(p.count[n]){
                if(!std::isfinite(l.log[n]))return -INF;
                value+=p.count[n]*l.log[n];
            }
        }return value;
    }
    Move move(const Directions& directions)const{
        Move m;std::map<int,Move::Effect> effects;
        for(int r=0;r<=R;++r)for(int c=0;c<NC;++c){
            Move::Coordinate q;q.r=r;q.c=c;bool any=false;
            for(int delta=0;delta<16;++delta)for(int bit=0;bit<4;++bit)
                if((delta>>bit)&1)q.delta[delta]^=directions[bit][r][c];
            for(auto x:q.delta)any|=x!=0;
            if(!any)continue;
            m.coordinates.push_back(q);m.shared|=(r==0||r==R);
            if(r<R){int f=NC*r+c;auto& e=effects[f];e.f=f;
                for(int d=0;d<16;++d)e.a[d]^=q.delta[d];}
            if(r>0)for(int bit=0;bit<4;++bit){
                int dst=4*c+bit,src=PI(dst),f=NC*(r-1)+src/4;
                auto& e=effects[f];e.f=f;
                for(int d=0;d<16;++d)e.b[d]^=static_cast<unsigned char>(((q.delta[d]>>bit)&1)<<(src%4));
            }
        }
        for(auto& e:effects)m.effects.push_back(std::move(e.second));
        return m;
    }

    void update(Tuple& s,const Level& l,const Move& m,int chosen,RNG& rng,Statistics& stats)const{
        int begin=m.shared?0:chosen,end=m.shared?int(s.paths.size()):chosen+1;
        std::vector<Real> score(16,0);
        for(int d=0;d<16;++d)for(int ci=begin;ci<end;++ci){
            const auto& p=s.paths[ci];int a=p.active,first=p.first,last=p.last;
            for(const auto& q:m.coordinates){int change=int((p.x[q.r][q.c]^q.delta[d])!=0)-int(p.x[q.r][q.c]!=0);
                if(q.r<R)a+=change;
                if(q.r==0)first+=change;
                if(q.r==R)last+=change;}
            if(a>cap||!first||!last){score[d]=-INF;break;}
            for(const auto& e:m.effects){int a0=p.x[e.f/NC][e.f%NC]^e.a[d],b0=p.b[e.f]^e.b[d];
                score[d]+=l.log[ddt[16*a0+b0]];
                if(!std::isfinite(score[d]))break;
            }
        }
        int d=choose(score,rng);if(l.hard&&l.beta==1)++stats.hard_move_attempts;
        if(!d)return;
        bool pattern_changed=false,end_changed=false;
        for(int ci=begin;ci<end;++ci){auto& p=s.paths[ci];
            for(const auto& e:m.effects){int a=p.x[e.f/NC][e.f%NC]^e.a[d],b=p.b[e.f]^e.b[d];
                --p.count[p.n[e.f]];p.b[e.f]=static_cast<unsigned char>(b);
                p.n[e.f]=static_cast<unsigned char>(ddt[16*a+b]);++p.count[p.n[e.f]];}
            for(const auto& q:m.coordinates){auto& x=p.x[q.r][q.c];
                int change=int((x^q.delta[d])!=0)-int(x!=0);
                if(q.r<R){p.active+=change;pattern_changed|=change!=0;}
                if(q.r==0)p.first+=change;
                if(q.r==R)p.last+=change;
                if(q.r==0||q.r==R)end_changed|=q.delta[d]!=0;
                x^=q.delta[d];
            }
        }
        if(l.hard&&l.beta==1){++stats.hard_moves_changed;stats.hard_pattern_changes+=pattern_changed;stats.hard_endpoint_changes+=end_changed;}
    }

    void recombine(Tuple& s,RNG& rng,bool hard,Statistics& stats)const{
        if(s.paths.size()<2)return;
        int i=draw(rng,int(s.paths.size())),j;
        do{j=draw(rng,int(s.paths.size()));}while(i==j);
        int lo=draw(rng,R-1),hi=lo+2+draw(rng,R-lo-1);
        if(hard)++stats.recombination_attempts;
        const auto& a=s.paths[i];const auto& b=s.paths[j];
        if(a.x[lo]!=b.x[lo]||a.x[hi]!=b.x[hi])return;
        auto ax=a.x,bx=b.x;bool changed=false;
        for(int r=lo+1;r<hi;++r){changed|=ax[r]!=bx[r];std::swap(ax[r],bx[r]);}
        if(!changed)return;
        auto aa=path(ax),bb=path(bx);if(aa.active>cap||bb.active>cap)return;
        s.paths[i]=std::move(aa);s.paths[j]=std::move(bb);if(hard)++stats.recombinations;
    }
};
#include "blocked_gibbs.hpp"
Trail read_trail(std::istream& input){
    Trail x{};std::string text;
    for(auto& row:x){if(!(input>>text)||text.size()!=NC)throw std::runtime_error("invalid ensemble witness state");
        for(int c=0;c<NC;++c){auto pos=std::string("0123456789ABCDEF").find(text[NC-1-c]);
            if(pos==std::string::npos)throw std::runtime_error("invalid ensemble witness hex");
            row[c]=static_cast<unsigned char>(pos);}}
    return x;
}
std::string state_text(const Trail& x){std::string s;for(int r=0;r<=R;++r){if(r)s+=',';s+=nibble_text(x[r]);}return s;}
std::string pattern_text(const Trail& x){std::string s;for(int r=0;r<R;++r)for(auto a:x[r])s+=a?'1':'0';return s;}
void run(const Options& o,Timer& timer){
    System sys(o.active);RNG rng(o.seed);std::ifstream input(o.bridge);std::string magic;
    int rounds=0,n=0;if(!(input>>magic>>rounds>>n)||magic!="GIFT_ENSEMBLE_V1"||rounds!=R||n<1||n>16)
        throw std::runtime_error("invalid ensemble witness header");
    std::vector<Path> seeds;for(int i=0;i<n;++i){auto p=sys.path(read_trail(input));
        if(!sys.legal(p))throw std::runtime_error("ensemble witness violates DDT or active cap");
        if(i&&(p.x[0]!=seeds[0].x[0]||p.x[R]!=seeds[0].x[R]))throw std::runtime_error("ensemble initial endpoints differ");
        seeds.push_back(std::move(p));}
    if(input>>magic)throw std::runtime_error("extra ensemble witness data");
    std::vector<Move> local,collective,endpoints;
    for(int r=0;r<=R;++r)for(int c=0;c<NC;++c){Directions d{};
        for(int bit=0;bit<4;++bit)d[bit][r][c]=static_cast<unsigned char>(1u<<bit);
        if(r==0||r==R)endpoints.push_back(sys.move(d));
        else{local.push_back(sys.move(d));for(int bit=0;bit<4;++bit)d[bit][r]=linear(d[bit][r]);local.push_back(sys.move(d));}}
    for(int lo=0;lo<R;++lo)for(int hi=lo+1;hi<=R;++hi){
        bool boundary=lo==0||hi==R;
        if(boundary&&(hi-lo>3||(lo==0&&hi==R)))continue;
        for(int c=0;c<NC;++c)for(int reverse=0;reverse<2;++reverse){timer.check();Directions d{};
            for(int bit=0;bit<4;++bit){Nibbles value{};value[c]=static_cast<unsigned char>(1u<<bit);
                for(int r=lo;r<=hi;++r){d[bit][r]=value;value=reverse?inverse_linear(value):linear(value);}}
            (boundary?endpoints:collective).push_back(sys.move(d));
        }
    }

    std::vector<Level> levels;for(Real t:{0.0L,.5L,1.0L,2.0L,3.0L,4.0L,6.0L,10.0L,16.0L})levels.push_back(sys.level(t));
    size_t target=levels.size();
    for(Real beta:{1.0L,1.5L,2.0L,3.0L})levels.push_back(sys.level(16,true,beta));
    std::vector<Tuple> states(levels.size());
    for(auto& s:states)for(int j=0;j<o.copies;++j)s.paths.push_back(seeds[size_t(draw(rng,n))]);
    Statistics stats;stats.swap_attempts.resize(levels.size()-1);stats.swap_accepts.resize(levels.size()-1);
    struct Hit{std::string states;Real weight=0;double first=0;uint64_t observations=0;};
    struct Group{uint64_t hard=0,soft=0;std::map<std::string,Hit> patterns;};
    std::map<std::string,Group> groups;
    auto witnessed_mass=[](const Group& g){Real z=0;for(const auto& h:g.patterns)z+=std::exp2(-h.second.weight);return z;};
    auto keep_patterns=[&](Group& g){
        if(g.patterns.size()<=64)return;
        std::vector<std::string> keys;for(const auto& h:g.patterns)keys.push_back(h.first);
        std::sort(keys.begin(),keys.end(),[&](const auto& a,const auto& b){
            return g.patterns[a].weight!=g.patterns[b].weight?g.patterns[a].weight<g.patterns[b].weight:a<b;});
        std::set<std::string> protected_keys(keys.begin(),keys.begin()+32);
        auto victim=g.patterns.end();
        for(auto it=g.patterns.begin();it!=g.patterns.end();++it)if(!protected_keys.count(it->first))
            if(victim==g.patterns.end()||text_hash(it->first,o.seed)>text_hash(victim->first,o.seed))victim=it;
        g.patterns.erase(victim);
    };
    auto keep_groups=[&](){
        if(groups.size()<=64)return;
        std::vector<std::string> keys;for(const auto& g:groups)keys.push_back(g.first);
        std::sort(keys.begin(),keys.end(),[&](const auto& a,const auto& b){
            Real za=witnessed_mass(groups[a]),zb=witnessed_mass(groups[b]);return za!=zb?za>zb:a<b;});
        std::set<std::string> protected_keys(keys.begin(),keys.begin()+16);
        std::sort(keys.begin(),keys.end(),[&](const auto& a,const auto& b){
            return groups[a].hard!=groups[b].hard?groups[a].hard>groups[b].hard:a<b;});
        protected_keys.insert(keys.begin(),keys.begin()+16);
        auto victim=groups.end();
        for(auto it=groups.begin();it!=groups.end();++it)if(!protected_keys.count(it->first))
            if(victim==groups.end()||text_hash(it->first,o.seed)>text_hash(victim->first,o.seed))victim=it;
        groups.erase(victim);
    };
    auto collect=[&](const Tuple& s,bool hard){
        std::vector<const Path*> legal;for(const auto& p:s.paths)if(sys.legal(p))legal.push_back(&p);
        if(legal.empty())return;
        std::string e=nibble_text(s.paths[0].x[0])+"_"+nibble_text(s.paths[0].x[R]);
        if(hard)++stats.hard_observations;else ++stats.soft_observations;
        auto& g=groups[e];if(hard)++g.hard;else ++g.soft;
        for(const auto* p:legal){auto key=pattern_text(p->x);auto it=g.patterns.find(key);Real w=sys.weight(*p);
            if(it==g.patterns.end()){
                Hit h;h.states=state_text(p->x);h.weight=w;h.first=timer.seconds();it=g.patterns.emplace(key,std::move(h)).first;
            }else if(w<it->second.weight){it->second.weight=w;it->second.states=state_text(p->x);}
            ++it->second.observations;
            keep_patterns(g);
        }
        keep_groups();
    };
    collect(states[target],true);
    try{for(uint64_t step=0;step<o.sweeps;++step){
        const Tuple previous_hard=states[target];
        for(size_t lane=0;lane<states.size();++lane){auto& s=states[lane];const auto& l=levels[lane];
            for(int ci=0;ci<o.copies;++ci){
                for(int j=0;j<32;++j){timer.check();sys.update(s,l,local[size_t(draw(rng,int(local.size())))],ci,rng,stats);}
                for(int j=0;j<4;++j){timer.check();sys.update(s,l,collective[size_t(draw(rng,int(collective.size())))],ci,rng,stats);}
            }
            for(int j=0;j<8;++j){timer.check();sys.update(s,l,endpoints[size_t(draw(rng,int(endpoints.size())))],0,rng,stats);}
            if(l.hard&&(lane==target||step%4==0)){

                if(step%16==0)for(int ci=0;ci<o.copies;++ci){timer.check();internal_block(sys,s,l,ci,rng,stats,timer);collective_block(sys,s,l,ci,rng,stats,timer);}
                boundary_block(sys,s,l,false,rng,stats,timer);
                boundary_block(sys,s,l,true,rng,stats,timer);
            }
            sys.recombine(s,rng,l.hard&&l.beta==1,stats);
        }
        ++stats.sweeps;
        for(size_t j=step%2;j+1<states.size();j+=2){++stats.swap_attempts[j];
            Real ratio=sys.density(states[j+1],levels[j])+sys.density(states[j],levels[j+1])-sys.density(states[j],levels[j])-sys.density(states[j+1],levels[j+1]);
            if(!std::isnan(ratio)&&accept(rng,ratio)){std::swap(states[j],states[j+1]);++stats.swap_accepts[j];}}
        const auto& now=states[target];
        stats.hard_endpoint_transitions+=(previous_hard.paths[0].x[0]!=now.paths[0].x[0]||
                                         previous_hard.paths[0].x[R]!=now.paths[0].x[R]);
        bool changed=false;
        for(int ci=0;ci<o.copies;++ci)for(int r=0;r<R;++r)for(int c=0;c<NC;++c)
            changed|=(previous_hard.paths[ci].x[r][c]!=0)!=(now.paths[ci].x[r][c]!=0);
        stats.hard_pattern_transitions+=changed;
        if(step%4==0){collect(states[target],true);collect(states[target-1],false);collect(states.back(),false);}
    }}catch(const Budget&){}
    collect(states[target],true);
    std::vector<std::string> order;for(const auto& g:groups)order.push_back(g.first);
    std::sort(order.begin(),order.end(),[&](const auto& a,const auto& b){
        if(groups[a].hard!=groups[b].hard)return groups[a].hard>groups[b].hard;
        if(groups[a].soft!=groups[b].soft)return groups[a].soft>groups[b].soft;
        return a<b;});
    std::vector<std::string> selected;
    auto add_unique=[&](const std::string& e){if(std::find(selected.begin(),selected.end(),e)==selected.end())selected.push_back(e);};
    auto mass_order=order;std::sort(mass_order.begin(),mass_order.end(),[&](const auto& a,const auto& b){
        Real za=witnessed_mass(groups[a]),zb=witnessed_mass(groups[b]);return za!=zb?za>zb:a<b;});
    for(size_t k=0;k<std::min(size_t(2),mass_order.size());++k)add_unique(mass_order[k]);
    for(size_t k=0;k<std::min(size_t(2),order.size());++k)add_unique(order[k]);
    size_t main_count=selected.size();
    auto diverse=order;std::sort(diverse.begin(),diverse.end(),[&](const auto& a,const auto& b){return text_hash(a,o.seed)<text_hash(b,o.seed);});
    for(const auto& e:diverse)if(selected.size()<main_count+2&&std::find(selected.begin(),selected.end(),e)==selected.end())selected.push_back(e);
    std::map<std::string,std::vector<std::string>> pattern_order;
    for(const auto& e:selected){auto& keys=pattern_order[e];
        for(const auto& entry:groups[e].patterns)keys.push_back(entry.first);
        std::sort(keys.begin(),keys.end(),[&](const auto& a,const auto& b){
            return groups[e].patterns[a].weight!=groups[e].patterns[b].weight?
                groups[e].patterns[a].weight<groups[e].patterns[b].weight:a<b;});
        auto diverse_keys=keys;std::sort(diverse_keys.begin(),diverse_keys.end(),[&](const auto& a,const auto& b){return text_hash(a,o.seed)<text_hash(b,o.seed);});
        auto mass_keys=keys;keys.clear();size_t mi=0,di=0;
        while(keys.size()<mass_keys.size())for(int source=0;source<2;++source){
            auto& from=source?diverse_keys:mass_keys;auto& cursor=source?di:mi;
            while(cursor<from.size()&&std::find(keys.begin(),keys.end(),from[cursor])!=keys.end())++cursor;
            if(cursor<from.size())keys.push_back(from[cursor++]);}
    }
    std::ofstream out(o.out+".part");out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"states\tfirst_seconds\ttrail_weight\thard_visits\tsoft_visits\n"<<std::setprecision(17);
    int rows=0;for(size_t k=0;k<64&&rows<o.limit;++k)for(const auto& e:selected){
        const auto& g=groups[e];if(k>=g.patterns.size()||rows>=o.limit)continue;
        const auto& h=g.patterns.at(pattern_order[e][k]);
        out<<h.states<<'\t'<<h.first<<'\t'<<h.weight<<'\t'<<g.hard<<'\t'<<g.soft<<'\n';++rows;
    }
    out.close();publish(o.out);
    std::ofstream diag(o.out+".ensemble.json");diag<<"{\"version\":\"blocked_joint_replicas_v2\",\"copies\":"<<o.copies<<",\"active_cap\":"<<o.active
        <<",\"target_lane\":"<<target<<",\"target_beta\":1,\"legal_betas\":[1,1.5,2,3]"
        <<",\"block_attempts\":"<<stats.block_attempts<<",\"block_moves\":"<<stats.block_moves<<",\"block_pattern_changes\":"<<stats.block_pattern_changes
        <<",\"collective_attempts\":"<<stats.collective_attempts<<",\"collective_moves\":"<<stats.collective_moves<<",\"collective_pattern_changes\":"<<stats.collective_pattern_changes
        <<",\"boundary_block_attempts\":"<<stats.boundary_block_attempts<<",\"boundary_block_moves\":"<<stats.boundary_block_moves
        <<",\"boundary_block_pattern_changes\":"<<stats.boundary_block_pattern_changes<<",\"boundary_block_endpoint_changes\":"<<stats.boundary_block_endpoint_changes
        <<",\"sweeps\":"<<stats.sweeps<<",\"hard_observations\":"<<stats.hard_observations<<",\"soft_observations\":"<<stats.soft_observations
        <<",\"hard_move_attempts\":"<<stats.hard_move_attempts<<",\"hard_moves_changed\":"<<stats.hard_moves_changed
        <<",\"hard_pattern_changes\":"<<stats.hard_pattern_changes<<",\"hard_endpoint_changes\":"<<stats.hard_endpoint_changes
        <<",\"hard_pattern_transitions\":"<<stats.hard_pattern_transitions<<",\"hard_endpoint_transitions\":"<<stats.hard_endpoint_transitions
        <<",\"recombination_attempts\":"<<stats.recombination_attempts<<",\"recombinations\":"<<stats.recombinations
        <<",\"selected_families\":"<<rows<<",\"retained_endpoints\":"<<groups.size()
        <<",\"visits_are_probability\":false,\"equilibrium_certified\":false,\"swaps\":[";
    for(size_t j=0;j<stats.swap_attempts.size();++j){if(j)diag<<',';diag<<'['<<stats.swap_attempts[j]<<','<<stats.swap_accepts[j]<<']';}diag<<"]}\n";
}
}
