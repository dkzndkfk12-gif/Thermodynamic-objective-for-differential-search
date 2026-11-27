#pragma once

Real heated(uint64_t weight,Real beta){
    return weight?(beta==1?Real(weight):std::pow(Real(weight),beta)):0;
}
size_t weighted_index(const std::vector<Real>& weights,RNG& rng){
    Real sum=std::accumulate(weights.begin(),weights.end(),Real(0));
    if(!(sum>0)||!std::isfinite(sum))throw std::runtime_error("empty/nonfinite block conditional");
    Real u=unit(rng)*sum;size_t last=weights.size();
    for(size_t j=0;j<weights.size();++j)if(weights[j]>0){
        last=j;if(u<weights[j])return j;u-=weights[j];
    }
    return last;
}
bool different_pattern(const Path& a,const Path& b){
    for(int r=0;r<R;++r)for(int c=0;c<NC;++c)
        if((a.x[r][c]!=0)!=(b.x[r][c]!=0))return true;
    return false;
}

void internal_block(const System& sys,Tuple& tuple,const Level& level,int ci,
                    RNG& rng,Statistics& stats,Timer& timer){
    int r=1+draw(rng,R-1),q=draw(rng,NC/4);auto& p=tuple.paths[ci];
    bool measured=level.beta==1;
    if(measured)++stats.block_attempts;
    Nibbles next=inverse_linear(p.x[r+1]);
    std::array<std::vector<int>,4> support;
    for(int j=0;j<4;++j)for(int y=0;y<16;++y)
        if(sys.ddt[16*p.x[r-1][4*q+j]+y])support[j].push_back(y);
    int old_active=0;for(int i=0;i<4;++i)old_active+=p.x[r][(NC/4)*i+q]!=0;
    std::vector<unsigned> candidates;std::vector<Real> weights;
    for(int a:support[0])for(int b:support[1])for(int c:support[2])for(int d:support[3]){
        timer.check();std::array<int,4> y{a,b,c,d},x{};int active=0;
        uint64_t w=1;
        for(int j=0;j<4;++j)w*=unsigned(sys.ddt[16*p.x[r-1][4*q+j]+y[j]]);
        for(int i=0;i<4;++i){
            for(int j=0;j<4;++j)x[i]|=((y[j]>>((i+j)%4))&1)<<((i+j)%4);
            active+=x[i]!=0;w*=unsigned(sys.ddt[16*x[i]+next[(NC/4)*i+q]]);
        }
        if(!w||p.active-old_active+active>sys.cap)continue;
        candidates.push_back(unsigned(a|(b<<4)|(c<<8)|(d<<12)));
        weights.push_back(heated(w,level.beta));
    }
    unsigned y=candidates.at(weighted_index(weights,rng));auto x=p.x;
    for(int i=0;i<4;++i){x[r][(NC/4)*i+q]=0;
        for(int j=0;j<4;++j)x[r][(NC/4)*i+q]|=((y>>(4*j+(i+j)%4))&1)<<((i+j)%4);}
    if(x==p.x)return;
    auto changed=sys.path(x);
    if(!sys.legal(changed))throw std::runtime_error("internal Gibbs support violation");
    if(measured){++stats.block_moves;stats.block_pattern_changes+=different_pattern(p,changed);}
    p=std::move(changed);
}

void collective_block(const System& sys,Tuple& tuple,const Level& level,int ci,
                      RNG& rng,Statistics& stats,Timer& timer){
    const bool measured=level.beta==1;
    if(measured)++stats.collective_attempts;
    const int r=1+draw(rng,R-2);const auto& old=tuple.paths[ci];auto x=old.x;
    const auto final=inverse_linear(x[r+2]);std::array<int,NC> allowed{};
    for(int c=0;c<NC;++c)for(int bit=0;bit<4;++bit)
        if(final[P(4*c+bit)/4])allowed[c]|=1<<bit;
    Nibbles y{};
    for(int c=0;c<NC;++c){timer.check();std::vector<Real> w(16,0);
        for(int b=0;b<16;++b)w[b]=sys.ddt[16*x[r-1][c]+b];
        y[c]=static_cast<unsigned char>(weighted_index(w,rng));
    }
    x[r]=linear(y);
    for(int c=0;c<NC;++c){std::vector<Real> w(16,0);
        for(int b=0;b<16;++b)if(!(b&~allowed[c]))w[b]=sys.ddt[16*x[r][c]+b];
        if(std::accumulate(w.begin(),w.end(),Real(0))==0)return;
        y[c]=static_cast<unsigned char>(weighted_index(w,rng));
    }
    x[r+1]=linear(y);if(x==old.x)return;
    auto proposed=sys.path(x);if(!sys.legal(proposed))return;
    auto scores=[&](const Trail& t){
        Real target=0,q=0;
        for(int round=r-1;round<=r+1;++round){auto out=inverse_linear(t[round+1]);
            for(int c=0;c<NC;++c){int n=sys.ddt[16*t[round][c]+out[c]];
                if(!n)throw std::runtime_error("collective reverse support failure");
                target+=std::log(Real(n)/16);
                if(round==r-1)q+=std::log(Real(n)/16);
                if(round==r){int total=0;
                    for(int b=0;b<16;++b)if(!(b&~allowed[c]))total+=sys.ddt[16*t[round][c]+b];
                    if(!total)throw std::runtime_error("collective reverse normalization failure");
                    q+=std::log(Real(n)/total);
                }
            }
        }return std::make_pair(target,q);
    };
    const auto before=scores(old.x),after=scores(x);
    if(!accept(rng,level.beta*(after.first-before.first)+before.second-after.second))return;
    if(measured){++stats.collective_moves;stats.collective_pattern_changes+=different_pattern(old,proposed);}
    tuple.paths[ci]=std::move(proposed);
}

