#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>

namespace corridor_integer {

template<unsigned LIMBS_> struct Integer {
    static constexpr unsigned LIMBS=LIMBS_;
    std::array<std::uint32_t,LIMBS> w{};
    Integer()=default;
    explicit Integer(std::uint64_t n){w[0]=std::uint32_t(n);static_assert(LIMBS>=2);w[1]=std::uint32_t(n>>32);}
    bool zero()const{for(auto a:w)if(a)return false;return true;}
    bool operator<(const Integer& b)const{
        for(int i=int(LIMBS)-1;i>=0;--i)if(w[std::size_t(i)]!=b.w[std::size_t(i)])return w[std::size_t(i)]<b.w[std::size_t(i)];
        return false;
    }
    bool operator==(const Integer& b)const{return w==b.w;}
    Integer shifted(unsigned bits)const{
        Integer out;unsigned words=bits/32,rem=bits%32;
        for(unsigned i=0;i<LIMBS;++i)if(w[i]){
            if(i+words>=LIMBS)throw std::overflow_error("corridor integer shift overflow");
            const std::uint64_t v=std::uint64_t(w[i])<<rem;
            out.w[i+words]|=std::uint32_t(v);
            if(v>>32){if(i+words+1>=LIMBS)throw std::overflow_error("corridor integer shift overflow");out.w[i+words+1]|=std::uint32_t(v>>32);}
        }
        return out;
    }
    Integer& operator+=(const Integer& b){
        std::uint64_t carry=0;
        for(unsigned i=0;i<LIMBS;++i){const auto v=std::uint64_t(w[i])+b.w[i]+carry;w[i]=std::uint32_t(v);carry=v>>32;}
        if(carry)throw std::overflow_error("corridor integer addition overflow");
        return *this;
    }
    Integer operator*(const Integer& b)const{
        Integer out;
        for(unsigned i=0;i<LIMBS;++i)if(w[i]){
            std::uint64_t carry=0;
            for(unsigned j=0;j<LIMBS-i;++j){
                const std::uint64_t v=std::uint64_t(w[i])*b.w[j]+out.w[i+j]+carry;
                out.w[i+j]=std::uint32_t(v);carry=v>>32;
            }
            if(carry)throw std::overflow_error("integer multiplication overflow");
            for(unsigned j=LIMBS-i;j<LIMBS;++j)if(b.w[j])
                throw std::overflow_error("integer multiplication overflow");
        }
        return out;
    }
    long double value()const{long double v=0;for(int i=int(LIMBS)-1;i>=0;--i)v=std::ldexp(v,32)+w[std::size_t(i)];return v;}
    std::string str()const{
        if(zero())return "0";
        auto q=*this;std::string s;
        while(!q.zero()){
            std::uint64_t rem=0;
            for(int i=int(LIMBS)-1;i>=0;--i){const auto v=(rem<<32)|q.w[std::size_t(i)];q.w[std::size_t(i)]=std::uint32_t(v/10);rem=v%10;}
            s.push_back(char('0'+rem));
        }
        return std::string(s.rbegin(),s.rend());
    }
};
using Mass=Integer<(3*NC*ROUNDS+32)/32>;
}
