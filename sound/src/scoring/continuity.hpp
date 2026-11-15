#ifndef RP_CONTINUITY_HPP
#define RP_CONTINUITY_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "mp3/chunk_meta.hpp"

// PROOF OF CONCEPT (RP_CONT=fixed|cal): exact main-data continuity as a junction cost.
//
// Every arc a->b of the graph is classed by junctionSlack: tight (b continues a's main data
// exactly), hole (it fits but leaves reservoir bytes unused), at the cap, or unknown. Never a filter:
// each class only adds a cost, in log1p(flux) units, to the PCM term of the DFS ranking and the
// repair and to the matching's edge cost (where hole arcs are not decoded at all).
//
// Calibrated per file without the true order ("cal"): consecutive complete frames inside one
// chunk are true pairs by construction. They give
//   - capEff, the encoder's own reservoir cap (largest main_data_begin over all frames);
//   - whether the encoder writes main_data_begin == capEff once the level reaches it
//     (`fullRule`, >= 95% of such in-chunk pairs) - then those junctions are testable too;
//   - the class distribution of true pairs; all arcs of the graph (nearly all wrong) give it
//     for wrong pairs. A class's cost is K * (best log-likelihood ratio - its own); Unknown has
//     ratio 1.
// "fixed": same check, fixed costs (tight 0, at cap / unknown 0.5, hole 2) - the control.
class Continuity {
public:
    // AtCap: the reservoir was at the encoder's cap (a property of the stream, calibrated);
    // Unknown: the junction can't be read (split header / side info, no complete frame) -
    // unrelated to whether it is true, so it costs as much as no evidence at all.
    enum Class : uint8_t { Tight = 0, AtCap = 1, Hole = 2, Unknown = 3 };

    Continuity(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
               const std::vector<std::vector<int>>& adj, const StreamProfile& profile, const std::string& mode,
               const std::vector<int>& origIndexOf);

    double cost(int a, int b) const { return cost_[cls(a, b)]; }
    bool hole(int a, int b) const { return cls(a, b) == Hole; }

private:
    Class cls(int a, int b) const {
        const auto it = cls_.find((static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b));
        return it == cls_.end() ? Unknown : static_cast<Class>(it->second);
    }
    std::unordered_map<uint64_t, uint8_t> cls_;
    double cost_[4] = {0, 0, 0, 0};
};

#endif
