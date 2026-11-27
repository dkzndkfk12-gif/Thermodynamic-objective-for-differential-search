#pragma once
#include <gmpxx.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>
namespace segment_mass {
inline std::uint64_t reserve_add(std::uint64_t a,std::uint64_t b){return b>UINT64_MAX-a?UINT64_MAX:a+b;}
inline std::uint64_t reserve_mul(std::uint64_t a,std::uint64_t b){return b&&a>UINT64_MAX/b?UINT64_MAX:a*b;}

inline std::uint64_t message_reservation_bytes(unsigned cells,unsigned rounds,std::uint64_t nodes){
    std::uint64_t each=2*sizeof(unsigned);
    for(unsigned t=0;t<=rounds;++t)
        each=reserve_add(each,48+((std::uint64_t(4)*cells*t+1+GMP_NUMB_BITS-1)/GMP_NUMB_BITS)*sizeof(mp_limb_t));
    return reserve_add(reserve_mul(rounds+1,sizeof(std::vector<mpz_class>)),reserve_mul(nodes,each));
}
inline std::uint64_t graph_seed_reservation_bytes(unsigned cells,unsigned rounds,std::uint64_t nodes,std::uint64_t edges){
    auto structure=reserve_add(reserve_mul(nodes,320),reserve_mul(edges,192+((std::uint64_t(4)*cells+1+GMP_NUMB_BITS-1)/GMP_NUMB_BITS)*sizeof(mp_limb_t)));
    return std::max(reserve_mul(structure,2),reserve_add(structure,reserve_mul(message_reservation_bytes(cells,rounds,nodes),4)));
}
}
