#pragma once
#include "model.hpp"
#include <random>

namespace thermo {

struct SupportCount {
    std::array<std::uint64_t,4> w{};
    SupportCount(std::uint64_t n=0) { w[0]=n; }
    bool zero() const { return !(w[0]|w[1]|w[2]|w[3]); }
    int compare(const SupportCount &b) const {
        for(int i=3;i>=0;--i) if(w[i]!=b.w[i]) return w[i]<b.w[i]?-1:1;
        return 0;
    }
    void add(const SupportCount &b) {
        unsigned __int128 carry=0;
        for(unsigned i=0;i<4;++i) {
            carry+=static_cast<unsigned __int128>(w[i])+b.w[i];
            w[i]=std::uint64_t(carry);carry>>=64;
        }
        require(!carry,"Activity-sequence counter overflow");
    }
    void subtract(const SupportCount &b) {
        require(compare(b)>=0,"Negative activity-sequence counter");
        std::uint64_t borrow=0;
        for(unsigned i=0;i<4;++i) {
            auto v=w[i];auto t=v-b.w[i];auto b1=v<b.w[i];
            w[i]=t-borrow;borrow=b1 || t<borrow;
        }
        require(!borrow,"Activity-sequence subtraction overflow");
    }
    std::string decimal() const {
        Z z;mpz_import(z.get_mpz_t(),4,-1,sizeof(std::uint64_t),0,0,w.data());
        return z.get_str();
    }
    static SupportCount below(const SupportCount &n,std::mt19937_64 &rng) {
        require(!n.zero(),"Cannot sample an empty activity space");
        unsigned top=3;while(!n.w[top])--top;
        unsigned bits=64-__builtin_clzll(n.w[top]);
        SupportCount x;
        do {
            for(unsigned i=0;i<=top;++i)x.w[i]=rng();
            if(bits<64)x.w[top]&=(std::uint64_t(1)<<bits)-1;
        } while(x.compare(n)>=0);
        return x;
    }
};

struct SupportSpec {
    unsigned columns=4, states=65536;
    std::array<std::array<bool,16>,16> edge{};
    std::array<std::vector<unsigned>,16> outputs;
    std::vector<unsigned> input_pack,output_pack,pop;

    explicit SupportSpec(const Cipher &c) {
        require(!c.present,"Support transfer only supports cell-XOR linear layers");
        for(unsigned v=0;v<65536;++v) {
            unsigned x[4],y[4],a=0,b=0;
            for(unsigned r=0;r<4;++r){x[r]=(v>>(4*r))&15;if(x[r])a|=1u<<r;}
            if(c.midori) {
                auto all=x[0]^x[1]^x[2]^x[3];
                for(unsigned r=0;r<4;++r)y[r]=all^x[r];
            } else {
                y[0]=x[0]^x[2]^x[3];y[1]=x[0];
                y[2]=x[1]^x[2];y[3]=x[0]^x[2];
            }
            for(unsigned r=0;r<4;++r)if(y[r])b|=1u<<r;
            edge[a][b]=true;
        }
        for(unsigned a=0;a<16;++a)for(unsigned b=0;b<16;++b)
            if(edge[a][b])outputs[a].push_back(b);
        input_pack.resize(states);output_pack.resize(states);pop.resize(states);
        for(unsigned a=0;a<states;++a) {
            pop[a]=__builtin_popcount(a);
            for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row) {
                unsigned global=c.midori?4*col+row:4*row+col;
                if(a&(1u<<c.sr[global]))input_pack[a]|=1u<<(4*col+row);
                if(a&(1u<<global))output_pack[a]|=1u<<(4*col+row);
            }
        }
    }

