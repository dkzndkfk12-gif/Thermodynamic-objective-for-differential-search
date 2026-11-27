#pragma once
#include "factor_sum.hpp"
namespace native_memo {
template<class Number> std::string encode(const std::vector<Number>&v){std::string s;for(const auto&x:v)s+=x.str()+"\n";return s;}
template<class Number> bool decode(const std::string&s,std::size_t n,std::vector<Number>&out){
    std::istringstream in(s);std::string row;out.clear();out.reserve(n);
    while(std::getline(in,row)){if(row.empty()||row.size()>2048||row.find_first_not_of("0123456789")!=std::string::npos||out.size()>=n)return false;
        Number x;for(char c:row){x*=std::uint32_t(10);x+=Number(std::uint32_t(c-'0'));}out.push_back(std::move(x));}return out.size()==n;
}
template<class Number> bool get(const std::string&key,std::size_t n,std::vector<Number>&out){
    if(key.size()>mass_messages::slot_bytes/2||n>mass_messages::slot_bytes/sizeof(Number))return false;
    std::string s;std::vector<Number> temporary;if(!mass_messages::cache().get(key,s)||!decode(s,n,temporary))return false;out=std::move(temporary);return true;
}
template<class Number> void put(const std::string&key,const std::vector<Number>&v){
    if(key.size()>mass_messages::slot_bytes/2||v.size()>mass_messages::slot_bytes/sizeof(Number))return;
    auto s=encode(v);mass_messages::cache().put(key,s);
}
template<class Factor,class Type> std::string factor_key(const Factor&f,const factor_sum::Domains&d,Type type,const std::string&cipher){
    int a=f.a,b=f.b;std::vector<int>aa,bb;
    for(int v:f.av)if(d[v].size()==1)a^=d[v][0];else aa.push_back(v);
    for(int v:f.bv)if(d[v].size()==1)b^=d[v][0];else bb.push_back(v);
    return "EXACT_DDT_NUMERATOR_V3/"+cipher+"/"+std::to_string(type(f))+"/"+std::to_string(a)+"/"+std::to_string(b)+"/"+factor_sum::ints(aa)+"/"+factor_sum::ints(bb);
}
inline std::string message_key(const std::vector<std::string>&inputs,const std::vector<int>&scope,
                               const factor_sum::Domains&d,int eliminated=-1){
    auto sorted=inputs;std::sort(sorted.begin(),sorted.end());std::string s="INTEGER_MESSAGE_V3/"+std::to_string(eliminated);
    if(eliminated>=0)factor_sum::append(s,factor_sum::ints(d[eliminated]));
    for(int v:scope){factor_sum::append(s,std::to_string(v));factor_sum::append(s,factor_sum::ints(d[v]));}
    for(const auto&t:sorted)factor_sum::append(s,t);
    return s;
}
}
