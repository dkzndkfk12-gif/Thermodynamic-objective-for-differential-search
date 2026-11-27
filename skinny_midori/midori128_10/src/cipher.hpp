#pragma once
#include <array>
#include <stdexcept>
inline constexpr int SB1[16]={1,0,5,3,14,2,15,7,13,10,9,11,12,8,4,6};
inline constexpr int BYTE_PERM[4][8]={{4,1,6,3,0,5,2,7},{1,6,7,0,5,2,3,4},
                                   {2,3,4,1,6,7,0,5},{7,4,1,2,3,0,5,6}};
inline int permute_byte(int x,int t){int y=0;for(int j=0;j<8;++j)y|=((x>>(7-BYTE_PERM[t][j]))&1)<<(7-j);return y;}
inline int byte_sbox(int x,int t){int y=permute_byte(x,t);y=(SB1[y>>4]<<4)|SB1[y&15];int z=0;
    for(int j=0;j<8;++j){z|=((y>>(7-j))&1)<<(7-BYTE_PERM[t][j]);}return z;}
struct ByteDDT {
    std::array<std::array<unsigned short,65536>,4> count{};
    ByteDDT(){int small[16][16]{};
        for(int a=0;a<16;++a)for(int x=0;x<16;++x)++small[a][SB1[x]^SB1[x^a]];
        for(int t=0;t<4;++t)for(int a=0;a<256;++a)for(int b=0;b<256;++b){
            int u=permute_byte(a,t),v=permute_byte(b,t);
            count[t][256*a+b]=static_cast<unsigned short>(small[u>>4][v>>4]*small[u&15][v&15]);}
    }
    int operator()(int t,int a,int b)const{return count[t&3][256*a+b];}
};
inline const ByteDDT& byte_ddt(){static const ByteDDT d;return d;}
inline int byte_weight(int n){if(n<=0||(n&(n-1)))throw std::runtime_error("Invalid Midori128 DDT count");
    int w=8;while(n>1){n>>=1;--w;}return w;}
