#pragma once

constexpr int ROUNDS = 9;
constexpr int CELLS = 16;
constexpr int INTERNAL_CELLS = CELLS * (ROUNDS - 1);
constexpr int MAX_FACTORS = CELLS * ROUNDS;
constexpr const char* MODEL_MAGIC = "SKINNY_MF9_V1";
constexpr const char* BRIDGE_MAGIC = "SKINNY_BRIDGE9_V1";
constexpr const char* EXACT_MAGIC = "SKINNY_EXACT9_V1";

static_assert(4 * INTERNAL_CELLS + MAX_FACTORS < 1024, "Increase exact integer capacity");
