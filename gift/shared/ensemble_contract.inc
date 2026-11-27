

inline unsigned ensemble_seed_max_active(const Cipher& cipher,const J& trails) {
    require(trails.is_array()&&!trails.empty()&&trails.size()<=16,
            "Ensemble must have 1..16 initial trails");
    unsigned maximum=0;
    for(const auto& trail:trails) {
        unsigned active=0;

        for(const auto& row:cipher.activity(trail.get<std::vector<std::string>>()))
            active+=unsigned(std::count(row.begin(),row.end(),'1'));
        maximum=std::max(maximum,active);
    }
    return maximum;
}
inline void validate_ensemble_seed_contract(const Cipher& cipher,const J& trails,unsigned cap) {
    const auto required=ensemble_seed_max_active(cipher,trails);
    require(cap>=required,"Ensemble active cap mismatch: assigned="+std::to_string(cap)+
            ", required_by_initial_trails="+std::to_string(required));
    std::vector<int> input,output;
    for(std::size_t i=0;i<trails.size();++i) {
        const auto states=trails.at(i).get<std::vector<std::string>>();
        std::vector<std::vector<int>> x;
        for(const auto& s:states)x.push_back(cipher.parse(s));
        require(std::any_of(x.front().begin(),x.front().end(),[](int v){return v!=0;})&&
                std::any_of(x.back().begin(),x.back().end(),[](int v){return v!=0;}),
                "Ensemble initial endpoints must be nonzero");
        if(i==0){input=x.front();output=x.back();}
        else require(x.front()==input&&x.back()==output,"Ensemble initial endpoints differ");
        for(int r=0;r<cipher.R;++r) {
            auto y=cipher.inverse(x.at(r+1));
            for(std::size_t cell=0;cell<x.at(r).size();++cell)
                require(cipher.count(cipher.kind(int(cell)),x.at(r).at(cell),y.at(cell))>0,
                        "Ensemble initial trail violates DDT: trail="+std::to_string(i)+
                        ", round="+std::to_string(r+1)+", cell="+std::to_string(cell));
        }
    }
}
