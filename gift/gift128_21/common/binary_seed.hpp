#pragma once
#include "model.hpp"
namespace thermo {
using BinaryMasks = std::vector<unsigned>;
using BinaryRanked = std::pair<std::string, BinaryMasks>;

inline std::map<int, std::vector<BinaryRanked>> ranked_binary_supports(
    const std::vector<unsigned> &basis, int rounds, const std::vector<int> &counts,
    std::uint64_t seed, unsigned keep, const Deadline &dl) {
    require(!basis.empty() && basis.size() <= 16 && rounds > 0 && keep > 0,
            "Invalid binary seed dimensions");
    const unsigned size = 1u << basis.size();
    for (auto v : basis) require(v < size, "Binary map basis outside range");
    std::vector<unsigned> linear(size);
    for (unsigned mask = 1; mask < size; ++mask) {
        if (!(mask % 256)) dl.check();
        const unsigned low = mask & (~mask + 1u);
        linear[mask] = linear[mask ^ low] ^ basis[__builtin_ctz(low)];
    }
    const std::set<int> targets(counts.begin(), counts.end());
    std::map<int, std::vector<BinaryRanked>> buckets;
    for (unsigned mask = 1; mask < size; ++mask) {
        if (!(mask % 256)) dl.check();
        BinaryMasks states;
        int active = 0;
        unsigned value = mask;
        for (int r = 0; r < rounds; ++r) {
            states.push_back(value);
            active += __builtin_popcount(value);
            value = linear[value];
        }
        if (!targets.count(active)) continue;
        states.push_back(value);
        auto &bucket = buckets[active];
        bucket.emplace_back(digest(J::array({seed, states})), std::move(states));
        std::sort(bucket.begin(), bucket.end());
        if (bucket.size() > keep) bucket.resize(keep);
    }
    dl.check();
    return buckets;
}

inline std::vector<std::vector<std::string>> midori_binary_witnesses(
    const Cipher &c, const std::vector<int> &counts, std::uint64_t seed, unsigned keep,
    const Deadline &dl) {
    require(c.midori && c.B == 4 && !c.present, "Binary equal-value starts apply to Midori64 only");
    std::vector<unsigned> basis;
    for (int i = 0; i < 16; ++i) {
        std::vector<int> x(16); x[i] = 1;
        const auto y = c.forward(x);
        unsigned mask = 0;
        for (int j = 0; j < 16; ++j) {
            require(y[j] == 0 || y[j] == 1, "Binary linear image is not binary");
            mask |= unsigned(y[j]) << j;
        }
        basis.push_back(mask);
    }
    auto buckets = ranked_binary_supports(basis, c.R, counts, seed, keep, dl);
    std::vector<int> values;
    for (int v = 1; v < 16; ++v) if (c.count(0, v, v) == 4) values.push_back(v);
    require(!values.empty(), "Midori64 self-DDT values missing");
    std::vector<std::vector<std::string>> result;
    for (const auto &kv : buckets) for (std::size_t j = 0; j < kv.second.size(); ++j) {
        dl.check();
        std::vector<std::string> states;
        for (unsigned mask : kv.second[j].second) {
            std::vector<int> x(16);
            for (int cell = 0; cell < 16; ++cell) x[cell] = (mask & (1u << cell)) ? values[j % values.size()] : 0;
            states.push_back(c.encode(x));
        }
        result.push_back(std::move(states));
    }
    return result;
}
}
