#pragma once
#include "../../common/gift_word.hpp"
constexpr int NC=16, NB=64, ROUNDS=14;
constexpr int GIFT_P[NB]={0,17,34,51,48,1,18,35,32,49,2,19,16,33,50,3,4,21,38,55,52,5,22,39,36,53,6,23,20,37,54,7,8,25,42,59,56,9,26,43,40,57,10,27,24,41,58,11,12,29,46,63,60,13,30,47,44,61,14,31,28,45,62,15};
constexpr int P(int i){return GIFT_P[i];}
constexpr int PI(int dst){for(int i=0;i<NB;++i)if(P(i)==dst)return i;return -1;}
