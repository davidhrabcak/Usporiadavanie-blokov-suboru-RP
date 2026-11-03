#ifndef RP_JUNCTION_SCORER_HPP
#define RP_JUNCTION_SCORER_HPP

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
#include "config.hpp"
#include "mp3/chunk_meta.hpp"
#include "mp3/main_data_decoder.hpp"
#include "scoring/pcm_junction.hpp"

// How well does chunk b continue the path that ends in chunk a? Three terms, all used only to
// rank candidates (never to reject an edge):
//   gain      |Δ global_gain| + |Δ big_values| / 32 across the boundary (side info only);
//   spectral  L2 distance of the 12-band energy of the two boundary granules (Milestone 2);
//   PCM       the spectral-flux spike when the junction is decoded to audio (pcm_junction).
// The spectral and PCM weights are calibrated per file (calibrateSpectral, calibratePcm).

constexpr int PCM_LEFT_MAX = 8;   // most path chunks a PCM window takes on its left side

// Per-chunk Layer III side-info features at each end, for the continuity ranking heuristic.
// *First = granule 0 of the chunk's first complete frame; *Last = last granule of the last
// complete frame. -1 when unavailable (no complete frame / side info didn't fit).
// See PLAN_sideinfo_mainbits.md Step 1 + A2: `global_gain` (granule loudness) is the dominant
// junction-continuity signal - |Δ global_gain| ranks the true successor #1 in ~60-74% of VBR
// branch points (vs ~30% for the graph's own order). A2: adding a small |Δ big_values|
// (spectral-occupancy) term robustly nudges rank-1 up ~1-3pp on every fixture.
struct BoundaryGain {
    int ggFirst = -1, ggLast = -1;   // global_gain
    int bvFirst = -1, bvLast = -1;   // big_values
};

// Summed PCM flux of an order: junctions the window can't measure are counted, not summed.
struct FluxSum {
    int unmeasured = 0;
    double sum = 0;
};

class JunctionScorer {
public:
    // Fixed penalty for a junction whose global_gain is unknown on either side: above the 255
    // max real delta, so an ordering that resolves more junctions scores better, unknowns sort
    // last, and the running sum stays finite (needed for the best-of branch-and-bound).
    static constexpr int UNKNOWN_JUNCTION_COST = 384;

    // Reads the boundary gains and sizes the PCM window (prints its size when it isn't 3).
    // `chunks` and `metas` are referenced, not copied: they must outlive the scorer and keep
    // their elements in place (metas' gapAfter flags may still change).
    JunctionScorer(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
                   const StreamProfile& profile, long chunkSize, const Config& cfg);

    int pcmLeft() const { return pcmLeft_; }        // path chunks on a PCM window's left side
    bool spectralOn() const { return spec_ && isLayerIII_; }
    bool pcmOn() const { return pcm_; }

    // Junction cost of putting a supernode whose first chunk is `b` right after one whose last
    // chunk is `a`: |Δ global_gain| + |Δ big_values|/32 across that boundary (A2 - the /32
    // matches the z-scored 1.0*gg + 0.15*bv the probe found robust). Used for both DFS
    // exploration order and the best-of score.
    int gainCost(int a, int b) const;
    // Spectral term for `candHead` after `curTail` (whose predecessor is `curPrev`, -1 none):
    // the trailing spectrum needs `curTail`'s predecessor and the leading one needs `curTail`
    // itself (cross-chunk main-data assembly). 0 when either side has no spectrum.
    double spectralCost(int curPrev, int curTail, int candHead);
    // Raw log1p(flux) for path tail `left[0..nLeft)` (oldest first) followed by `cand`;
    // -1 when the window can't be measured (too little audio, silent pre side).
    double pcmRaw(const int* left, int nLeft, int cand);
    // The PCM term: pcmRaw scaled, unmeasurable junctions at the neutral value.
    double pcmCost(const int* left, int nLeft, int cand);

    // PCM window left side of the junction before seq[k]: the (up to) pcmLeft() chunks before
    // it, oldest first. Returns their count.
    int leftOf(const std::vector<int>& seq, int k, int left[PCM_LEFT_MAX]) const;
    // Summed pcmRaw over every junction of `seq`.
    FluxSum summedFlux(const std::vector<int>& seq);

    // Scales the spectral term so a median random junction contributes ~3 (skipped when
    // RP_SPEC_SCALE is set). Clears the spectral memo.
    void calibrateSpectral();
    // Scales the PCM term so its spread over real structural edges of `adj` is RP_PCM_W times
    // the spread of gain + spectral, and sets the neutral value; turns the term off when
    // ranking (RP_RANK) or the term (RP_PCM) is off. Clears the PCM memo.
    void calibratePcm(const std::vector<std::vector<int>>& adj);

private:
    const Mp3MainData::GranuleSpectrum* memoSpec(bool head, int prev, int chunk);

    const std::vector<std::vector<uint8_t>>& chunks_;
    const std::vector<ChunkMeta>& metas_;
    const bool isLayerIII_;
    const bool rank_;
    const std::optional<double> specScaleOverride_;
    const double pcmWeight_;
    std::vector<BoundaryGain> boundaryGain_;

    // Milestone 2 spectral term. Memoised by (predecessor, chunk); distinct keys ~= edge count.
    bool spec_ = true;
    int sampleRate_ = 44100;
    double specScale_ = 10.0;
    std::unordered_map<long long, Mp3MainData::GranuleSpectrum> specHead_, specTail_;

    // Full-PCM term (PLAN_full_pcm_decode.md), memoised by the window's chunk tuple.
    bool pcm_ = true;
    double pcmScale_ = 1.0;     // set by calibration
    double pcmNeutral_ = 0.0;   // raw value used when a junction can't be measured
    int pcmLeft_ = 3;
    // Window key: up to PCM_LEFT_MAX left ids (oldest first, -1 padded) + the candidate.
    struct PcmKey {
        std::array<int, PCM_LEFT_MAX + 1> id;
        bool operator==(const PcmKey& o) const { return id == o.id; }
    };
    struct PcmKeyHash {
        size_t operator()(const PcmKey& k) const {
            uint64_t h = 1469598103934665603ull;
            for (int v : k.id) h = (h ^ (uint32_t)v) * 1099511628211ull;
            return (size_t)h;
        }
    };
    std::unordered_map<PcmKey, float, PcmKeyHash> pcmMemo_;
    // The DFS scores all candidates of one node with the same left side, so keep the decoded
    // left sides of the last few contexts (a LeftContext holds up to ~250 KB at 32 kbps).
    std::unordered_map<PcmKey, std::unique_ptr<PcmJunction::LeftContext>, PcmKeyHash> leftCache_;
    std::deque<PcmKey> leftOrder_;
};

#endif