    SupportSpec(unsigned cols,std::array<std::array<bool,16>,16> e)
        :columns(cols),states(1u<<(4*cols)),edge(e) {
        require(cols>=1 && cols<=4,"Invalid test support dimension");
        for(unsigned a=0;a<16;++a)for(unsigned b=0;b<16;++b)
            if(edge[a][b])outputs[a].push_back(b);
        input_pack.resize(states);output_pack.resize(states);pop.resize(states);
        for(unsigned a=0;a<states;++a){input_pack[a]=output_pack[a]=a;pop[a]=__builtin_popcount(a);}
    }
    bool compatible(unsigned a,unsigned b) const {
        unsigned x=input_pack.at(a),y=output_pack.at(b);
        for(unsigned c=0;c<columns;++c)if(!edge[(x>>(4*c))&15][(y>>(4*c))&15])return false;
        return true;
    }
};

class SupportTransfer {
    using Row=std::vector<SupportCount>;
    struct Work { unsigned stage=0,cursor=0;Row a,b;explicit Work(unsigned n):a(n),b(n){} };
    SupportSpec spec;
    unsigned rounds,max_count;
    mutable std::mutex mu;
    std::map<std::pair<unsigned,unsigned>,std::shared_ptr<const Row>> rows;
    std::map<std::pair<unsigned,unsigned>,std::shared_ptr<Work>> work;
    bool enabled=true;
    std::string disabled_reason;
    unsigned wanted=0;
    bool layer_ready_locked(unsigned t) const {
        if(!t)return true;
        for(unsigned k=t;k<=std::min(4*spec.columns*t,max_count-1);++k)
            if(!rows.count({t,k}))return false;
        return true;
    }
  public:
    static std::uint64_t memory_bytes(unsigned R,unsigned N,unsigned columns=4) {
        std::uint64_t n=0,max_layer=0;
        for(unsigned t=1;t<R;++t) {
            auto upper=std::min(4*columns*t,N-1);
            if(upper>=t){auto s=upper-t+1;n+=s;max_layer=std::max<std::uint64_t>(max_layer,s);}
        }
        return (n+2*max_layer)*(std::uint64_t(1)<<(4*columns))*sizeof(SupportCount)+(16ull<<20);
    }
    SupportTransfer(SupportSpec s,unsigned R,unsigned N):spec(std::move(s)),rounds(R),max_count(N) {
        require(R>=2 && R<=16 && N>=1 && N<=4*spec.columns*R,"Unsupported activity transfer range");
        for(unsigned t=1;t<R;++t){auto upper=std::min(4*spec.columns*t,N-1);if(upper>=t)wanted+=upper-t+1;}
    }
    bool ready() const {std::lock_guard<std::mutex> l(mu);return enabled && rows.size()==wanted;}
    void disable(const std::string &why) {std::lock_guard<std::mutex> l(mu);enabled=false;disabled_reason=why;}
    J status() const {
        std::lock_guard<std::mutex> l(mu);
        return J{{"enabled",enabled},{"reason",disabled_reason},{"rows_complete",rows.size()},
                 {"rows_required",wanted},{"ready",enabled && rows.size()==wanted},
                 {"reserved_bytes",memory_bytes(rounds,max_count,spec.columns)},
                 {"counts_are_probability",false},{"scope","labelled locally compatible binary activity sequences"}};
    }
    J next(const std::set<std::pair<unsigned,unsigned>> &busy) const {
        std::lock_guard<std::mutex> l(mu);
        if(!enabled)return nullptr;
        for(unsigned t=1;t<rounds;++t) {
            if(!layer_ready_locked(t-1))return nullptr;
            for(unsigned k=t;k<=std::min(4*spec.columns*t,max_count-1);++k)
                if(!rows.count({t,k}) && !busy.count({t,k}))return J{{"layer",t},{"count",k}};
        }
        return nullptr;
    }
    J build(unsigned t,unsigned k,const Deadline &dl) {
        const auto key=std::make_pair(t,k);
        std::shared_ptr<Work> w;
        std::map<unsigned,std::shared_ptr<const Row>> previous;
        {
            std::lock_guard<std::mutex> l(mu);
            if(rows.count(key))return J{{"status","COMPLETE"},{"support_row",{t,k}}};
            require(enabled && t>=1 && t<rounds && k>=t && k<=std::min(4*spec.columns*t,max_count-1),"Invalid support DP task");
            require(layer_ready_locked(t-1),"Support DP dependency not ready");
            if(!work.count(key))work[key]=std::make_shared<Work>(spec.states);
            w=work.at(key);
            for(unsigned u=0;u<k;++u){auto i=rows.find({t-1,u});if(i!=rows.end())previous[u]=i->second;}
        }
        try {
            while(w->stage<=spec.columns+1) {
                while(w->cursor<spec.states) {
                    if(!(w->cursor%128))dl.check();
                    unsigned a=w->cursor;
                    if(w->stage==0) {
                        if(a && k>=spec.pop[a]) {
                            auto u=k-spec.pop[a];
                            if(t==1 && u==0)w->a[spec.output_pack[a]]=SupportCount(1);
                            else {auto i=previous.find(u);if(i!=previous.end())w->a[spec.output_pack[a]]=i->second->at(a);}
                        }
                    } else if(w->stage<=spec.columns) {
                        unsigned shift=4*(w->stage-1),digit=(a>>shift)&15,base=a&~(15u<<shift);
                        SupportCount total;
                        for(unsigned v:spec.outputs[digit])total.add(w->a[base|(v<<shift)]);
                        w->b[a]=total;
                    } else w->b[a]=w->a[spec.input_pack[a]];
                    ++w->cursor;
                }
                if(w->stage>=1 && w->stage<=spec.columns)w->a.swap(w->b);
                ++w->stage;w->cursor=0;
            }
        } catch(const Cancelled &e) {
            return J{{"status","INCOMPLETE"},{"reason",e.what()},{"support_row",{t,k}},
                     {"support_stage",w->stage},{"support_cursor",w->cursor}};
        }
        {
            std::lock_guard<std::mutex> l(mu);
            rows[key]=std::make_shared<const Row>(std::move(w->b));work.erase(key);
        }
        return J{{"status","COMPLETE"},{"support_row",{t,k}}};
    }
    SupportCount value(unsigned t,int k,unsigned a) const {
        if(!t)return SupportCount(k==0);
        if(k<0)return {};
        auto i=rows.find({t,unsigned(k)});
        return i==rows.end()?SupportCount():i->second->at(a);
    }
    SupportCount total(unsigned n) const {
        require(ready(),"Activity transfer table not prepared");
        SupportCount out;
        for(unsigned a=1;a<spec.states;++a)out.add(value(rounds-1,int(n)-int(spec.pop[a]),a));
        return out;
    }
    std::vector<unsigned> sample(unsigned n,std::mt19937_64 &rng,const Deadline &dl) const {
        auto z=total(n);if(z.zero())return {};
        std::vector<unsigned> sequence;
        int left=int(n);unsigned prev=0;
        for(unsigned r=0;r<rounds;++r) {
            SupportCount sum;
            for(unsigned b=1;b<spec.states;++b) {
                if(!(b%256))dl.check();
                if(!r || spec.compatible(prev,b))sum.add(value(rounds-r-1,left-int(spec.pop[b]),b));
            }
            require(!sum.zero(),"Empty conditional activity transfer");
            auto u=SupportCount::below(sum,rng);bool selected=false;
            for(unsigned b=1;b<spec.states;++b) {
                if(!(b%256))dl.check();
                if(r && !spec.compatible(prev,b))continue;
                auto weight=value(rounds-r-1,left-int(spec.pop[b]),b);
                if(u.compare(weight)<0){sequence.push_back(b);prev=b;left-=int(spec.pop[b]);selected=true;break;}
                u.subtract(weight);
            }
            require(selected,"Activity transfer draw out of range");
        }
        require(left==0,"Activity transfer count mismatch");return sequence;
    }
};
}
