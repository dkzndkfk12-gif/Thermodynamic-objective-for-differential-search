#pragma once
#include <cstdint>
#include <string>
#include <stdexcept>
#include <functional>
namespace gift_word {
using Word=__uint128_t;
struct Hash {std::size_t operator()(Word x) const noexcept {
    std::uint64_t a=std::uint64_t(x),b=std::uint64_t(x>>64);
    auto mix=[](std::uint64_t v){v^=v>>30;v*=0xbf58476d1ce4e5b9ULL;v^=v>>27;v*=0x94d049bb133111ebULL;return v^(v>>31);};
    return std::size_t(mix(a)^mix(b+0x9e3779b97f4a7c15ULL));
}};
inline std::string text(Word x,int cells) {
    std::string s(cells,'0');for(int i=cells-1;i>=0;--i){s[i]="0123456789ABCDEF"[unsigned(x&15)];x>>=4;}return s;
}
inline Word parse(const std::string&s,int cells) {
    if(int(s.size())!=cells)throw std::runtime_error("GIFT state length mismatch");
    Word x=0;for(char c:s){auto v=std::string("0123456789ABCDEF").find(c);
        if(v==std::string::npos)throw std::runtime_error("invalid GIFT hex");
        x=(x<<4)|Word(v);}return x;
}
}
