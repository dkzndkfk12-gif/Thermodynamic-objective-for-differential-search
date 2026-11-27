
inline void Campaign::merge_graph_certificate(const J& job,const J& receipt){
    require(job.value("segment_graph",false),"graph certificate without graph worker provenance");
    const auto& proof=receipt.at("graph_certificate");
    const auto sid=proof.at("scope_id").get<std::string>();
    J original=proof;original.erase("scope_id");
    require(digest(original.dump())==sid,"graph certificate digest mismatch");
    auto& certs=m["graph_certificates"];if(certs.is_null())certs=J::object();
    auto& scopes=m["graph_scope_proofs"];if(scopes.is_null())scopes=J::object();
    auto& refs=m["graph_scope_refcounts"];
    if(refs.is_null()){refs=J::object();for(const auto& c:certs){const auto key=c.at("scope_id").get<std::string>();db.touch("graph_scope_refcounts",key);increment(refs,key);}}
    bool used=false;
    for(const auto& h:receipt.at("hits")){
        if(!h.contains("graph_certificate_record"))continue;
        require(h.at("graph_certificate_scope")==sid,"graph receipt scope mismatch");
        const auto& rec=h.at("graph_certificate_record");
        require(std::find(proof.at("records").begin(),proof.at("records").end(),rec)!=proof.at("records").end(),"graph endpoint outside proof");
        const auto ep=h.at("endpoint").get<std::string>();
        J mass=dyadic(integer(rec.at("numerator")),rec.at("denominator_power").get<unsigned>());
        require(compare_mass(mass,dyadic(Z(1),0))<=0&&integer(mass.at("numerator"))>0,"graph certificate probability out of range");
        if(certs.contains(ep)&&compare_mass(certs.at(ep).at("mass"),mass)>=0)continue;
        if(certs.contains(ep)){
            const auto old=certs.at(ep).at("scope_id").get<std::string>();
            auto n=count(refs,old.c_str());require(n>0,"graph proof reference count");
            db.touch("graph_scope_refcounts",old);if(n==1)db.touch("graph_scope_proofs",old);refs[old]=n-1;if(n==1){scopes.erase(old);refs.erase(old);}
        }
        db.touch("graph_scope_refcounts",sid);db.touch("graph_certificates",ep);ep_dirty.insert(ep);increment(refs,sid);used=true;
        certs[ep]={{"input",h.at("input")},{"output",h.at("output")},{"mass",mass},
                   {"scope_id",sid},{"raw_input",rec.at("input")},{"raw_output",rec.at("output")},
                   {"canonical_witness",h.at("id")},{"seconds",elapsed()},{"job",job.at("id")}};
        union_dirty_endpoints.insert(ep);unions_dirty=true;
    }
    if(used){db.touch("graph_scope_proofs",sid);scopes[sid]=std::move(original);}
}
inline void Campaign::apply_graph_lower_bounds(){

    if(!m.contains("graph_certificates"))return;
    std::map<std::string,std::size_t> index;
    for(std::size_t i=0;i<partial_cache.size();++i)index.emplace(partial_cache[i].at("endpoint").get<std::string>(),i);
    for(auto it=m.at("graph_certificates").begin();it!=m.at("graph_certificates").end();++it){
        const auto& ep=it.key();const auto& cert=it.value();
        auto idx=index.find(ep);
        if(idx==index.end()){
            partial_cache.push_back({{"endpoint",ep},{"input",cert.at("input")},{"output",cert.at("output")},
                {"component_ids",J::array()},{"complete_patterns",0},{"unfinished_patterns",0},{"mass",dyadic(Z(0),0)}});
            idx=index.emplace(ep,partial_cache.size()-1).first;
        }
        auto& group=partial_cache[idx->second];
        if(!group.contains("pattern_and_corridor_mass"))group["pattern_and_corridor_mass"]=group.at("mass");
        group["graph_certificate"]=cert;
        group["aggregation"]="max(pattern/corridor union, verified graph scope); overlap not added";
        if(compare_mass(cert.at("mass"),group.at("mass"))>0)group["mass"]=cert.at("mass");

    }
    std::sort(partial_cache.begin(),partial_cache.end(),[](const J& x,const J& y){
        int c=compare_mass(x.at("mass"),y.at("mass"));return c?c>0:x.at("endpoint")<y.at("endpoint");});
}
