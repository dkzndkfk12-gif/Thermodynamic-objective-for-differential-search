
std::map<std::string,J> meta_parts_cache;
std::set<std::string> small_meta_ids;
using MetaPath=std::vector<std::string>;
std::map<MetaPath,std::set<std::string>> meta_touched;
std::map<MetaPath,std::size_t> group_sizes;
bool meta_parts_valid=false;
std::uint64_t meta_rows_written=0,meta_bytes_written=0,large_entries_examined=0;
void touch(const std::string& group,const std::string& child){meta_touched[{group}].insert(child);}
void touch_region(const std::string& child){meta_touched[{"unseen_mass","regions"}].insert(child);}
static bool tracked(const std::string& top){
    return top=="graph_certificates"||top=="graph_scope_proofs"||top=="graph_scope_refcounts"||
        top=="endpoint_roles"||top=="mass_condition_slots"||top=="segment_slots";
}
void put_state_impl(const J& value,bool audit){
    require(value.is_object(),"Campaign metadata must be an object");
    const bool full=audit||!meta_parts_valid;
    if(!meta_parts_valid){meta_parts_cache=load_raw("meta_parts");group_sizes.clear();small_meta_ids.clear();}
    std::set<std::string> all_seen,next_small;std::set<MetaPath> groups_now;
    auto record=[&](const MetaPath& path,const J& v,bool large){
        const auto id=J(path).dump();
        if(full)all_seen.insert(id);
        if(!large)next_small.insert(id);else ++large_entries_examined;
        auto old=meta_parts_cache.find(id);
        if(old!=meta_parts_cache.end()&&old->second==v)return;
        put_raw("meta_parts",id,v);meta_parts_cache[id]=v;++meta_rows_written;
    };
    auto remove=[&](const std::string& id){if(meta_parts_cache.erase(id)){erase("meta_parts",id);++meta_rows_written;}};
    auto big=[&](MetaPath path,const J& obj) {
        groups_now.insert(path);auto old=group_sizes.find(path);
        const auto mutations=meta_touched.find(path);
        auto sub=path;sub.emplace_back();

        bool reconcile=full||old==group_sizes.end();
        if(!reconcile){
            auto predicted=old->second;
            if(mutations!=meta_touched.end())for(const auto& k:mutations->second){
                sub.back()=k;const bool had=meta_parts_cache.count(J(sub).dump())!=0,has=obj.contains(k);
                if(!had&&has)++predicted;else if(had&&!has)--predicted;
            }
            reconcile=predicted!=obj.size();
        }
        if(reconcile){
            if(!full&&old!=group_sizes.end()) {

                for(auto it=meta_parts_cache.begin();it!=meta_parts_cache.end();){
                    const auto key=J::parse(it->first).get<MetaPath>();const auto id=(it++)->first;
                    if(key.size()==sub.size()&&std::equal(path.begin(),path.end(),key.begin())&&!obj.contains(key.back()))remove(id);
                }
            }
            for(auto it=obj.begin();it!=obj.end();++it){sub.back()=it.key();record(sub,it.value(),true);}
        } else if(mutations!=meta_touched.end())for(const auto& k:mutations->second){
            sub.back()=k;if(obj.contains(k))record(sub,obj.at(k),true);else remove(J(sub).dump());
        }
        group_sizes[path]=obj.size();
        if(obj.empty())record(path,obj,false);
    };
    for(auto it=value.begin();it!=value.end();++it){
        const auto& v=it.value();const auto& top=it.key();
        if(tracked(top)&&v.is_object())big({top},v);
        else if(v.is_object()&&!v.empty()){
            for(auto ch=v.begin();ch!=v.end();++ch){
                if(top=="unseen_mass"&&ch.key()=="regions"&&ch.value().is_object())big({top,ch.key()},ch.value());
                else record({top,ch.key()},ch.value(),false);
            }
        }else record({top},v,false);
    }
    if(full){
        for(auto it=meta_parts_cache.begin();it!=meta_parts_cache.end();){const auto id=(it++)->first;if(!all_seen.count(id))remove(id);}
    }else{
        for(const auto& id:small_meta_ids)if(!next_small.count(id))remove(id);
        for(const auto& kv:group_sizes)if(!groups_now.count(kv.first)){
            for(auto it=meta_parts_cache.begin();it!=meta_parts_cache.end();){const auto id=(it++)->first;
                auto k=J::parse(id).get<MetaPath>();if(k.size()==kv.first.size()+1&&std::equal(kv.first.begin(),kv.first.end(),k.begin()))remove(id);}
        }
    }
    for(auto it=group_sizes.begin();it!=group_sizes.end();)if(!groups_now.count(it->first))it=group_sizes.erase(it);else ++it;
    small_meta_ids=std::move(next_small);meta_touched.clear();meta_parts_valid=true;
    put_raw("meta","state",J{{"metadata_storage","thermo_parts_v2"},{"elapsed",value.value("elapsed",J())},
        {"closed",value.value("closed",J())},{"counter",value.value("counter",J())}});
}
void put_campaign_state(const J& value){put_state_impl(value,false);}
void put(const std::string& table,const std::string& id,const J& data){
    if(table=="meta"&&id=="state")put_state_impl(data,true);else put_raw(table,id,data);
}
std::map<std::string,J> load(const std::string& table){
    auto result=load_raw(table);
    if(table!="meta"||!result.count("state"))return result;
    const auto tag=result.at("state").value("metadata_storage",std::string());
    if(tag!="thermo_parts_v1"&&tag!="thermo_parts_v2"){require(tag.empty(),"Unsupported campaign metadata format");return result;}
    auto parts=load_raw("meta_parts");J state=J::object();std::set<MetaPath> leaves,branches;
    for(const auto& row:parts){auto k=J::parse(row.first).get<MetaPath>();
        require(!k.empty()&&k.size()<=(tag=="thermo_parts_v1"?2u:3u)&&!branches.count(k),"Conflicting metadata part");
        J* cur=&state;MetaPath prefix;
        for(std::size_t i=0;i+1<k.size();++i){prefix.push_back(k[i]);require(!leaves.count(prefix),"Conflicting metadata shape");
            branches.insert(prefix);if(!cur->contains(k[i]))(*cur)[k[i]]=J::object();cur=&cur->at(k[i]);require(cur->is_object(),"Invalid metadata child");}
        require(leaves.insert(k).second&&!cur->contains(k.back()),"Duplicate metadata part");(*cur)[k.back()]=row.second;
    }
    result["state"]=std::move(state);return result;
}
