#pragma once
inline constexpr int ROUNDS=11;
inline constexpr int CELLS=16;
inline constexpr int INTERNAL_CELLS=CELLS*(ROUNDS-1);
inline constexpr int MAX_FACTORS=CELLS*ROUNDS;
inline constexpr int INTEGER_LIMBS=32;
inline constexpr const char* MODEL_MAGIC="SKINNY_MF11_V1";
inline constexpr const char* BRIDGE_MAGIC="SKINNY_BRIDGE11_V1";
inline constexpr int SBOX[16]={12,6,9,0,1,10,2,11,3,8,5,13,4,14,7,15};

static_assert(4*INTERNAL_CELLS+MAX_FACTORS < 32*INTEGER_LIMBS,
              "Every exact intermediate must fit in the integer capacity");
