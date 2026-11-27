#pragma once
#include "twine.hpp"

namespace twine {

inline std::string encode_trail_record(const Trail& trail){
    constexpr char digits[]="0123456789ABCDEF";
    std::string code=hex(trail.front());
    code.reserve(16+8*(trail.size()-1));
    for(size_t r=0;r+1<trail.size();++r){
        for(int j=0;j<8;++j){
            int even=2*j,odd=even+1;
            if(trail[r][even])code+=digits[trail[r][odd]^trail[r+1][PI[odd]]];
        }
    }
    return code;
}

inline Trail decode_trail_record(const std::string& code,int rounds){
    require(code.size()>=16,"short compact trail record");
    Trail trail;
    trail.reserve(size_t(rounds)+1);
    trail.push_back(unhex(code.substr(0,16)));
    size_t offset=16;
    for(int r=0;r<rounds;++r){
        const auto& state=trail.back();
        std::array<uint8_t,8> deltas{};
        for(int j=0;j<8;++j){
            if(!state[2*j])continue;
            require(offset<code.size(),"truncated compact trail record");
            char digit=code[offset++];
            int value=digit>='0'&&digit<='9'?digit-'0':digit-'A'+10;
            require((digit>='0'&&digit<='9')||(digit>='A'&&digit<='F'),"bad compact trail digit");
            deltas[j]=uint8_t(value);
        }
        trail.push_back(forward(state,deltas));
    }
    require(offset==code.size(),"trailing data in compact trail record");
    return trail;
}
}
