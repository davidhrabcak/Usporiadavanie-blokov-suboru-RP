#include "graph/supernodes.hpp"

using namespace std;

vector<Supernode> buildSupernodes(
        const vector<vector<int>>& adj,
        const vector<int>& indeg,
        const vector<int>& outdeg,
        const int n,
        const int start,
        vector<vector<int>>& superAdj) {

    vector<int> chunkToSuper(n, -1);
    vector<Supernode> supernodes;

    // Find chain heads: nodes that start a forced chain.
    // A chain head is a node where we cannot extend a forced chain backwards,
    // i.e. indeg != 1 or its predecessor has outdeg != 1.
    // We detect them by: node is `start`, or indeg[node] != 1, or the unique
    // predecessor has outdeg != 1.
    vector<bool> isHead(n, false);
    isHead[start] = true;
    for (int i = 0; i < n; ++i) {
        if (indeg[i] == 0) { isHead[i] = true; continue; }
        if (indeg[i] > 1)  { isHead[i] = true; continue; }
        // indeg[i] == 1: find predecessor
        bool predOk = false;
        for (int p = 0; p < n; ++p) {
            for (int s : adj[p]) {
                if (s == i && outdeg[p] == 1) { predOk = true; break; }
            }
            if (predOk) break;
        }
        if (!predOk) isHead[i] = true;
    }

    // Grow a chain from each head
    for (int i = 0; i < n; ++i) {
        if (!isHead[i]) continue;
        Supernode sn;
        int cur = i;
        while (true) {
            if (chunkToSuper[cur] != -1) break; // already part of another supernode
            chunkToSuper[cur] = static_cast<int>(supernodes.size());
            sn.chunks.push_back(cur);
            if (outdeg[cur] == 1) {
                const int next = adj[cur][0];
                if (indeg[next] == 1) {
                    cur = next;
                    continue;
                }
            }
            break;
        }
        supernodes.push_back(sn);
    }

    // Any chunk not yet assigned (cycle-only nodes, shouldn't happen in practice)
    for (int i = 0; i < n; ++i) {
        if (chunkToSuper[i] == -1) {
            chunkToSuper[i] = static_cast<int>(supernodes.size());
            supernodes.push_back({{i}});
        }
    }

    const int sn = static_cast<int>(supernodes.size());
    superAdj.assign(sn, {});

    for (int si = 0; si < sn; ++si) {
        const int tail = supernodes[si].chunks.back();
        for (const int nb : adj[tail]) {
            int sj = chunkToSuper[nb];
            if (sj != si)
                superAdj[si].push_back(sj);
        }
    }

    return supernodes;
}

vector<int> chunksOf(const vector<Supernode>& supernodes, const vector<int>& order) {
    vector<int> seq;
    for (int si : order) for (int ci : supernodes[si].chunks) seq.push_back(ci);
    return seq;
}
