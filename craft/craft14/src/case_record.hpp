#pragma once
#include "model.hpp"
#include "mass_screen.hpp"
#include <optional>
#include <zlib.h>

namespace craft {
class CompressedRecord {
    std::vector<uint8_t> bytes;
public:
    explicit CompressedRecord(const J& record){
        auto raw=J::to_cbor(record);
        require(raw.size()<=UINT32_MAX,"compressed record exceeds codec range");
        uLongf size=compressBound(uLong(raw.size()));
        bytes.resize(4+size);
        for(unsigned k=0;k<4;++k)bytes[k]=uint8_t(raw.size()>>(8*k));
        require(compress2(bytes.data()+4,&size,raw.data(),uLong(raw.size()),1)==Z_OK,"compress record failed");
        bytes.resize(4+size);
    }
    J json()const{
        uint32_t length=0;
        for(unsigned k=0;k<4;++k)length|=uint32_t(bytes[k])<<(8*k);
        std::vector<uint8_t> raw(length);
        uLongf actual=length;
        require(uncompress(raw.data(),&actual,bytes.data()+4,uLong(bytes.size()-4))==Z_OK&&actual==length,
                "damaged compressed record");
        return J::from_cbor(raw);
    }
};
inline J packed_trail(const Trail& trail){
    std::vector<uint8_t> bytes;
    bool low=false;
    for(const auto& state:trail)for(auto value:state){
        if(!value)continue;
        if(low)bytes.back()|=uint8_t(value<<4);
        else bytes.push_back(value);
        low=!low;
    }
    return J::binary(std::move(bytes));
}

inline Trail unpacked_trail(const J& packed,const Pattern& pattern){
    const auto& bytes=packed.get_binary();
    size_t size=0;
    for(auto mask:pattern)size+=pop(mask);
    require(bytes.size()==(size+1)/2,"bad stored CRAFT trail size");
    Trail trail(pattern.size());
    size_t offset=0;
    for(size_t r=0;r<pattern.size();++r)for(int c=0;c<16;++c){
        if(!(pattern[r]&(1u<<c)))continue;
        auto value=uint8_t((bytes[offset/2]>>(4*(offset%2)))&15);
        require(value!=0,"zero value in stored active CRAFT cell");
        trail[r][c]=value;
        ++offset;
    }
    if(size%2)require((bytes.back()&0xf0)==0,"bad stored CRAFT trail padding");
    return trail;
}

inline J stored_dyadic(const Dyadic& value){return J::array({value.n.get_str(),value.e});}
inline J restored_dyadic(const J& value){
    require(value.is_array()&&value.size()==2,"bad stored dyadic");
    return Dyadic(mpz_class(value.at(0).get<std::string>()),value.at(1).get<unsigned>()).json();
}

struct FamilySnapshot {
    size_t id;
    std::shared_ptr<const Model> model;
    Values witness;
    Values initial_witness;
    Dyadic lower,common,gain;
    ScreenEvidence screen;
    std::shared_ptr<const CompressedRecord> completed_counter;
    std::string source;
    double first_elapsed,cpu,completed_elapsed;
    uint64_t first_generation,completed_generation,nodes;
    J record()const{
        J row={{"trail",packed_trail(model->decode(witness))},{"source",source},
               {"first_elapsed",first_elapsed},{"lower",stored_dyadic(lower)}};
        if(first_generation)row["first_resume_generation"]=first_generation;
        if(!initial_witness.empty())row["initial_trail"]=packed_trail(model->decode(initial_witness));
        if(cpu>0)row["cpu_seconds"]=cpu;
        if(nodes)row["nodes"]=nodes;
        if(common.n>0)row["common_lower"]=stored_dyadic(common);
        if(gain.n>0)row["evaluation_gain"]=stored_dyadic(gain);
        if(screen.paths||screen.cpu>0)row["screen"]=screen.json();
        if(completed_counter){
            row["completed_counter"]=completed_counter->json();
            row["completed_elapsed"]=completed_elapsed;
            if(completed_generation)row["completed_resume_generation"]=completed_generation;
        }
        return row;
    }
};

inline J legacy_family_record(const J& row,const Pattern& pattern){
    auto trail=unpacked_trail(row.at("trail"),pattern);
    J family={{"witness",trail_json(trail)},{"source",row.at("source")},
              {"first_elapsed",row.at("first_elapsed")},{"cpu_seconds",row.value("cpu_seconds",0.0)},
              {"nodes",row.value("nodes",uint64_t(0))},{"lower",restored_dyadic(row.at("lower"))},
              {"complete",row.contains("completed_counter")}};
    if(row.contains("initial_trail")){
        family["feedback_witness"]=family.at("witness");
        family["witness"]=trail_json(unpacked_trail(row.at("initial_trail"),pattern));
    }
    for(const char* key:{"screen","first_resume_generation","completed_resume_generation","completed_elapsed"})
        if(row.contains(key))family[key]=row.at(key);
    if(row.contains("common_lower"))family["common_lower"]=restored_dyadic(row.at("common_lower"));
    if(row.contains("evaluation_gain"))family["evaluation_gain"]=restored_dyadic(row.at("evaluation_gain"));
    if(row.contains("completed_counter")){
        family["counter"]=row.at("completed_counter");
    }else family["recovery_certified_floor"]=family.at("lower");
    return family;
}
}
