#include "graph/supernodes.hpp"

using namespace std;

vector<Supernode> buildSupernodes(const vector<vector<int>>& adj, const int start,
                                  vector<vector<int>>& superAdj) {
    const int n = static_cast<int>(adj.size());
    vector<int> indeg(n, 0), pred(n, -1);   // pred: a predecessor (the only one when indeg == 1)
    for (int i = 0; i < n; ++i)
        for (const int j : adj[i]) { ++indeg[j]; pred[j] = i; }

    // i continues its predecessor's chain when it is that chunk's only successor and has no
    // other predecessor; every other chunk heads a chain.
    auto isHead = [&](const int i) {
        return i == start || indeg[i] != 1 || adj[pred[i]].size() != 1;
    };

    vector<int> chunkToSuper(n, -1);
    vector<Supernode> supernodes;
    auto grow = [&](int cur) {
        Supernode sn;
        while (chunkToSuper[cur] == -1) {
            chunkToSuper[cur] = static_cast<int>(supernodes.size());
            sn.chunks.push_back(cur);
            if (adj[cur].size() != 1 || indeg[adj[cur][0]] != 1 || adj[cur][0] == start) break;
            cur = adj[cur][0];
        }
        supernodes.push_back(std::move(sn));
    };
    for (int i = 0; i < n; ++i) if (isHead(i)) grow(i);
    // chunks on a forced cycle have no head (not reachable in a valid order): one supernode each
    for (int i = 0; i < n; ++i)
        if (chunkToSuper[i] == -1) {
            chunkToSuper[i] = static_cast<int>(supernodes.size());
            supernodes.push_back({{i}});
        }

    superAdj.assign(supernodes.size(), {});
    for (size_t si = 0; si < supernodes.size(); ++si)
        for (const int nb : adj[supernodes[si].chunks.back()]) {
            const int sj = chunkToSuper[nb];
            if (sj != static_cast<int>(si)) superAdj[si].push_back(sj);
        }
    return supernodes;
}

vector<int> chunksOf(const vector<Supernode>& supernodes, const vector<int>& order) {
    vector<int> seq;
    for (int si : order) for (int ci : supernodes[si].chunks) seq.push_back(ci);
    return seq;
}
