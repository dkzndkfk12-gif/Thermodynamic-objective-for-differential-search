#pragma once
inline constexpr int ROUNDS=9;
inline constexpr int CELLS=16;
inline constexpr int INTERNAL_CELLS=CELLS*(ROUNDS-1);
inline constexpr int MAX_FACTORS=CELLS*ROUNDS;
inline constexpr int INTEGER_LIMBS=64;
inline constexpr const char* MODEL_MAGIC="MIDORI128_MF9_V1";
inline constexpr const char* BRIDGE_MAGIC="MIDORI128_BRIDGE9_V1";

static_assert(8*INTERNAL_CELLS+4*MAX_FACTORS < 32*INTEGER_LIMBS,
              "Every integer intermediate must fit the declared capacity");
