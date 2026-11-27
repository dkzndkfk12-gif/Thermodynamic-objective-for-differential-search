#pragma once
inline constexpr int ROUNDS=10;
inline constexpr int CELLS=16;
inline constexpr int INTERNAL_CELLS=CELLS*(ROUNDS-1);
inline constexpr int MAX_FACTORS=CELLS*ROUNDS;
inline constexpr int INTEGER_LIMBS=32;
inline constexpr const char* MODEL_MAGIC="MIDORI_MF10_V1";
inline constexpr const char* BRIDGE_MAGIC="MIDORI_BRIDGE10_V1";
inline constexpr int SBOX[16]={12,10,13,3,14,11,15,7,8,9,1,5,0,2,4,6};

static_assert(4*INTERNAL_CELLS+MAX_FACTORS < 32*INTEGER_LIMBS,
              "Every exact intermediate must fit in the integer capacity");
