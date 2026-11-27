#pragma once

constexpr int ROUNDS = 17;
constexpr int INTERNAL_CELLS = 16*(ROUNDS-1);
constexpr int MAX_FACTORS = 16*ROUNDS;
constexpr unsigned INTEGER_LIMBS = 128;
constexpr const char* ADAPTIVE_MAGIC = "PRESENT_ADAPTIVE17_V1";

static_assert(4*INTERNAL_CELLS+MAX_FACTORS < 32*INTEGER_LIMBS,
              "Increase checked integer capacity");
constexpr unsigned char SBOX[16] = {12,5,6,11,9,0,10,13,3,14,15,8,4,7,1,2};
