#include "pipeline/input.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include "graph/adjacency.hpp"
#include "mp3/frame_scanner.hpp"
#include "mp3/reservoir.hpp"
#include "run_report.hpp"

using namespace std;

namespace {

// Constant-bitrate detection: if every frame shares one bitrate, enable the frame-header
// bitrate filter in StreamProfile::matches so false-positive syncs at other bitrates are
// rejected. Order-independent (detectCBRUnordered): the first chunk is unknown.
bool detectCbr(const vector<vector<uint8_t>>& chunks, const Config& cfg, int& kbps) {
    bool cbr = false;
    kbps = 0;
    {
        StreamProfile tmp{};
        if (voteProfile(chunks, tmp)) cbr = detectCBRUnordered(chunks, tmp, kbps);
    }
    if (!cfg.cbrFilter) cbr = false;
    if (cbr)
        cout << "Detected constant bitrate: " << kbps
             << " kbps - frame-header bitrate filter enabled\n";
    return cbr;
}

// Shuffle seed. Deterministic by default so a given input always reconstructs the same way;
// RP_SEED=<n> pins a specific one, RP_SEED=random draws from the clock. origIndexOf is shuffled
// with the same seed/engine as the chunks, so it stays in lock-step and lets us score the
// reconstruction against the true order at the end.
void shuffleChunks(Problem& p, const Config& cfg) {
    const unsigned seed = cfg.seed;
    vector<vector<uint8_t>>& chunks = p.chunks;
    p.origIndexOf.resize(chunks.size());
    for (size_t i = 0; i < p.origIndexOf.size(); ++i) p.origIndexOf[i] = static_cast<int>(i);
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));
    shuffle(p.origIndexOf.begin() + 1, p.origIndexOf.end(), default_random_engine(seed));

    // The first chunk is not known either: move it to a random position too. Done as one swap
    // after the shuffle above, so a correctly found start restores exactly the layout the
    // pipeline had when chunk 0 was pinned. RP_START=pinned keeps chunk 0 in place (A/B).
    if (!cfg.startPinned && chunks.size() > 1) {
        std::uniform_int_distribution<size_t> pick(0, chunks.size() - 1);
        std::mt19937 rngStart(seed + 2);   // not minstd: its first draw from a small seed is ~0
        const size_t k = pick(rngStart);
        std::swap(chunks[0], chunks[k]);
        std::swap(p.origIndexOf[0], p.origIndexOf[k]);
    }
}

// Find the first chunk from content alone (findStartCandidates): a header at offset 0 and, for
// Layer III, main_data_begin == 0. Several candidates: prefer a Xing/Info/VBRI frame, then the
// lowest index (arbitrary; reported). The start is then swapped to index 0, and everything
// after treats index 0 as the start.
bool placeStart(Problem& p) {
    vector<vector<uint8_t>>& chunks = p.chunks;
    const vector<int>& origIndexOf = p.origIndexOf;
    StreamProfile vote{};
    if (!voteProfile(chunks, vote)) {
        cerr << "Could not derive stream profile from any chunk\n";
        RunReport::set("error", "could not derive stream profile");
        return false;
    }
    const vector<StartCandidate> cands = findStartCandidates(chunks, vote);
    int nXing = 0;
    for (const auto& c : cands) nXing += c.xing;
    if (cands.empty()) {
        cerr << "No chunk can be the first (no header at offset 0 with main_data_begin == 0)\n";
        RunReport::set("error", "no start candidate");
        return false;
    }
    int start = cands[0].index;
    for (const auto& c : cands) if (c.xing) { start = c.index; break; }
    int trueCands = 0;
    for (const auto& c : cands) trueCands += origIndexOf[c.index] == 0;
    cout << "Start chunk: " << cands.size() << " candidate(s)"
         << (nXing ? " (" + std::to_string(nXing) + " with a Xing/Info/VBRI frame)" : string())
         << ", chose index " << start
         << (cands.size() > 1 && !(nXing == 1) ? " - AMBIGUOUS" : "")
         << " [vs source: " << (origIndexOf[start] == 0 ? "correct" : "WRONG")
         << (trueCands ? "" : ", true start not among the candidates") << "]\n";
    RunReport::set("start_candidates", cands.size());
    RunReport::set("start_xing", nXing);
    RunReport::set("start_correct", origIndexOf[start] == 0);
    std::swap(chunks[0], chunks[start]);
    std::swap(p.origIndexOf[0], p.origIndexOf[start]);
    return true;
}

