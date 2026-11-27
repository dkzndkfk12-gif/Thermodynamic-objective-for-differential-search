#pragma once
namespace thermo {
inline double negative_log_sum(double a,double b){
    if(!std::isfinite(a))return b;
    if(!std::isfinite(b))return a;
    const double lo=std::min(a,b);return lo-std::log2(1+std::exp2(-std::abs(a-b)));
}

inline void mass_work_feedback(J&row,double gain_F,double cpu,unsigned window){
    if(!(cpu>0))return;
    auto &s=row["mass_work"];if(s.is_null())s=J{{"jobs",0},{"cpu_ema",0},{"gain_F_ema",J()},{"rate_F",J()}};
    const bool first=count(s,"jobs")==0;

    const double alpha=window?1./window:1./(double(count(s,"jobs"))+1.);
    double old=number(s,"gain_F_ema",INFINITY);
    double f=first?gain_F:negative_log_sum(alpha<1?old-std::log2(1-alpha):INFINITY,
                                         std::isfinite(gain_F)?gain_F-std::log2(alpha):INFINITY);
    s["cpu_ema"]=first?cpu:(1-alpha)*number(s,"cpu_ema")+alpha*cpu;
    s["gain_F_ema"]=std::isfinite(f)?J(f):J();
    s["rate_F"]=std::isfinite(f)?J(f+std::log2(number(s,"cpu_ema"))):J();
    increment(s,"jobs");s["last_gain_F"]=std::isfinite(gain_F)?J(gain_F):J();
}
inline double mass_cost_score(const J&c,double prior_F,double seconds){
    if(c.contains("mass_work")&&c.at("mass_work").at("rate_F").is_number())return number(c.at("mass_work"),"rate_F");

    return prior_F+std::log2(std::max(1.,seconds));
}
}
