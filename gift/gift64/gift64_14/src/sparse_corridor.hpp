#pragma once
#include <unordered_map>
#include <deque>
#include "corridor_integer.hpp"

namespace present_corridor {
using Word=gift_word::Word;
using Mass=corridor_integer::Mass;
struct Resource:std::runtime_error{using std::runtime_error::runtime_error;};
int activity(Word x){int n=0;for(int i=0;i<NC;++i){n+=(x&15)!=0;x>>=4;}return n;}
std::string text(Word x){return gift_word::text(x,NC);}
Word parse_word(const std::string& s){return gift_word::parse(s,NC);}
struct Node{Mass mass;Word parent=0;Real best=INF;};
struct Hit{
    Word a=0,b=0;Mass mass;int denominator=0;Real weight=0;
    std::array<Word,R+1> witness{};double first=0;
};
struct Row{Word a=0;Real score=-INF;std::vector<Hit> hits;};
struct Diagnostics{
    std::uint64_t rows=0,zero_rows=0,proposals=0,accepts=0,cache_hits=0,swaps=0,swap_attempts=0;
    std::uint64_t peak_nodes=0,peak_edges=0;std::string stop="BUDGET";
    Word unfinished_input=0;
};
struct Kernel{
    struct Choice{Word routed;unsigned numerator;};
    std::array<std::array<std::vector<Choice>,16>,NC> choices;
    std::array<int,256> ddt{};
    Kernel(){
        constexpr int sb[16]={1,10,4,12,6,15,3,9,2,13,11,7,5,0,8,14};
        for(int a=0;a<16;++a)for(int x=0;x<16;++x)++ddt[16*a+(sb[x]^sb[x^a])];
        for(int a=1;a<16;++a)for(int b=0;b<16;++b)if(ddt[16*a+b]!=0&&ddt[16*a+b]!=2&&ddt[16*a+b]!=4&&ddt[16*a+b]!=6)
            throw std::runtime_error("unsupported DDT multiplicity");
        for(int c=0;c<NC;++c)for(int a=1;a<16;++a)for(int b=1;b<16;++b)if(ddt[16*a+b]){
            Word routed=0;for(int j=0;j<4;++j)if((b>>j)&1)routed|=Word(1)<<P(4*c+j);
            choices[std::size_t(c)][std::size_t(a)].push_back({routed,unsigned(ddt[16*a+b]/2)});
        }
    }
    template<class F>void successors(Word x,int cap,F&& f,Timer& timer)const{
        std::array<int,NC> cells{},values{};int count=0;
        for(int c=0;c<NC;++c)if((x>>(4*c))&15){cells[std::size_t(count)]=c;values[std::size_t(count++)]=int((x>>(4*c))&15);}

        std::uint64_t branches=0;
        auto visit=[&](auto&& self,int j,Word y,std::uint64_t n)->void{

            if((++branches&1023)==0)timer.check();
            if(j==count){f(y,3*count,n);return;}
            for(const auto& q:choices[std::size_t(cells[std::size_t(j)])][std::size_t(values[std::size_t(j)])]){
                Word z=y|q.routed;if(cap<NC&&activity(z)>cap)continue;
                self(self,j+1,z,n*q.numerator);
            }
        };
        timer.check();visit(visit,0,0,1);
    }
    Real witness_weight(const std::array<Word,R+1>& x)const{
        Real w=0;
        for(int r=0;r<R;++r){Word y=0;
            for(int i=0;i<NB;++i)if((x[std::size_t(r+1)]>>P(i))&1)y|=Word(1)<<i;
            for(int c=0;c<NC;++c){int n=ddt[16*int((x[std::size_t(r)]>>(4*c))&15)+int((y>>(4*c))&15)];
                if(!n)throw std::runtime_error("corridor witness DDT failure");
                w-=std::log2(Real(n)/16);}
        }return w;
    }
};

template<class TransitionKernel>
Row evaluate(Word a,int cap,int copies,std::size_t node_limit,std::uint64_t edge_limit,
             const TransitionKernel& kernel,Timer& timer,RNG& rng,Diagnostics& diag){
    using Layer=std::unordered_map<Word,Node,gift_word::Hash>;
    std::vector<Layer> layers(std::size_t(R+1));
    Node start;start.mass=Mass(1);start.best=0;layers[0].emplace(a,start);
    std::uint64_t nodes=1,edges=0;int denominator=0;
    for(int r=0;r<R;++r){
        auto& next=layers[std::size_t(r+1)];const auto& old=layers[std::size_t(r)];
        int shift=3*(r==0?activity(a):cap);denominator+=shift;
        for(const auto& entry:old){
            timer.check();kernel.successors(entry.first,r==R-1?NC:cap,[&](Word z,int exponent,std::uint64_t num){
                if(++edges>edge_limit){diag.peak_edges=std::max(diag.peak_edges,edges);throw Resource("EDGE_LIMIT");}
                if((edges&4095)==0)timer.check();
                auto inserted=next.emplace(z,Node{});
                if(inserted.second&&++nodes>node_limit){diag.peak_nodes=std::max(diag.peak_nodes,nodes);throw Resource("NODE_LIMIT");}
                auto& dest=inserted.first->second;
                const Real weight=exponent-std::log2(Real(num));
                if(exponent>shift)throw std::runtime_error("invalid corridor common denominator");
                dest.mass+=(entry.second.mass*Mass(num)).shifted(unsigned(shift-exponent));
                if(entry.second.best+weight<dest.best){dest.best=entry.second.best+weight;dest.parent=entry.first;}
            },timer);
        }
        if(next.empty()){
            ++diag.rows;++diag.zero_rows;
            diag.peak_nodes=std::max(diag.peak_nodes,nodes);diag.peak_edges=std::max(diag.peak_edges,edges);
            Row zero;zero.a=a;return zero;
        }
    }
    Row row;row.a=a;Mass total;Real log_sum=-INF;
    auto& last=layers.back();std::vector<Word> best;
    Word sampled=0;

    for(const auto& entry:last){
        timer.check();total+=entry.second.mass;

        Real logq=copies*(std::log(entry.second.mass.value())-denominator*std::log(Real(2)));
        if(!std::isfinite(log_sum)){log_sum=logq;sampled=entry.first;}
        else{
            Real top=std::max(log_sum,logq);
            Real next=top+std::log1p(std::exp(std::min(log_sum,logq)-top));
            if(std::log(unit(rng))<logq-next)sampled=entry.first;
            log_sum=next;
        }
        best.push_back(entry.first);
    }
    if(Mass(1).shifted(unsigned(denominator))<total)throw std::runtime_error("corridor row probability exceeds one");
    std::sort(best.begin(),best.end(),[&](Word x,Word y){
        const auto& u=last.at(x).mass;const auto& v=last.at(y).mass;
        return u==v?x<y:v<u;
    });
    if(best.size()>8)best.resize(8);
    if(sampled&&std::find(best.begin(),best.end(),sampled)==best.end())best.push_back(sampled);
    if(!last.empty()&&!std::isfinite(log_sum))throw std::runtime_error("invalid corridor log score");
    row.score=log_sum;
    for(Word b:best){
        Hit h;h.a=a;h.b=b;h.mass=last.at(b).mass;h.denominator=denominator;h.weight=last.at(b).best;
        h.first=timer.seconds();h.witness[std::size_t(R)]=b;
        for(int r=R;r>0;--r)h.witness[std::size_t(r-1)]=layers[std::size_t(r)].at(h.witness[std::size_t(r)]).parent;
        if(h.witness[0]!=a||std::abs(kernel.witness_weight(h.witness)-h.weight)>1e-9L)throw std::runtime_error("corridor witness reconstruction failed");
        for(int r=1;r<R;++r)if(activity(h.witness[std::size_t(r)])>cap)throw std::runtime_error("corridor witness scope failure");
        row.hits.push_back(std::move(h));
    }
    ++diag.rows;diag.zero_rows+=last.empty();diag.peak_nodes=std::max(diag.peak_nodes,nodes);diag.peak_edges=std::max(diag.peak_edges,edges);
    return row;
}

Word exact_activity_input(int k,RNG& rng){
    if(k<1||k>NC)throw std::runtime_error("bad starting activity");
    std::array<int,NC> positions{};std::iota(positions.begin(),positions.end(),0);
    std::shuffle(positions.begin(),positions.end(),rng);Word x=0;
    for(int i=0;i<k;++i)x|=Word(1+draw(rng,15))<<(4*positions[std::size_t(i)]);
    return x;
}
Word stratified_input(int cap,RNG& rng){return exact_activity_input(1+draw(rng,cap),rng);}
Real stratum_log_volume(int k){
    Real value=k*std::log(Real(15));for(int i=1;i<=k;++i)value+=std::log(Real(NC+1-i))-std::log(Real(i));return value;
}
struct Proposal{Word x;Real log_hastings=0;};
Proposal propose(Word x,int input_cap,RNG& rng){

    switch(draw(rng,5)){
    case 0:{Word y=stratified_input(input_cap,rng);return {y,stratum_log_volume(activity(y))-stratum_log_volume(activity(x))};}
    case 1:{int i=draw(rng,NC),j=draw(rng,NC);Word a=(x>>(4*i))&15,b=(x>>(4*j))&15;return {x^((a^b)<<(4*i))^((a^b)<<(4*j)),0};}
    case 2:{int i=draw(rng,NC);if(!((x>>(4*i))&15))return {x,0};return {(x&~(Word(15)<<(4*i)))|(Word(1+draw(rng,15))<<(4*i)),0};}
    case 3:{int shift=4*(1+draw(rng,NC-1));const Word mask=NB==128?~Word(0):(Word(1)<<64)-1;return {((x<<shift)|(x>>(NB-shift)))&mask,0};}
    default:{int i=draw(rng,NC);Word y=(x&~(Word(15)<<(4*i)))|(Word(draw(rng,16))<<(4*i));
        return {y&&activity(y)<=input_cap?y:x,0};}
    }
}

void run(const Options& o,Timer& timer){
    const int cap=o.center,input_cap=std::min(o.width,4*cap);
    if(cap<1||cap>NC||input_cap<1||input_cap>NC||o.start_active<0||o.start_active>input_cap||(o.copies!=1&&o.copies!=2&&o.copies!=4))throw std::runtime_error("invalid GIFT corridor cap/input/copies");
#ifdef __linux__
    if(o.memory_bytes){
        rlimit previous{};if(getrlimit(RLIMIT_AS,&previous))throw std::runtime_error("cannot inspect corridor RAM limit");
        const auto cap_bytes=static_cast<rlim_t>(o.memory_bytes);
        rlimit limit{std::min(cap_bytes,previous.rlim_cur),previous.rlim_max};
        if(setrlimit(RLIMIT_AS,&limit))throw std::runtime_error("cannot set corridor RAM limit");
    }
#endif

    const std::size_t node_limit=std::size_t(o.corridor_nodes?o.corridor_nodes:std::uint64_t(o.paths));const auto edge_limit=o.sweeps;
    Kernel kernel;RNG rng(o.seed);Diagnostics diag;
    std::map<Word,Row> cache;std::map<std::pair<Word,Word>,Hit> archive;
    std::deque<Word> cache_order;
    const std::size_t cache_limit=std::size_t(std::max<std::uint64_t>(64,
        std::min<std::uint64_t>(32768,o.memory_bytes?o.memory_bytes/(128*1024):8192)));
    double last_checkpoint=-1e100;
    auto stronger=[](const Hit& a,const Hit& b){
        int den=std::max(a.denominator,b.denominator);
        return b.mass.shifted(unsigned(den-b.denominator))<a.mass.shifted(unsigned(den-a.denominator));
    };
    auto collect=[&](const Row& row){
        for(const auto& h:row.hits)archive.emplace(std::make_pair(h.a,h.b),h);
        while(archive.size()>std::size_t(o.limit)){
            auto worst=archive.begin();for(auto it=archive.begin();it!=archive.end();++it)if(stronger(worst->second,it->second))worst=it;
            archive.erase(worst);
        }
    };
    auto checkpoint=[&](){
        if(timer.seconds()-last_checkpoint<5)return;
        const std::string file=o.out+".checkpoint";
        std::ofstream out(file+".part");out.exceptions(std::ios::badbit|std::ios::failbit);
        out<<"input\toutput\tnumerator\tdenominator_power\tcap\tstates\ttrail_weight\tfirst_seconds\n"<<std::setprecision(17);
        for(const auto& item:archive){const auto& h=item.second;
            out<<text(h.a)<<'\t'<<text(h.b)<<'\t'<<h.mass.str()<<'\t'<<h.denominator<<'\t'<<cap<<'\t';
            for(int r=0;r<=R;++r){if(r)out<<',';out<<text(h.witness[std::size_t(r)]);}
            out<<'\t'<<h.weight<<'\t'<<h.first<<'\n';
        }out.close();
        if(std::rename((file+".part").c_str(),file.c_str()))throw std::runtime_error("cannot save row archive checkpoint");
        last_checkpoint=timer.seconds();
    };
    auto get=[&](Word a)->Row{
        auto found=cache.find(a);if(found!=cache.end()){++diag.cache_hits;return found->second;}
        diag.unfinished_input=a;
        Row row=evaluate(a,cap,o.copies,node_limit,edge_limit,kernel,timer,rng,diag);collect(row);
        diag.unfinished_input=0;
        checkpoint();
        if(cache.size()>=cache_limit){cache.erase(cache_order.front());cache_order.pop_front();}
        cache.emplace(a,row);cache_order.push_back(a);return row;
    };

    std::array<Real,4> gamma{.25L,.5L,1,2};std::array<Row,4> states{};
    try{
        Word initial=o.initial.empty()?(o.start_active?exact_activity_input(o.start_active,rng):stratified_input(input_cap,rng)):parse_word(o.initial);
        if(activity(initial)>input_cap||!initial)throw std::runtime_error("corridor start outside input universe");
        Row first=get(initial);
        while(!std::isfinite(first.score)){timer.check();first=get(stratified_input(input_cap,rng));}
        states.fill(first);
        for(std::uint64_t step=0;;++step){
            for(std::size_t lane=0;lane<states.size();++lane){
                timer.check();const auto proposal=propose(states[lane].a,input_cap,rng);Word a=proposal.x;if(a==states[lane].a)continue;
                Row next=get(a);++diag.proposals;
                if(std::isfinite(next.score)&&accept(rng,gamma[lane]*(next.score-states[lane].score)+proposal.log_hastings)){states[lane]=std::move(next);++diag.accepts;}
            }
            for(std::size_t j=std::size_t(step%2);j+1<states.size();j+=2){
                ++diag.swap_attempts;Real ratio=(gamma[j]-gamma[j+1])*(states[j+1].score-states[j].score);
                if(accept(rng,ratio)){std::swap(states[j],states[j+1]);++diag.swaps;}
            }
        }
    }catch(const Budget&){diag.stop="TIME_LIMIT";}catch(const Resource& e){diag.stop=e.what();}
    catch(const std::bad_alloc&){diag.stop="MEMORY_LIMIT";}

    std::ofstream out(o.out+".part");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"input\toutput\tnumerator\tdenominator_power\tcap\tstates\ttrail_weight\tfirst_seconds\n"<<std::setprecision(17);
    for(const auto& item:archive){const auto& h=item.second;
        out<<text(h.a)<<'\t'<<text(h.b)<<'\t'<<h.mass.str()<<'\t'<<h.denominator<<'\t'<<cap<<'\t';
        for(int r=0;r<=R;++r){if(r)out<<',';out<<text(h.witness[std::size_t(r)]);}
        out<<'\t'<<h.weight<<'\t'<<h.first<<'\n';}
    out.close();publish(o.out);
    std::ofstream d(o.out+".corridor.json");
    d<<"{\"version\":\"collapsed_transfer_v3\",\"cap\":"<<cap<<",\"input_active\":"<<input_cap<<",\"start_active\":"<<o.start_active<<",\"copies\":"<<o.copies
     <<",\"completed_rows\":"<<diag.rows<<",\"proved_zero_rows\":"<<diag.zero_rows
     <<",\"proposals\":"<<diag.proposals<<",\"accepted_inputs\":"<<diag.accepts<<",\"cache_hits\":"<<diag.cache_hits
     <<",\"swap_attempts\":"<<diag.swap_attempts<<",\"swaps\":"<<diag.swaps
     <<",\"peak_nodes\":"<<diag.peak_nodes<<",\"peak_edges\":"<<diag.peak_edges
     <<",\"unfinished_input\":\""<<text(diag.unfinished_input)<<"\",\"certificates\":"<<archive.size()<<",\"stop\":\""<<diag.stop<<"\",\"equilibrium_certified\":false}\n";
}
}
