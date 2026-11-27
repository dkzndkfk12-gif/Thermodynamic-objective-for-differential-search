#pragma once
#include "segment_graph.hpp"
#include <string>
namespace segment_mass {

inline nlohmann::json graph_certificate(const Spec& s,const nlohmann::json& paths,
                                      std::uint64_t memory,const std::function<void()>& check){
    using J=nlohmann::json;
    Graph graph(s,s.N,s.N,memory);std::set<std::pair<W,W>> endpoints;
    if(!paths.is_array())throw std::runtime_error("graph certificate paths not array");
    for(const auto& row:paths){check();if(!row.is_array()||row.size()!=s.R+1)throw std::runtime_error("graph certificate path length");
        std::vector<W> p;for(const auto& x:row)p.push_back(word(x.get<std::string>(),s.N));
        if(!p.front()||!p.back())throw std::runtime_error("graph certificate zero endpoints");
        for(unsigned r=0;r<s.R;++r){if(s.coefficient(p[r],p[r+1])<=0)throw std::runtime_error("graph certificate illegal transition");graph.add(p[r],p[r+1]);}
        endpoints.emplace(p.front(),p.back());
    }
    J rows=J::array();W last=0;std::vector<std::vector<Z>> front;
    for(const auto& ep:endpoints){check();if(front.empty()||last!=ep.first){front=graph.messages(false,graph.index.at(ep.first),check);last=ep.first;}
        const auto& z=front[s.R][graph.index.at(ep.second)];if(z<=0)throw std::runtime_error("empty graph certificate endpoint");
        rows.push_back({{"input",hex(ep.first,s.N)},{"output",hex(ep.second,s.N)},
                       {"numerator",z.get_str()},{"denominator_power",4*s.N*s.R}});
    }
    return {{"revision",1},{"identity",s.identity()},{"rounds",s.R},{"scope","all length-R paths in the retained transition-edge set"},
            {"paths",paths},{"edges",graph.edges.size()},{"records",rows}};
}
inline nlohmann::json verify_graph_certificate(const Spec& s,const nlohmann::json& receipt,
                                              std::uint64_t memory,const std::function<void()>& check){
    if(receipt.value("revision",0)!=1||receipt.value("identity",std::string())!=s.identity()||receipt.value("rounds",0u)!=s.R)
        throw std::runtime_error("graph certificate identity mismatch");
    auto fresh=graph_certificate(s,receipt.at("paths"),memory,check);
    if(fresh.at("records")!=receipt.at("records")||fresh.at("edges")!=receipt.at("edges"))
        throw std::runtime_error("graph certificate integer mass mismatch");
    return fresh;
}
}
