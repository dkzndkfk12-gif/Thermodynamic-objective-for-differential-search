#pragma once
#include "geometry.hpp"
constexpr int INTERNAL_CELLS=NC*(ROUNDS-1), MAX_FACTORS=NC*ROUNDS;
constexpr unsigned INTEGER_LIMBS=(4*INTERNAL_CELLS+2*MAX_FACTORS+32)/32;
constexpr const char* ADAPTIVE_MAGIC="GIFT128_EXACT21_V1";
constexpr unsigned char SBOX[16]={1,10,4,12,6,15,3,9,2,13,11,7,5,0,8,14};
static_assert(4*INTERNAL_CELLS+2*MAX_FACTORS<32*INTEGER_LIMBS,"Exact integer capacity");
