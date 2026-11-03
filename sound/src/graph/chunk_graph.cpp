#include "graph/chunk_graph.hpp"

#include <algorithm>
#include <iostream>
#include "mp3/reservoir.hpp"

using namespace std;

StructuralGraph buildStructuralGraph(vector<ChunkMeta>& metas, const vector<vector<uint8_t>>& pool,
                                     const long chunkSize) {
    StructuralGraph g;
    g.adj = buildAdjacency(metas);
    if (!pool.empty()) {
        g.gaps = findGapSlots(metas, g.adj, pool, chunkSize);
        for (size_t i = 0; i < metas.size(); ++i) metas[i].gapAfter = !g.gaps.offsets[i].empty();
        addGapEdges(g.adj, metas, g.gaps);
    }
    return g;
}

long filterReservoirArcs(vector<vector<int>>& adj, const vector<ChunkMeta>& metas,
                         const vector<vector<uint8_t>>& chunks, const int sideInfo, const int maxReservoir,
                         const vector<int>& origIndexOf, long& removedTrue) {
    long removed = 0;
    removedTrue = 0;
    for (size_t a = 0; a < adj.size(); ++a) {
        int base = maxReservoir * 8;
        if (!advanceReservoirChunk(base, nullptr, metas[a], chunks[a], sideInfo, maxReservoir)) continue; // a itself: unknown, keep
        auto& out = adj[a];
        out.erase(std::remove_if(out.begin(), out.end(), [&](int b) {
            int level = base;
            if (advanceReservoirChunk(level, &metas[a], metas[b], chunks[b], sideInfo, maxReservoir)) return false;
            ++removed;
            if (origIndexOf[b] == origIndexOf[a] + 1) ++removedTrue;
            return true;
        }), out.end());
    }
    return removed;
}

ChunkGraph buildGraph(vector<ChunkMeta>& metas, const vector<vector<uint8_t>>& chunks,
                      const vector<vector<uint8_t>>& pool, const StreamProfile& profile,
                      const long chunkSize, const int splitTailsBefore, const TrueOrderCheck& truth,
                      const Config& cfg, const vector<int>& origIndexOf) {
    if (splitTailsBefore > 0) {
        const SplitTails st = promoteSplitTails(metas, chunks, chunkSize, profile, chunks[0].data());
        const int n = st.promoted[1] + st.promoted[2] + st.promoted[3];
        if (n != splitTailsBefore)
            cout << "WARNING: split tails promoted after the shuffle " << n << " != " << splitTailsBefore << " before\n";
    }
    StructuralGraph sg = buildStructuralGraph(metas, pool, chunkSize);
    ChunkGraph g;
    g.adj = std::move(sg.adj);
    g.gaps = std::move(sg.gaps);

    // Reservoir arc filter. The DFS would reject these arcs anyway, but the matching and the
    // AC-3 / supernode collapse still counted them. Gated like AC-3: only when the reservoir
    // model passed the true order (a model that rejects real arcs would let AC-3 force wrong
    // ones). RP_RESV_FILTER=0 off, =1 forces past the gate.
    if (profile.isLayerIII()) {
        const bool force = cfg.resvFilter == Switch::On, off = cfg.resvFilter == Switch::Off;
        if (!off && (truth.reservoirBreaks == 0 || force)) {
            long removedTrue = 0;
            const long removed = filterReservoirArcs(g.adj, metas, chunks, sideInfoSize(profile.versionID, profile.isMono()),
                                                     maxReservoirBytes(profile.versionID), origIndexOf, removedTrue);
            cout << "Reservoir arc filter: removed " << removed << " arcs [vs source: " << removedTrue
                 << " true arcs removed]\n";
        } else if (!off) {
            cout << "Reservoir arc filter: skipped (" << (truth.reservoirBreaks < 0 ? "no true-order diagnostic"
                 : "reservoir model rejects the true order") << "; RP_RESV_FILTER=1 forces)\n";
        }
    }
    g.adjFull = g.adj;   // before AC-3 / locking - the repair pass may undo either

    // Arc-consistency prune (A4) - shrinks the branch-point count before the search. It is
    // only sound when `canFollow` has no false negatives among the true consecutive pairs
    // (otherwise it can force a wrong edge where a real predecessor is missing from the
    // graph). Gate on the pre-shuffle diagnostic: skip when it found breaks. RP_AC3=0 forces
    // off, RP_AC3=1 forces on regardless of the gate.
    const bool force = cfg.ac3 == Switch::On;
    const bool off   = cfg.ac3 == Switch::Off;
    const bool safe  = truth.breaks == 0;
    if (off) {
        // disabled
    } else if (!safe && !force) {
        cout << "Arc consistency: skipped (" << (truth.breaks < 0 ? "no pre-shuffle diagnostic"
             : "pre-shuffle found breaks - canFollow is lossy on this file") << "; RP_AC3=1 to force)\n";
    } else {
        const long acRemoved = propagateArcConsistency(g.adj, static_cast<int>(chunks.size()), 0);
        if (acRemoved < 0)
            cout << "Arc consistency: contradiction (canFollow is missing a real edge) - graph left as built\n";
        else
            cout << "Arc consistency: removed " << acRemoved << " edges\n";
    }
    return g;
}