// Derive the stream profile from chunk 0 (the start found above, or pinned with RP_START=pinned).
bool deriveStreamProfile(Problem& p, const bool cbr, const int cbrKbps) {
    StreamProfile& profile = p.profile;
    profile = StreamProfile{};
    if (!deriveProfile(p.chunks[0], profile)) {
        cerr << "Could not derive stream profile from first chunk\n";
        RunReport::set("error", "could not derive stream profile");
        return false;
    }
    if (cbr) { profile.isCBR = true; profile.bitrateKbps = cbrKbps; }
    RunReport::setRaw("profile", string("{\"mpeg\": ") + RunReport::quote(profile.versionID == 0b11 ? "1" : profile.versionID == 0b10 ? "2" : "2.5")
        + ", \"layer\": " + std::to_string(profile.layerID == 0b01 ? 3 : profile.layerID == 0b10 ? 2 : 1)
        + ", \"sample_rate\": " + std::to_string(profile.sampleRate)
        + ", \"cbr_kbps\": " + (profile.isCBR ? std::to_string(profile.bitrateKbps) : string("null"))
        + ", \"crc\": " + (profile.hasCrc ? "true" : "false")
        + ", \"mono\": " + (profile.isMono() ? "true" : "false") + "}");
    cout << "Stream profile: versionID=" << static_cast<int>(profile.versionID)
         << " layerID=" << static_cast<int>(profile.layerID)
         << " sampleRate=" << profile.sampleRate
         << " mono=" << profile.isMono()
         << (profile.isCBR ? (" cbr=" + std::to_string(profile.bitrateKbps) + "kbps") : string(" vbr"))
         << (profile.hasCrc ? " crc" : "") << "\n";
    return true;
}

void computeMetas(Problem& p) {
    p.metas.reserve(p.chunks.size());
    int invalid = 0, noHeader = 0;
    for (const auto& c : p.chunks) {
        p.metas.push_back(computeChunkMeta(c, p.profile, static_cast<long>(c.size()) < p.chunkSize));
        if (p.metas.back().valid) continue;
        ++invalid;
        if (p.metas.back().frameStarts.empty()) ++noHeader;
    }
    if (invalid > 0)
        cerr << invalid << " chunks have no usable frame run (" << noHeader << " without any frame header: "
             << "frames longer than the chunk are not supported, use a larger RP_CHUNK_SIZE) - "
             << "reconstruction may fail\n";
    RunReport::set("invalid_chunks", invalid);
}

// Diagnostic: how the rules fare on the source order. A break means the structural rule
// (canFollow) or the reservoir model rejects a true junction, so the true order is not a path
// of the graph the search runs on.
void checkSourceOrder(const Problem& p) {
    const int n = static_cast<int>(p.chunks.size());
    vector<int> src(n);
    for (int ci = 0; ci < n; ++ci) src[p.origIndexOf[ci]] = ci;
    int broken = 0;
    for (int i = 0; i + 1 < n; ++i) {
        const ChunkMeta &a = p.metas[src[i]], &b = p.metas[src[i + 1]];
        if (canFollow(a, b)) continue;
        cout << "  BREAK at " << i << "->" << i + 1 << ": tailOvfl=" << a.tailOverflow
             << " tailPartialLen=" << a.tailPartialLen << " headOff=" << b.headOffset << "\n";
        ++broken;
    }
    RunReport::set("pre_breaks", broken);
    cout << "Structural rule on the source order: " << broken << " break(s) in " << n - 1 << " junctions\n";
    if (!p.profile.isLayerIII()) return;
    vector<int> breakAt;
    const int breaks = reservoirBreaks(p.metas, p.chunks, src, sideInfoSize(p.profile.versionID, p.profile.isMono()),
                                       maxReservoirBytes(p.profile.versionID), &breakAt);
    for (int i : breakAt) cout << "  RESERVOIR BREAK at chunk " << i << "\n";
    RunReport::set("resv_breaks", breaks);
    cout << "Reservoir model on the source order: " << breaks << " break(s)\n";
}

} // namespace

bool loadProblem(const Config& cfg, Problem& p) {
    Mp3FrameScanner scanner(cfg.input);
    if (scanner.getFrameCount() == 0) {
        cerr << "No frames found\n";
        RunReport::set("error", "no frames found");
        return false;
    }

    p.chunkSize = cfg.chunkSize;
    cout << "Chunk size: " << p.chunkSize << " bytes\n";
    p.chunks = scanner.chunksFromFirstFrame(p.chunkSize);

    int cbrKbps = 0;
    const bool cbr = detectCbr(p.chunks, cfg, cbrKbps);
    shuffleChunks(p, cfg);
    if (!cfg.startPinned && !placeStart(p)) return false;

    cout << "Total chunks: " << p.chunks.size() << "\n";
    RunReport::set("chunk_size", static_cast<long long>(p.chunkSize));
    RunReport::set("seed", static_cast<long long>(cfg.seed));
    RunReport::set("chunks", p.chunks.size());
    cout << "Shuffle seed: " << cfg.seed << (cfg.randomSeed ? " (random)" : " (fixed default; RP_SEED to override)") << "\n";

    if (!deriveStreamProfile(p, cbr, cbrKbps)) return false;
    computeMetas(p);
    checkSourceOrder(p);
    return true;
}
