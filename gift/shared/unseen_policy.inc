
inline Z unseen_hex(const std::string& s) { Z z; require(!s.empty()&&z.set_str(s,16)==0&&z>=0,"Invalid unseen endpoint hex"); return z; }
inline std::string unseen_hex_text(const Z& z,unsigned digits) { auto s=z.get_str(16);require(s.size()<=digits,"Unseen endpoint overflow");return std::string(digits-s.size(),'0')+s; }
inline int unseen_compare(const J& a,const J& b) {
    Z an=integer(a.at("numerator")),ad=integer(a.at("denominator")),bn=integer(b.at("numerator")),bd=integer(b.at("denominator"));
    require(an>=0&&bn>=0&&ad>0&&bd>0,"Invalid unseen rational bound");Z l=an*bd,r=bn*ad;return l<r?-1:l>r?1:0;
}
inline J unseen_one() { return {{"numerator","1"},{"denominator","1"}}; }
inline J unseen_dyadic_fraction(const J& x) { return {{"numerator",x.at("numerator")},{"denominator",pow2(x.at("denominator_power").get<unsigned>()).get_str()}}; }
inline void Campaign::unseen_initialize() {
    if(unseen_initialized)return;
    const unsigned digits=cipher.cfg.value("cells",16u),bits=4*digits;
    const auto model=digest(J::array({"UNSEEN_DDT_V1",cipher.cfg}));
    if(!m.contains("unseen_mass")) {
        J u={{"revision",1},{"model_context",model},{"scope","full independent-round-key DDT model; all nonzero inputs and all outputs"},{"regions",J::object()},{"next_ticket",0},{"submitted",0},{"finished",0}};
        for(unsigned i=0;i<bits;++i) { const auto id="nz"+std::to_string(i);Z value=pow2(i),mask=pow2(i+1)-1;
            J region={{"id",id},{"input_mask",unseen_hex_text(mask,digits)},{"input_value",unseen_hex_text(value,digits)},
                      {"output_mask",std::string(digits,'0')},{"output_value",std::string(digits,'0')}};
            auto ticket=count(u,"next_ticket");u["next_ticket"]=ticket+1;
            db.touch_region(id);u["regions"][id]={{"region",region},{"state","OPEN"},{"depth",0},{"upper",unseen_one()},{"ticket",ticket},{"submitted",0},{"finished",0}};
        }
        m["unseen_mass"]=std::move(u);
    }
    const auto& u=m.at("unseen_mass");require(u.value("revision",0)==1&&u.at("model_context")==model,"Unseen ledger model/revision mismatch");
    const auto& nodes=u.at("regions");require(nodes.is_object(),"Unseen region ledger is not an object");std::set<std::string> visited;
    std::function<void(const std::string&,const J&,const J&)> check=[&](const std::string& id,const J& expected,const J& inherited){
        require(visited.insert(id).second&&nodes.contains(id),"Duplicate/cyclic/missing unseen region");const auto& node=nodes.at(id);const auto& region=node.at("region");
        require(region==expected,"Unseen region partition mismatch");require(unseen_compare(node.at("upper"),inherited)<=0,"Child unseen bound exceeds parent");
        const auto state=node.at("state").get<std::string>();
        if(state!="SPLIT") {require(state=="OPEN"||state=="PRUNED_BY_CERTIFIED_ENDPOINT_LOWER","Unknown unseen region state");
            if(state=="OPEN" && !node.value("enumeration_done",false))
                unseen_fair_index.emplace(count(node,"ticket"),id);
            return;
        }
        const auto& children=node.at("children");require(children.is_array()&&children.size()==2,"Unseen split must have two children");
        unsigned chosen=2*bits;for(unsigned k=0;k<2*bits;++k){auto mask=unseen_hex(region.at(k%2==0?"output_mask":"input_mask"));if(!mpz_tstbit(mask.get_mpz_t(),k/2)){chosen=k;break;}}
        require(chosen<2*bits,"Fully fixed unseen region cannot split");const bool output=chosen%2==0;const unsigned bit=chosen/2;
        for(unsigned value=0;value<2;++value){auto child=region;const auto cid=id+"/"+(output?"o":"i")+std::to_string(bit)+"="+std::to_string(value);require(children.at(value)==cid,"Unseen child id mismatch");child["id"]=cid;
            const char* mk=output?"output_mask":"input_mask";const char* vk=output?"output_value":"input_value";Z mask=unseen_hex(region.at(mk)),v=unseen_hex(region.at(vk));mpz_setbit(mask.get_mpz_t(),bit);if(value)mpz_setbit(v.get_mpz_t(),bit);else mpz_clrbit(v.get_mpz_t(),bit);child[mk]=unseen_hex_text(mask,digits);child[vk]=unseen_hex_text(v,digits);check(cid,child,node.at("upper"));}
    };
    for(unsigned i=0;i<bits;++i){const auto id="nz"+std::to_string(i);J region={{"id",id},{"input_mask",unseen_hex_text(Z(pow2(i+1)-1),digits)},{"input_value",unseen_hex_text(pow2(i),digits)},{"output_mask",std::string(digits,'0')},{"output_value",std::string(digits,'0')}};check(id,region,unseen_one());}
    require(visited.size()==nodes.size(),"Unreachable/overlapping unseen region in restored ledger");unseen_initialized=true;
}
inline J Campaign::unseen_job() {
    if(!a.unseen_mass || a.unseen_share<=0)return nullptr;
    unseen_initialize();auto& u=m["unseen_mass"];auto& regions=u["regions"];
    std::set<std::string> busy;for(const auto* j:running("seed"))if(j->value("unseen_mass",false))busy.insert(j->at("unseen_region").at("id").get<std::string>());
    J lower=partial_cache.empty()?J{{"numerator","0"},{"denominator","1"}}:unseen_dyadic_fraction(partial_cache.front().at("mass"));
    const bool fair=count(u,"submitted")%4==0;J* chosen=nullptr;
    auto consider=[&](J& node){
        const auto id=node.at("region").at("id").get<std::string>();
        if(node.value("state",std::string())!="OPEN"||busy.count(id))return;
        if(!lower.is_null()&&unseen_compare(node.at("upper"),lower)<=0) {db.touch_region(id);node["state"]="PRUNED_BY_CERTIFIED_ENDPOINT_LOWER";node["pruned_against"]=lower;unseen_fair_index.erase({count(node,"ticket"),id});return;}
        if(node.value("enumeration_done",false))return;
        if(!chosen)chosen=&node;else {int cmp=unseen_compare(node.at("upper"),chosen->at("upper"));if((!fair&&cmp>0)||((fair||cmp==0)&&count(node,"ticket")<count(*chosen,"ticket")))chosen=&node;}
    };
    const auto limit=std::max<std::size_t>(256,std::size_t(slots)*4);
    if(fair){auto it=unseen_fair_index.begin();for(std::size_t n=0;it!=unseen_fair_index.end()&&n<limit;++n){const auto id=(it++)->second;consider(regions.at(id));if(chosen)break;}}
    else if(!regions.empty()){
        auto& objects=regions.get_ref<J::object_t&>();auto it=objects.upper_bound(u.value("priority_cursor",std::string()));if(it==objects.end())it=objects.begin();
        for(std::size_t n=0;n<std::min(limit,regions.size());++n){u["priority_cursor"]=it->first;consider(it->second);if(++it==objects.end())it=objects.begin();}
        if(!chosen){auto f=unseen_fair_index.begin();for(std::size_t n=0;f!=unseen_fair_index.end()&&n<limit;++n){const auto id=(f++)->second;consider(regions.at(id));if(chosen)break;}}
    }
    if(!chosen)return nullptr;
    J j={{"kind","seed"},{"unseen_mass",true},{"unseen_region",chosen->at("region")},{"unseen_bound_width",a.unseen_bound_width},
         {"seconds",a.pattern_seconds},{"keep",a.keep},{"evaluation",false}};
    if(chosen->contains("resume"))j["unseen_resume"]=chosen->at("resume");
    j["unseen_context"]=digest(J::array({"UNSEEN_DDT_V1",cipher.cfg,chosen->at("region")}));
    return j;
}
inline void Campaign::unseen_completed(const J& job,const J& result,double cpu,std::size_t created) {
    unseen_initialize();auto& u=m["unseen_mass"];const auto id=job.at("unseen_region").at("id").get<std::string>();
    db.touch_region(id);auto& node=u["regions"].at(id);require(node.at("region")==job.at("unseen_region"),"Unseen region receipt context changed");
    const bool has_receipt=result.contains("unseen_diagnostics");
    require(has_receipt||result.value("status",std::string())!="COMPLETE","Missing complete unseen receipt");
    J receipt=has_receipt?result.at("unseen_diagnostics"):J();J upper;
    if(has_receipt){
        require(receipt.at("region_id")==id,"Unseen worker region mismatch");
        require(receipt.at("controller_context")==job.at("unseen_context"),"Unseen worker model context mismatch");
        upper={{"numerator",receipt.at("upper_numerator")},{"denominator",receipt.at("upper_denominator")}};
        require(unseen_compare(upper,unseen_one())<=0,"Unseen probability bound exceeds one");
        (void)receipt.value("bound_complete",false);(void)receipt.value("solver_exhausted",false);(void)receipt.value("reason",std::string());
        for(const char* k:{"bound_seconds","csp_seconds"}){double v=number(receipt,k);require(std::isfinite(v)&&v>=0,"Invalid unseen receipt timing");}
        if(receipt.contains("resume"))require(receipt.at("resume").is_object()||receipt.at("resume").is_null(),"Invalid unseen resume");
    }
    increment(u,"finished");increment(node,"finished");increment(u,"new_families",created);increment(node,"new_families",created);
    u["cpu_seconds"]=number(u,"cpu_seconds")+cpu;node["cpu_seconds"]=number(node,"cpu_seconds")+cpu;
    unseen_fair_index.erase({count(node,"ticket"),id});node["ticket"]=count(u,"next_ticket");increment(u,"next_ticket");unseen_fair_index.emplace(count(node,"ticket"),id);
    if(!has_receipt)return;

    if(receipt.value("bound_complete",false) && unseen_compare(upper,node.at("upper"))<0){node["upper"]=upper;increment(u,"upper_improvements");}
    node["last_bound_complete"]=receipt.value("bound_complete",false);node["last_reason"]=receipt.value("reason",std::string());
    for(const char* k:{"bound_seconds","csp_seconds"})u[k]=number(u,k)+number(receipt,k);
    increment(u,"solver_nodes",count(receipt,"nodes"));
    node["solver_exhausted"]=receipt.value("solver_exhausted",false);
    node["total_trails_seen"]=count(receipt,"total_trails_seen");
    if(receipt.contains("resume")&&!receipt.at("resume").is_null())node["resume"]=receipt.at("resume");
    if(count(node,"finished")<a.unseen_split_visits && !node.value("solver_exhausted",false))return;
    const unsigned digits=cipher.cfg.value("cells",16u),bits=digits*4;
    const auto old=node.at("region");unsigned split=2*bits;

    for(unsigned k=0;k<2*bits;++k){bool output=(k%2==0);unsigned bit=k/2;auto mask=unseen_hex(old.at(output?"output_mask":"input_mask"));
        if(!mpz_tstbit(mask.get_mpz_t(),bit)){split=k;break;}}
    if(split==2*bits){node["enumeration_done"]=node.value("solver_exhausted",false);if(node.value("enumeration_done",false))unseen_fair_index.erase({count(node,"ticket"),id});return;}
    const bool output=split%2==0;const unsigned bit=split/2;const char* maskkey=output?"output_mask":"input_mask";const char* valuekey=output?"output_value":"input_value";
    J children=J::array();const auto inherited=node.at("upper");const auto depth=count(node,"depth");
    for(unsigned value=0;value<2;++value){auto region=old;auto child=id+"/"+(output?"o":"i")+std::to_string(bit)+"="+std::to_string(value);
        region["id"]=child;Z mask=unseen_hex(old.at(maskkey)),v=unseen_hex(old.at(valuekey));mpz_setbit(mask.get_mpz_t(),bit);if(value)mpz_setbit(v.get_mpz_t(),bit);else mpz_clrbit(v.get_mpz_t(),bit);
        region[maskkey]=unseen_hex_text(mask,digits);region[valuekey]=unseen_hex_text(v,digits);
        auto ticket=count(u,"next_ticket");increment(u,"next_ticket");
        db.touch_region(child);u["regions"][child]={{"region",region},{"parent",id},{"depth",depth+1},{"state","OPEN"},{"upper",inherited},{"ticket",ticket},{"submitted",0},{"finished",0}};children.push_back(child);unseen_fair_index.emplace(ticket,child);
    }

    auto& parent=u["regions"].at(id);unseen_fair_index.erase({count(parent,"ticket"),id});parent["state"]="SPLIT";parent["children"]=children;parent.erase("resume");increment(u,"splits");
}
inline J Campaign::unseen_status() {
    if(!m.contains("unseen_mass"))return {{"enabled",a.unseen_mass},{"scope","full DDT; not initialized"}};
    unseen_initialize();
    const auto& u=m.at("unseen_mass");J maximum={{"numerator","0"},{"denominator","1"}};std::uint64_t open=0,pruned=0,split=0;
    for(const auto& node:u.at("regions")){const auto state=node.value("state",std::string());if(state=="SPLIT"){++split;continue;}if(state=="OPEN")++open;else ++pruned;
        if(unseen_compare(node.at("upper"),maximum)>0)maximum=node.at("upper");}
    J lower=partial_cache.empty()?J{{"numerator","0"},{"denominator","1"}}:unseen_dyadic_fraction(partial_cache.front().at("mass"));
    if(!lower.is_null()&&unseen_compare(lower,maximum)>0)maximum=lower;
    J displayed_F=integer(maximum.at("numerator"))>0?J(log2z(integer(maximum.at("denominator")))-log2z(integer(maximum.at("numerator")))):J();

    return {{"enabled",a.unseen_mass},{"discovery_admission_share",a.unseen_share},{"bound_width",a.unseen_bound_width},{"split_after_visits",a.unseen_split_visits},
        {"submitted",count(u,"submitted")},{"finished",count(u,"finished")},{"open_regions",open},{"pruned_regions",pruned},{"split_regions",split},
        {"global_upper",maximum},{"global_upper_F_display_only",displayed_F},{"incumbent_lower",lower},{"priority_policy","one fourth FIFO; others highest bound within bounded cyclic ledger page"},{"global_upper_scope","all nonzero inputs and all outputs; full DDT model"},{"new_families",count(u,"new_families")},
        {"upper_improvements",count(u,"upper_improvements")},{"cpu_seconds",number(u,"cpu_seconds")},{"bound_seconds",number(u,"bound_seconds")},{"csp_seconds",number(u,"csp_seconds")},
        {"ledger_location","state.sqlite3 metadata unseen_mass; includes resumable regions"},{"upper_is_achieved_probability",false}};
}
