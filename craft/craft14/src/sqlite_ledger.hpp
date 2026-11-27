#pragma once
#include "archive.hpp"
#include <filesystem>
#include <sqlite3.h>
#include <zlib.h>

namespace craft {
inline constexpr char ledger_format[]="craft-sqlite-v1";

class Ledger {
    sqlite3* db=nullptr;
    sqlite3_stmt* put_case=nullptr;
    sqlite3_stmt* put_meta=nullptr;
    std::unordered_map<const Model*,sqlite3_int64> pattern_ids;
    size_t durable_families=0;

    void check(int result,const char* action)const{
        if(result!=SQLITE_OK)throw std::runtime_error(std::string("SQLite ")+action+": "+sqlite3_errmsg(db));
    }
    sqlite3_stmt* prepare(const char* sql)const{
        sqlite3_stmt* statement=nullptr;
        check(sqlite3_prepare_v2(db,sql,-1,&statement,nullptr),"prepare");
        return statement;
    }
    void write(sqlite3_stmt* statement)const{
        int result=sqlite3_step(statement);
        std::string error=sqlite3_errmsg(db);
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        if(result!=SQLITE_DONE)throw std::runtime_error("SQLite write: "+error);
    }
    static std::vector<uint8_t> encode(const J& row){
        auto raw=J::to_cbor(row);
        require(raw.size()<=UINT32_MAX,"SQLite record exceeds codec range");
        uLongf size=compressBound(uLong(raw.size()));
        std::vector<uint8_t> blob(4+size);
        for(unsigned k=0;k<4;++k)blob[k]=uint8_t(raw.size()>>(8*k));
        require(compress2(blob.data()+4,&size,raw.data(),uLong(raw.size()),1)==Z_OK,"compress SQLite record failed");
        blob.resize(4+size);
        return blob;
    }
    static J decode(const void* data,int size){
        require(size>=4,"short SQLite family record");
        auto bytes=static_cast<const uint8_t*>(data);
        uint32_t length=0;
        for(unsigned k=0;k<4;++k)length|=uint32_t(bytes[k])<<(8*k);
        std::vector<uint8_t> raw(length);
        uLongf actual=length;
        require(uncompress(raw.data(),&actual,bytes+4,uLong(size-4))==Z_OK&&actual==length,
                "damaged SQLite family record");
        return J::from_cbor(raw);
    }
    sqlite3_int64 pattern(const std::shared_ptr<const Model>& model){
        auto found=pattern_ids.find(model.get());
        if(found!=pattern_ids.end())return found->second;
        auto select=prepare("SELECT id FROM patterns WHERE key=?");
        check(sqlite3_bind_text(select,1,model->id.data(),int(model->id.size()),SQLITE_TRANSIENT),"bind pattern key");
        int result=sqlite3_step(select);
        sqlite3_int64 id=0;
        if(result==SQLITE_ROW){
            id=sqlite3_column_int64(select,0);
            sqlite3_finalize(select);
        }else{
            sqlite3_finalize(select);
            require(result==SQLITE_DONE,"SQLite pattern lookup failed");
            auto masks=J::to_cbor(model->pattern);
            auto insert=prepare("INSERT INTO patterns(key,masks) VALUES(?,?)");
            sqlite3_bind_text(insert,1,model->id.data(),int(model->id.size()),SQLITE_TRANSIENT);
            sqlite3_bind_blob(insert,2,masks.data(),int(masks.size()),SQLITE_TRANSIENT);
            try{write(insert);}catch(...){sqlite3_finalize(insert);throw;}
            sqlite3_finalize(insert);
            id=sqlite3_last_insert_rowid(db);
        }
        pattern_ids.emplace(model.get(),id);
        return id;
    }
    void meta(const J& state){
        auto text=state.dump();
        check(sqlite3_bind_text(put_meta,1,text.data(),int(text.size()),SQLITE_TRANSIENT),"bind metadata");
        write(put_meta);
    }
public:
    explicit Ledger(const std::filesystem::path& path){
        int result=sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr);
        if(result!=SQLITE_OK){
            std::string error=db?sqlite3_errmsg(db):"open failed";
            if(db)sqlite3_close(db);
            db=nullptr;
            throw std::runtime_error("SQLite open: "+error);
        }
        try{
            check(sqlite3_busy_timeout(db,5000),"busy timeout");
            auto quick=prepare("PRAGMA quick_check(1)");
            bool valid=sqlite3_step(quick)==SQLITE_ROW;
            auto text=valid?sqlite3_column_text(quick,0):nullptr;
            valid=valid&&text&&std::string(reinterpret_cast<const char*>(text))=="ok";
            sqlite3_finalize(quick);
            require(valid,"SQLite ledger integrity check failed");
            exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
            exec("CREATE TABLE IF NOT EXISTS patterns(id INTEGER PRIMARY KEY,key TEXT NOT NULL UNIQUE,masks BLOB NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS cases(id INTEGER PRIMARY KEY,pattern_id INTEGER NOT NULL,data BLOB NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS meta(id TEXT PRIMARY KEY,data TEXT NOT NULL);");
            put_case=prepare("INSERT INTO cases(id,pattern_id,data) VALUES(?,?,?) "
                             "ON CONFLICT(id) DO UPDATE SET pattern_id=excluded.pattern_id,data=excluded.data");
            put_meta=prepare("INSERT INTO meta(id,data) VALUES('state',?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");
            auto count=prepare("SELECT coalesce(max(id)+1,0) FROM cases");
            require(sqlite3_step(count)==SQLITE_ROW,"SQLite family count read failed");
            durable_families=size_t(sqlite3_column_int64(count,0));
            sqlite3_finalize(count);
        }catch(...){close();throw;}
    }
    Ledger(const Ledger&)=delete;
    Ledger& operator=(const Ledger&)=delete;
    ~Ledger(){close();}
    void close(){
        if(put_case){sqlite3_finalize(put_case);put_case=nullptr;}
        if(put_meta){sqlite3_finalize(put_meta);put_meta=nullptr;}
        if(db){sqlite3_close(db);db=nullptr;}
    }
    void exec(const char* sql)const{
        char* detail=nullptr;
        int result=sqlite3_exec(db,sql,nullptr,nullptr,&detail);
        if(result!=SQLITE_OK){
            std::string error=detail?detail:sqlite3_errmsg(db);
            sqlite3_free(detail);
            throw std::runtime_error("SQLite: "+error);
        }
    }
    J state()const{
        auto statement=prepare("SELECT data FROM meta WHERE id='state'");
        int result=sqlite3_step(statement);
        J value;
        try{
            if(result==SQLITE_ROW)value=J::parse(reinterpret_cast<const char*>(sqlite3_column_text(statement,0)));
            else require(result==SQLITE_DONE,"SQLite metadata read failed");
        }catch(...){sqlite3_finalize(statement);throw;}
        sqlite3_finalize(statement);
        return value;
    }
    size_t stored_families()const{return durable_families;}
    size_t flush(Archive& archive,J metadata,Clock::time_point until=Clock::time_point::max()){
        std::vector<size_t> written;
        auto previous=durable_families;
        exec("BEGIN IMMEDIATE");
        try{
            while(Clock::now()<until){
                auto snapshot=archive.storage_snapshot();
                if(!snapshot)break;
                written.push_back(snapshot->id);
                require(snapshot->id<=durable_families,"out-of-order new SQLite family");
                auto id=pattern(snapshot->model);
                auto bytes=encode(snapshot->record());
                check(sqlite3_bind_int64(put_case,1,sqlite3_int64(snapshot->id)),"bind family id");
                check(sqlite3_bind_int64(put_case,2,id),"bind pattern id");
                check(sqlite3_bind_blob64(put_case,3,bytes.data(),sqlite3_uint64(bytes.size()),SQLITE_TRANSIENT),"bind family record");
                write(put_case);
                if(snapshot->id==durable_families)++durable_families;
            }
            metadata["durable_families"]=durable_families;
            meta(metadata);
            exec("COMMIT");
        }catch(...){
            sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
            durable_families=previous;
            pattern_ids.clear();
            archive.storage_failed(written);
            throw;
        }
        return written.size();
    }
    template<class Read> void families(Read read)const{
        std::unordered_map<sqlite3_int64,Pattern> patterns;
        auto statement=prepare("SELECT id,masks FROM patterns");
        try{
            int result;
            while((result=sqlite3_step(statement))==SQLITE_ROW){
                auto data=static_cast<const uint8_t*>(sqlite3_column_blob(statement,1));
                int size=sqlite3_column_bytes(statement,1);
                patterns.emplace(sqlite3_column_int64(statement,0),J::from_cbor(data,data+size).get<Pattern>());
            }
            require(result==SQLITE_DONE,"SQLite pattern read failed");
        }catch(...){sqlite3_finalize(statement);throw;}
        sqlite3_finalize(statement);
        statement=prepare("SELECT id,pattern_id,data FROM cases ORDER BY id");
        size_t count=0;
        try{
            int result;
            while((result=sqlite3_step(statement))==SQLITE_ROW){
                require(sqlite3_column_int64(statement,0)==sqlite3_int64(count),"SQLite family id gap");
                auto row=decode(sqlite3_column_blob(statement,2),sqlite3_column_bytes(statement,2));
                read(count,patterns.at(sqlite3_column_int64(statement,1)),row);
                ++count;
            }
            require(result==SQLITE_DONE&&count==durable_families,"SQLite family read failed");
        }catch(...){sqlite3_finalize(statement);throw;}
        sqlite3_finalize(statement);
    }
    J restore(Archive& archive,uint64_t seed){
        auto metadata=state();
        require(!metadata.is_null()&&metadata.at("checkpoint_format")==ledger_format,"incompatible SQLite ledger");
        require(metadata.at("rounds")==archive.rounds&&metadata.at("seed")==seed,"SQLite run identity changed");
        families([&](size_t,const Pattern& pattern,const J& row){
            archive.restore(J{{"schema","craft-mass-v1"},{"rounds",archive.rounds},
                              {"families",J::array({legacy_family_record(row,pattern)})}});
        });
        if(metadata.value("clean_stop",false)){
            auto status=archive.status();
            require(status.at("families")==metadata.at("family_count")&&status.at("complete_families")==metadata.at("complete_families"),
                    "SQLite final count mismatch");
        }
        if(metadata.contains("guidance_state"))archive.restore_guidance(metadata.at("guidance_state"));
        archive.storage_loaded();
        return metadata;
    }
};
}
