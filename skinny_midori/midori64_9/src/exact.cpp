#include "../../common/native_memo.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "ddt_support.hpp"
#include "rounds.hpp"
#ifdef __linux__
#include <sys/resource.h>
#endif
using Clock=std::chrono::steady_clock;

class Big {
    static constexpr unsigned CAP=32;
    std::array<std::uint32_t,CAP> w{};
    unsigned n=0;
public:
    Big(std::uint32_t value=0){if(value){w[0]=value;n=1;}}
    bool operator==(std::uint32_t value)const{return value?(n==1&&w[0]==value):n==0;}
    Big& operator+=(const Big& b){
        const unsigned count=std::max(n,b.n);std::uint64_t carry=0;
        for(unsigned i=0;i<count;++i){std::uint64_t sum=carry+(i<n?std::uint64_t(w[i]):0)+(i<b.n?std::uint64_t(b.w[i]):0);w[i]=std::uint32_t(sum);carry=sum>>32;}
        n=count;if(carry){if(n==CAP)throw std::runtime_error("INTEGER_OVERFLOW");w[n++]=std::uint32_t(carry);}return *this;
    }
    Big& operator*=(std::uint32_t b){
        if(!b||!n){*this=Big();return *this;}std::uint64_t carry=0;
        for(unsigned i=0;i<n;++i){std::uint64_t v=std::uint64_t(w[i])*b+carry;w[i]=std::uint32_t(v);carry=v>>32;}
        if(carry){if(n==CAP)throw std::runtime_error("INTEGER_OVERFLOW");w[n++]=std::uint32_t(carry);}return *this;
    }
    Big& operator*=(const Big& b){
        if(!n||!b.n){*this=Big();return *this;}
        if(b.n==1)return *this*=b.w[0];
        Big out;
        for(unsigned i=0;i<n;++i){std::uint64_t carry=0;
            for(unsigned j=0;j<b.n;++j){unsigned k=i+j;if(k>=CAP)throw std::runtime_error("INTEGER_OVERFLOW");
                std::uint64_t v=std::uint64_t(w[i])*b.w[j]+out.w[k]+carry;out.w[k]=std::uint32_t(v);carry=v>>32;}
            unsigned k=i+b.n;while(carry){if(k>=CAP)throw std::runtime_error("INTEGER_OVERFLOW");std::uint64_t v=std::uint64_t(out.w[k])+carry;out.w[k++]=std::uint32_t(v);carry=v>>32;}
        }
        out.n=std::min(CAP,n+b.n);while(out.n&&!out.w[out.n-1])--out.n;*this=out;return *this;
    }
    std::string str()const{
        if(!n)return "0";
        Big copy=*this;std::vector<std::uint32_t> blocks;
        while(copy.n){std::uint64_t remainder=0;for(unsigned i=copy.n;i-- >0;){std::uint64_t v=(remainder<<32)|copy.w[i];copy.w[i]=std::uint32_t(v/1000000000);remainder=v%1000000000;}
            blocks.push_back(std::uint32_t(remainder));while(copy.n&&!copy.w[copy.n-1])--copy.n;}
        std::string out=std::to_string(blocks.back());for(std::size_t i=blocks.size()-1;i-- >0;){auto s=std::to_string(blocks[i]);out+=std::string(9-s.size(),'0')+s;}return out;
    }
};
struct Factor { int a=0,b=0; std::vector<int> av,bv; };
struct Table {std::string semantic; std::vector<int> scope; std::vector<Big> data; };
struct Engine {
    int nv=0,nf=0; std::vector<int> pin,order,values,positions;
    std::vector<std::vector<int>> domains; std::vector<Factor> factors;
    int ddt[16][16]{}; std::size_t live_cells=0,limit_cells;
    Clock::time_point deadline;
    explicit Engine(const std::string& path,std::uint64_t bytes,double seconds)
      :limit_cells(bytes/256),deadline(Clock::now()+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds))) {
        std::ifstream f(path); std::string magic; int np,no;
        if(!(f>>magic>>nv>>nf>>np>>no)||magic!=EXACT_MAGIC||nv<0||nv>INTERNAL_CELLS||nf<ROUNDS||nf>MAX_FACTORS||np<0||no<0||np+no!=nv)
            throw std::runtime_error("invalid exact header");
        pin.resize(np);order.resize(no);for(int&v:pin)f>>v;for(int&v:order)f>>v;
        auto partition=pin;partition.insert(partition.end(),order.begin(),order.end());std::sort(partition.begin(),partition.end());
        for(int i=0;i<nv;++i)if(partition[i]!=i)throw std::runtime_error("invalid variable partition");
        domains.resize(nv);values.resize(nv);positions.resize(nv);
        for(int i=0;i<nv;++i){int n;f>>n;if(!f||n<1||n>15)throw std::runtime_error("invalid domain size");
            std::set<int> seen;for(int j=0;j<n;++j){int v;f>>v;if(!f||v<1||v>15||!seen.insert(v).second)throw std::runtime_error("invalid domain");domains[i].push_back(v);}values[i]=domains[i][0];}
        for(int i=0;i<nf;++i){Factor e;f>>e.a>>e.b;if(!f||e.a<0||e.a>15||e.b<0||e.b>15)throw std::runtime_error("invalid constant");
            for(auto* s:{&e.av,&e.bv}){int n;f>>n;if(!f||n<0||n>16)throw std::runtime_error("invalid form");std::set<int> seen;
                for(int j=0;j<n;++j){int v;f>>v;if(!f||v<0||v>=nv||!seen.insert(v).second)throw std::runtime_error("invalid form variable");s->push_back(v);}}
            factors.push_back(std::move(e));}
        if(f>>magic)throw std::runtime_error("extra exact model input");
        const int s[16]={12,10,13,3,14,11,15,7,8,9,1,5,0,2,4,6};
        int raw[16][16]{};for(int a=0;a<16;++a)for(int x=0;x<16;++x)++raw[a][s[x]^s[x^a]];
        for(int a=1;a<16;++a)for(int b=1;b<16;++b){if(raw[a][b]!=0&&raw[a][b]!=2&&raw[a][b]!=4)throw std::runtime_error("unexpected DDT");ddt[a][b]=raw[a][b]/2;}
    }
    void check()const{if(Clock::now()>=deadline)throw std::runtime_error("QUERY_TIME_LIMIT");}
    std::size_t size(const std::vector<int>& scope)const{
        std::size_t n=1;for(int v:scope){if(n>std::numeric_limits<std::size_t>::max()/domains[v].size())throw std::runtime_error("TABLE_INDEX_LIMIT");n*=domains[v].size();}return n;
    }
    Table allocate(std::vector<int> scope){auto n=size(scope);if(live_cells>limit_cells||n>limit_cells-live_cells)throw std::runtime_error("TABLE_MEMORY_LIMIT");
        live_cells+=n;Table t;t.scope=std::move(scope);t.data.resize(n);return t;}
    void decode(std::size_t ix,const std::vector<int>& scope){for(auto i=scope.rbegin();i!=scope.rend();++i){int v=*i;positions[v]=int(ix%domains[v].size());values[v]=domains[v][positions[v]];ix/=domains[v].size();}}
    int factor(const Factor&e)const{int a=e.a,b=e.b;for(int v:e.av)a^=values[v];for(int v:e.bv)b^=values[v];return ddt[a][b];}
    Big run(std::uint64_t slice){
        auto count=size(pin);if(slice>=count)throw std::runtime_error("invalid slice");decode(slice,pin);

        ddt_support::Domains reduced(nv);
        for(int v=0;v<nv;++v)for(int x:domains[v])reduced[v].set(x);
        for(int v:pin){reduced[v].reset();reduced[v].set(values[v]);}
        if(!ddt_support::propagate(reduced,factors,ddt,[&](){check();}))return Big(0);
        for(int v=0;v<nv;++v){domains[v].clear();
            for(int x=1;x<16;++x)if(reduced[v][x])domains[v].push_back(x);
            values[v]=domains[v][0];positions[v]=0;}

        std::set<int> pinned(pin.begin(),pin.end());std::map<std::vector<int>,std::vector<int>> groups;
        for(int i=0;i<nf;++i){std::set<int> s;for(int v:factors[i].av)if(!pinned.count(v))s.insert(v);for(int v:factors[i].bv)if(!pinned.count(v))s.insert(v);groups[{s.begin(),s.end()}].push_back(i);}
        std::vector<Table> live;
        for(const auto&g:groups){Table t=allocate(g.first);std::vector<std::string> ancestry;
            for(int id:g.second)ancestry.push_back(native_memo::factor_key(factors[id],domains,[](const Factor&){return 0;},"midori64"));
            t.semantic=native_memo::message_key(ancestry,t.scope,domains);
            bool memoized=native_memo::get(t.semantic,t.data.size(),t.data);
            for(std::size_t i=0;!memoized&&i<t.data.size();++i){if(!(i&1023))check();decode(i,t.scope);Big p=1;for(int j:g.second){int q=factor(factors[j]);if(!q){p=0;break;}p*=q;}t.data[i]=std::move(p);}if(!memoized)native_memo::put(t.semantic,t.data);live.push_back(std::move(t));}
        for(int v:order){
            check();std::vector<int> bucket;std::set<int> vars;
            for(std::size_t i=0;i<live.size();++i)if(std::binary_search(live[i].scope.begin(),live[i].scope.end(),v)){bucket.push_back(int(i));vars.insert(live[i].scope.begin(),live[i].scope.end());}
            if(bucket.empty())throw std::runtime_error("missing sum variable");
            vars.erase(v);Table out=allocate({vars.begin(),vars.end()});std::vector<std::string> ancestry;
            for(int id:bucket)ancestry.push_back(live[id].semantic);
            out.semantic=native_memo::message_key(ancestry,out.scope,domains,v);
            bool memoized=native_memo::get(out.semantic,out.data.size(),out.data);
            struct Projection{int id;std::size_t step=0;std::vector<std::pair<int,std::size_t>> rest;};std::vector<Projection> ps;
            for(int id:bucket){Projection p;p.id=id;std::size_t stride=1;for(auto it=live[id].scope.rbegin();it!=live[id].scope.rend();++it){if(*it==v)p.step=stride;else p.rest.push_back({*it,stride});stride*=domains[*it].size();}ps.push_back(std::move(p));}
            std::vector<std::size_t> offsets(ps.size());
            for(std::size_t i=0;!memoized&&i<out.data.size();++i){if(!(i&1023))check();decode(i,out.scope);for(std::size_t j=0;j<ps.size();++j){offsets[j]=0;for(auto p:ps[j].rest)offsets[j]+=std::size_t(positions[p.first])*p.second;}
                Big total=0;for(std::size_t k=0;k<domains[v].size();++k){Big product=1;for(std::size_t j=0;j<ps.size();++j){const Big&q=live[ps[j].id].data[offsets[j]+k*ps[j].step];if(q==0){product=0;break;}product*=q;}total+=product;}out.data[i]=std::move(total);}
            if(!memoized)native_memo::put(out.semantic,out.data);
            std::vector<Table> next;for(std::size_t i=0;i<live.size();++i){if(std::binary_search(bucket.begin(),bucket.end(),int(i)))live_cells-=live[i].data.size();else next.push_back(std::move(live[i]));}next.push_back(std::move(out));live=std::move(next);
        }
        Big total=1;for(const auto&t:live){if(!t.scope.empty()||t.data.size()!=1)throw std::runtime_error("unsummed variable");total*=t.data[0];}check();return total;
    }
};
int main(int argc,char**argv){
    auto start=Clock::now();std::string status="ERROR",reason="invalid arguments",num;int nf=0;std::uint64_t slice=0;
    try{if(argc!=6)throw std::runtime_error("usage: exact MODEL SLICE ADDRESS_BYTES TABLE_BYTES SECONDS");slice=std::stoull(argv[2]);auto address=std::stoull(argv[3]),tables=std::stoull(argv[4]);double seconds=std::stod(argv[5]);
        if(!(seconds>0)||seconds>86400||tables<256||address<tables)throw std::runtime_error("invalid budget");
#ifdef __linux__
        rlimit lim{};if(getrlimit(RLIMIT_AS,&lim))throw std::runtime_error("getrlimit failed");lim.rlim_cur=std::min<rlim_t>(address,lim.rlim_max);if(setrlimit(RLIMIT_AS,&lim))throw std::runtime_error("setrlimit failed");
#endif
        Engine e(argv[1],tables,seconds);nf=e.nf;Big mass=e.run(slice);num=mass.str();status="EXACT_SLICE";reason="complete";
    }catch(const std::bad_alloc&){status="RESOURCE_DEFERRED";reason="ADDRESS_SPACE_LIMIT";}
    catch(const std::exception&e){reason=e.what();if(reason=="QUERY_TIME_LIMIT"||reason=="TABLE_MEMORY_LIMIT"||reason=="TABLE_INDEX_LIMIT")status="RESOURCE_DEFERRED";}
    double cpu=0;long rss=0;
#ifdef __linux__
    rusage u{};if(!getrusage(RUSAGE_SELF,&u)){cpu=u.ru_utime.tv_sec+u.ru_stime.tv_sec+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)/1e6;rss=u.ru_maxrss;}
#endif

    std::cout<<"{\"status\":\""<<status<<"\",\"reason\":\""<<reason<<"\",\"slice\":"<<slice<<",\"denominator_power\":"<<3*nf
      <<",\"numerator\":\""<<num<<"\",\"wall_seconds\":"<<std::chrono::duration<double>(Clock::now()-start).count()<<",\"shared_message_hits\":"<<mass_messages::cache().hits<<",\"shared_message_stores\":"<<mass_messages::cache().stores<<",\"cpu_seconds\":"<<cpu<<",\"peak_rss_kib\":"<<rss<<"}\n";
    return status=="ERROR"?1:0;
}