void boundary_block(const System& sys,Tuple& tuple,const Level& level,bool prefix,
                    RNG& rng,Statistics& stats,Timer& timer){
    int c=draw(rng,NC),m=int(tuple.paths.size());bool measured=level.beta==1;
    if(measured)++stats.boundary_block_attempts;
    using Matrix=std::array<std::array<Real,16>,16>;
    std::vector<Matrix> weights(size_t(m),Matrix{});
    std::vector<Real> scores(16,0);
    for(int i=0;i<m;++i){timer.check();const auto& p=tuple.paths[i];
        Nibbles fixed=prefix?inverse_linear(p.x[2]):inverse_linear(p.x[R-1]);
        Nibbles endpoint=prefix?p.x[0]:inverse_linear(p.x[R]);
        bool other_nonzero=false;for(int j=0;j<NC;++j)if(j!=c)other_nonzero|=endpoint[j]!=0;
        for(int v=0;v<16;++v){Real total=0;
            if(!v&&!other_nonzero){scores[v]=-INF;continue;}
            for(int u=0;u<16;++u){
                int activity=p.active;uint64_t w=unsigned(sys.ddt[prefix?16*v+u:16*u+v]);
                if(!w)continue;
                if(prefix){
                    activity+=int(v!=0)-int(p.x[0][c]!=0);
                    for(int k=0;k<4;++k){int dst=P(4*c+k)/4,bit=P(4*c+k)%4;
                        int old=p.x[1][dst],value=(old&~(1<<bit))|(((u>>k)&1)<<bit);
                        activity+=int(value!=0)-int(old!=0);
                        w*=unsigned(sys.ddt[16*value+fixed[dst]]);
                    }
                }else{
                    activity+=int(u!=0)-int(p.x[R-1][c]!=0);
                    for(int k=0;k<4;++k){int src=PI(4*c+k)/4,bit=PI(4*c+k)%4;
                        int value=(fixed[src]&~(1<<bit))|(((u>>k)&1)<<bit);
                        w*=unsigned(sys.ddt[16*p.x[R-2][src]+value]);
                    }
                }
                if(activity>sys.cap)continue;
                weights[i][v][u]=heated(w,level.beta);total+=weights[i][v][u];
            }
            scores[v]+=total>0?std::log(total):-INF;
        }
    }
    int v=choose(scores,rng);bool changed=false,pattern=false;
    bool endpoint_changed=(prefix?tuple.paths[0].x[0][c]:inverse_linear(tuple.paths[0].x[R])[c])!=v;
    std::vector<Path> replacement;replacement.reserve(size_t(m));
    for(int i=0;i<m;++i){const auto& p=tuple.paths[i];
        int u=int(weighted_index(std::vector<Real>(weights[i][v].begin(),weights[i][v].end()),rng));
        auto x=p.x;
        if(prefix){x[0][c]=static_cast<unsigned char>(v);
            for(int k=0;k<4;++k){int dst=P(4*c+k)/4,bit=P(4*c+k)%4;
                x[1][dst]=static_cast<unsigned char>((x[1][dst]&~(1<<bit))|(((u>>k)&1)<<bit));}
        }else{x[R-1][c]=static_cast<unsigned char>(u);
            auto y=inverse_linear(x[R]);y[c]=static_cast<unsigned char>(v);x[R]=linear(y);}
        auto next=sys.path(x);
        if(!sys.legal(next))throw std::runtime_error("boundary Gibbs support violation");
        changed|=x!=p.x;pattern|=different_pattern(p,next);replacement.push_back(std::move(next));
    }
    if(measured){stats.boundary_block_moves+=changed;stats.boundary_block_pattern_changes+=pattern;
        stats.boundary_block_endpoint_changes+=endpoint_changed;}
    tuple.paths=std::move(replacement);
}
