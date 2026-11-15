#ifndef RP_CONTINUITY_HPP
#define RP_CONTINUITY_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "mp3/chunk_meta.hpp"

// Exact main-data continuity as a junction cost (HOW_IT_WORKS 4.7a; RP_CONT=0 turns it off).
//
// Every arc a->b of the graph is classed by junctionSlack (reservoir.hpp): tight (b continues
// a's main data exactly), hole (it fits but leaves reservoir bytes unused), at the cap (the
// reservoir was full and this encoder doesn't say what it writes then), or unknown (split header
// or side info unreadable, no complete frame). Never a filter - 0-10% of true junctions are
// holes - each class only adds a cost, in log1p(flux) units, to the PCM term of the DFS ranking
// and the repair and to the matching's edge cost (where hole arcs are not decoded at all).
//
// What the check needs about the encoder is read from consecutive complete frames inside one
// chunk, which are true pairs by construction, so no chunk order is used:
//   - capEff, the encoder's own reservoir cap (largest main_data_begin over all frames);
//   - `fullRule`: the encoder writes main_data_begin == capEff once the level reaches it
//     (>= 95% of such in-chunk pairs, >= 20 of them) - then those junctions are testable too.
// Costs are fixed: tight 0, at cap / unknown 0.5, hole 2. Per-file costs from the class
// distribution of in-chunk pairs vs all arcs gave identical outcomes (A/B 2026-11-15).
class Continuity {
public:
    enum Class : uint8_t { Tight = 0, AtCap = 1, Hole = 2, Unknown = 3 };

    Continuity(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
               const std::vector<std::vector<int>>& adj, const StreamProfile& profile,
               const std::vector<int>& origIndexOf);

    double cost(int a, int b) const { return COST[cls(a, b)]; }
    bool hole(int a, int b) const { return cls(a, b) == Hole; }

private:
    static constexpr double COST[4] = {0.0, 0.5, 2.0, 0.5};
    Class cls(int a, int b) const {
        const auto it = cls_.find((static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b));
        return it == cls_.end() ? Unknown : static_cast<Class>(it->second);
    }
    std::unordered_map<uint64_t, uint8_t> cls_;
};

#endif
