#pragma once
#include "archive.hpp"
#include "result_tables.hpp"

namespace craft {
                                                                                     
inline J save_shared_checkpoint(const Archive& archive,const std::filesystem::path& directory,
        uint64_t generation,int requested_workers,const std::function<void(const J&)>& heartbeat,
        const std::function<void()>& check_writing={}){
    require(requested_workers>0,"checkpoint needs at least one worker");
    size_t total=archive.frozen_family_count(),workers=std::max<size_t>(1,std::min<size_t>(size_t(requested_workers),std::max<size_t>(1,total)));
    MessageRegistry registry;std::atomic<size_t> next{0},saved{0},finished{0};std::atomic<bool> failed{false};std::mutex error_mutex;std::exception_ptr error;
    std::vector<std::thread> threads;J family_files=J::array(),message_shards=J::array();std::vector<std::string> families,data,index;
    for(size_t i=0;i<workers;++i){auto suffix=std::to_string(generation)+"_part"+std::to_string(i);families.push_back("state_families_"+suffix+".jsonl");data.push_back("messages_"+suffix+".bin.zst");index.push_back("messages_"+suffix+".index.jsonl");family_files.push_back(families.back());message_shards.push_back({{"data_file",data.back()},{"index_file",index.back()}});}
    auto join=[&]{for(auto& thread:threads)if(thread.joinable())thread.join();};
    try{
        for(size_t i=0;i<workers;++i)threads.emplace_back([&,i]{
            try{
                auto allow=[&]{
                    if(failed.load(std::memory_order_relaxed))throw std::runtime_error("another checkpoint shard stopped");
                    if(check_writing)check_writing();
                };
                MessageShardWriter messages(registry,directory/data[i],directory/index[i],allow);TableSinkScope scope(&messages);
                auto final=directory/families[i],tmp=std::filesystem::path(final.string()+".tmp");std::ofstream out(tmp);out.exceptions(std::ios::failbit|std::ios::badbit);
                while(!failed.load(std::memory_order_relaxed)){size_t id=next.fetch_add(1);if(id>=total)break;allow();out<<archive.frozen_checkpoint_record(id).dump()<<'\n';++saved;}
                out.flush();out.close();durable_file(tmp);messages.finish();std::filesystem::rename(tmp,final);
            }catch(...){std::lock_guard<std::mutex> lock(error_mutex);if(!error)error=std::current_exception();failed=true;}
            ++finished;
        });
        while(finished.load()!=workers){heartbeat(J{{"phase","checkpoint"},{"saved_families",saved.load()},{"total_families",total},{"saving_workers",workers-finished.load()},{"message_references",registry.references.load()},{"unique_messages",registry.unique.load()},{"written_messages",registry.written.load()},{"compressed_message_bytes",registry.bytes.load()}});std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        join();
    }catch(...){failed=true;join();throw;}
    if(error)std::rethrow_exception(error);
    require(saved==total&&registry.written==registry.unique,"incomplete checkpoint save");
    heartbeat(J{{"phase","checkpoint_complete"},{"saved_families",saved.load()},{"total_families",total},{"saving_workers",0},{"message_references",registry.references.load()},{"unique_messages",registry.unique.load()},{"written_messages",registry.written.load()},{"compressed_message_bytes",registry.bytes.load()}});
    return {{"checkpoint_format","shared-messages-zstd-v3"},{"family_files",family_files},{"family_count",total},{"message_shards",message_shards},{"unique_messages",registry.unique.load()},{"message_references",registry.references.load()},{"compressed_message_bytes",registry.bytes.load()}};
}
inline size_t restore_shared_checkpoint(Archive& archive,const std::filesystem::path& directory,const J& manifest){
    MessageStoreReader messages(directory,manifest);TableSourceScope scope(&messages);size_t count=0;
    for(const auto& name:manifest.at("family_files")){std::ifstream in(directory/safe_file(name));require(bool(in),"missing checkpoint family shard");std::string line;while(std::getline(in,line)){auto q=J::parse(line);archive.restore(J{{"schema","craft-mass-v1"},{"rounds",archive.rounds},{"families",J::array({std::move(q)})}});++count;}require(!in.bad(),"family shard read error");}
    require(count==manifest.at("family_count").get<size_t>(),"checkpoint family count mismatch");return count;
}
                                                                           
                                                                            
inline constexpr char staging_marker[]=".craft-wall-staging-v1";
inline void discard_uncommitted_staging(const std::filesystem::path& root,const J& manifest){
    namespace fs=std::filesystem;
    std::set<std::string> committed;
    if(manifest.contains("family_files")){
        for(const auto& name:manifest.at("family_files"))committed.insert(safe_file(name));
        for(const auto& shard:manifest.at("message_shards")){
            committed.insert(safe_file(shard.at("data_file")));committed.insert(safe_file(shard.at("index_file")));
        }
    }
    for(const auto& it:fs::directory_iterator(root)){
        auto staging=it.path();
        auto prefix=staging.filename().string();
        if(prefix.rfind("frontiers_",0)!=0||!fs::is_directory(it.symlink_status()))continue;
        std::ifstream marker(staging/staging_marker);std::string tag;
        if(!std::getline(marker,tag)||tag!="craft-wall-staging-v1")continue;
        for(const auto& entry:fs::directory_iterator(staging)){
            if(!fs::is_regular_file(entry.symlink_status()))continue;
            auto name=entry.path().filename().string();
            if(name==staging_marker)continue;
            for(const auto& alias:{name,prefix+"_"+name}){
                if(committed.count(alias))continue;
                auto file=root/alias;
                if(fs::exists(file)&&fs::equivalent(entry.path(),file))fs::remove(file);
            }
        }
        fs::remove_all(staging);
    }
}
                                                                        
                                                                          
inline void restore_compact_checkpoint(Archive& archive,const std::filesystem::path& directory,const J& manifest){
    auto file=directory/safe_file(manifest.at("family_details_file"));
    size_t count=0;
    auto restore_row=[&](const std::string& line){
        const auto row=J::parse(line);
        J family={{"witness",row.value("initial_witness",row.at("witness"))},
                  {"feedback_witness",row.at("witness")},{"source",row.at("source")},
                  {"first_elapsed",row.at("first_elapsed")},{"cpu_seconds",row.at("cpu_seconds")},
                  {"common_lower",row.at("common_subset_lower_bound")},{"lower",row.at("lower_bound")},
                  {"evaluation_gain",row.at("evaluation_gain")},{"complete",row.at("complete_for_this_pattern")}};
        for(const char* key:{"first_resume_generation","completed_resume_generation"})if(row.contains(key))family[key]=row.at(key);
        if(row.contains("screen"))family["screen"]=row.at("screen");
        family["completed_elapsed"]=row.value("completed_elapsed",0.0);
        if(family.at("complete").get<bool>())family["counter"]=row.at("completed_counter");
        else family["recovery_certified_floor"]=row.at("lower_bound");
        archive.restore(J{{"schema","craft-mass-v1"},{"rounds",archive.rounds},{"families",J::array({std::move(family)})}});
        ++count;
    };
    auto format=manifest.value("checkpoint_format",std::string("certified-family-results-v1"));
    if(format==compact_gzip_format){
        require(std::filesystem::file_size(file)==manifest.at("family_details_bytes").get<uintmax_t>(),
                "compressed family table size mismatch");
        read_gzip_table(file,restore_row);
    }else{
        require(format=="certified-family-results-v1","unknown family result checkpoint format");
        std::ifstream in(file);
        require(bool(in),"missing compact checkpoint");
        std::string line;
        while(std::getline(in,line))restore_row(line);
        require(!in.bad(),"compact checkpoint read failed");
    }
    require(count==manifest.at("family_count").get<size_t>()&&archive.frozen_family_count()==count,
            "compact checkpoint family count mismatch");
    require(archive.completed.load()==manifest.at("complete_families").get<size_t>(),"compact checkpoint completion mismatch");
}
}                   
