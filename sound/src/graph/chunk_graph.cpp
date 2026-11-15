#include "graph/chunk_graph.hpp"

#include <algorithm>
#include <iostream>
#include "graph/adjacency.hpp"
#include "mp3/reservoir.hpp"

using namespace std;

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

ChunkGraph buildGraph(const vector<ChunkMeta>& metas, const vector<vector<uint8_t>>& chunks,
                      const StreamProfile& profile, const Config& cfg, const vector<int>& origIndexOf) {
    ChunkGraph g;
    g.adj = buildAdjacency(metas);

    // Reservoir arc filter. The DFS would reject these arcs anyway, but the matching and the
    // AC-3 / supernode collapse still counted them.
    if (cfg.resvFilter && profile.isLayerIII()) {
        long removedTrue = 0;
        const long removed = filterReservoirArcs(g.adj, metas, chunks, sideInfoSize(profile.versionID, profile.isMono()),
                                                 maxReservoirBytes(profile.versionID), origIndexOf, removedTrue);
        cout << "Reservoir arc filter: removed " << removed << " arcs [vs source: " << removedTrue
             << " true arcs removed]\n";
    }
    g.adjFull = g.adj;   // before AC-3 / locking - the repair pass may undo either

    // Arc consistency shrinks the branch-point count before the search. It forces a wrong edge
    // where a real one is missing from the graph, which shows as a contradiction sooner or later;
    // the graph is then left as built.
    if (cfg.ac3) {
        const long acRemoved = propagateArcConsistency(g.adj, 0);
        if (acRemoved < 0)
            cout << "Arc consistency: contradiction (canFollow is missing a real edge) - graph left as built\n";
        else
            cout << "Arc consistency: removed " << acRemoved << " edges\n";
    }
    return g;
}
