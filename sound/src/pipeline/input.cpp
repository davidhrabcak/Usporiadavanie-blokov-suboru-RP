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

// Constant-bitrate detection (A3a): if every frame shares one bitrate, enable the frame-header
// bitrate filter in StreamProfile::matches so false-positive syncs at other bitrates are
// rejected. Order-independent (detectCBRUnordered): the first chunk is unknown.
bool detectCbr(const vector<vector<uint8_t>>& chunks, const Config& cfg, int& kbps) {
    bool cbr = false;
    kbps = 0;
    {
        StreamProfile tmp{};
        if (voteProfile(chunks, tmp)) cbr = detectCBRUnordered(chunks, tmp, kbps);
    }
    if (!cfg.cbrFilter) cbr = false; // A/B toggle for A3a
    if (cbr)
        cout << "Detected constant bitrate: " << kbps
             << " kbps - frame-header bitrate filter enabled\n";
    return cbr;
}

// Frames longer than the chunk: promotes the split-tail chunks, then moves the headerless chunks
// from p.chunks into p.pool (RP_GAP=0 disables). `origMetas` follows p.chunks.
void takeOutHeaderless(Problem& p, vector<ChunkMeta>& origMetas, const StreamProfile& prof0, const uint8_t* proto) {
    vector<vector<uint8_t>>& chunks = p.chunks;
    const SplitTails st = promoteSplitTails(origMetas, chunks, p.chunkSize, prof0, proto);
    p.splitTails = st.promoted[1] + st.promoted[2] + st.promoted[3];
    if (p.splitTails > 0 || st.expected[1] + st.expected[2] + st.expected[3] > 0) {
        cout << "Split-tail chunks (frame end + 1-3 header bytes): promoted / expected by t:";
        for (int t = 1; t <= 3; ++t) cout << " " << t << ":" << st.promoted[t] << "/" << st.expected[t];
        cout << "\n";
        RunReport::set("split_tails", p.splitTails);
        RunReport::set("split_tails_expected", st.expected[1] + st.expected[2] + st.expected[3]);
    }
    vector<int> keep;
    for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
        (isHeaderless(origMetas[i], chunks[i].size(), p.chunkSize) ? p.poolFull : keep).push_back(i);
    if (!p.poolFull.empty()) {
        vector<vector<uint8_t>> kept;
        vector<ChunkMeta> keptMetas;
        for (int i : p.poolFull) p.pool.push_back(chunks[i]);
        for (int i : keep) {
            kept.push_back(std::move(chunks[i]));
            keptMetas.push_back(origMetas[i]);
            keptMetas.back().chunkIndex = static_cast<int>(keptMetas.size()) - 1;
        }
        chunks = std::move(kept);
        origMetas = std::move(keptMetas);
        p.keptFull = std::move(keep);
    }
}

// The true-order diagnostic: canFollow / canFollowGap on every consecutive pair, then the
// reservoir model over the whole order. Sets p.truth.
void checkTrueOrder(Problem& p, vector<ChunkMeta>& origMetas, const StreamProfile& prof0) {
    const vector<vector<uint8_t>>& chunks = p.chunks;
    const StructuralGraph origGraph = buildStructuralGraph(origMetas, p.pool, p.chunkSize);
    const GapSlots& gaps0 = origGraph.gaps;
    if (!p.pool.empty()) {
        cout << "Headerless chunks (frames > chunk): " << p.pool.size() << " taken out; demanders "
             << gaps0.demanders() << " (" << gaps0.case1 << " whole header, " << gaps0.case2
             << " split header" << (gaps0.ambiguous ? ", " + std::to_string(gaps0.ambiguous) + " ambiguous left out" : string())
             << ") - count check " << ((int)p.pool.size() == gaps0.demanders() ? "OK" : "MISMATCH") << "\n";
        RunReport::set("pool", p.pool.size());
        RunReport::set("demanders", gaps0.demanders());
        RunReport::set("demanders_ambiguous", gaps0.ambiguous);
        RunReport::set("count_check", (int)p.pool.size() == gaps0.demanders());
    }
    int broken = 0;
    for (int i = 0; i + 1 < static_cast<int>(origMetas.size()); ++i) {
        const bool follows = origMetas[i].gapAfter ? canFollowGap(origMetas[i + 1], gaps0.offsets[i])
                                                   : canFollow(origMetas[i], origMetas[i + 1]);
        if (!follows) {
            cout << "  BREAK at " << i << "->" << i+1
                 << ": tailOvfl=" << origMetas[i].tailOverflow
                 << " tailPartialLen=" << origMetas[i].tailPartialLen
                 << " headOff=" << origMetas[i+1].headOffset << "\n";
            ++broken;
        }
    }
    p.truth.breaks = broken;
    RunReport::set("pre_breaks", broken);
    if (broken == 0)
        cout << "Pre-shuffle check PASSED: all " << chunks.size()
             << " consecutive pairs compatible\n";
    else
        cout << "Pre-shuffle check FAILED: " << broken << " breaks\n";

    // Validate the part2_3_length-based reservoir model against the true, unshuffled order
    // before trusting it inside the DFS - the same technique that caught the earlier failed
    // mainDataBegin-as-consumption attempt (see advanceReservoirFrame). A correct model must
    // never reject a real chunk sequence.
    if (prof0.isLayerIII()) {
        vector<int> trueOrder(origMetas.size()), breakAt;
        for (size_t i = 0; i < trueOrder.size(); ++i) trueOrder[i] = static_cast<int>(i);
        const int breaks = reservoirBreaks(origMetas, chunks, trueOrder,
            sideInfoSize(prof0.versionID, prof0.isMono()), maxReservoirBytes(prof0.versionID), &breakAt);
        for (int i : breakAt) cout << "  RESERVOIR BREAK at chunk " << i << "\n";
        p.truth.reservoirBreaks = breaks;
        RunReport::set("resv_breaks", breaks);
        if (breaks == 0)
            cout << "Reservoir check PASSED on true order: all "
                 << chunks.size() << " chunks consistent\n";
        else
            cout << "Reservoir check FAILED on true order: "
                 << breaks << " breaks\n";
    }
}

// Before the shuffle. The pool is an input to the search, so nothing here may use the true
// first chunk: the vote profile (differs from chunk 0's at most in channelIdx, which matches()
// ignores - musiclib) and, as promoteSplitTails' `proto`, the header of any chunk that can be
// the start (only its constant bytes 0-1 and byte 3 are read).
void prepareTrueOrder(Problem& p, const bool cbr, const int cbrKbps, const Config& cfg) {
    StreamProfile prof0{};
    vector<StartCandidate> protoCands;
    if (voteProfile(p.chunks, prof0)) protoCands = findStartCandidates(p.chunks, prof0);
    if (protoCands.empty()) return;
    if (cbr) { prof0.isCBR = true; prof0.bitrateKbps = cbrKbps; }
    const uint8_t* proto = p.chunks[protoCands[0].index].data();
    vector<ChunkMeta> origMetas;
    origMetas.reserve(p.chunks.size());
    for (int i = 0; i < static_cast<int>(p.chunks.size()); ++i)
        origMetas.push_back(computeChunkMeta(i, p.chunks[i], prof0, static_cast<long>(p.chunks[i].size()) < p.chunkSize));
    if (cfg.gap) takeOutHeaderless(p, origMetas, prof0, proto);
    checkTrueOrder(p, origMetas, prof0);
}

// Shuffle seed. Deterministic by default (C1) so a given input always reconstructs the same way
// and results are reproducible; RP_SEED=<n> pins a specific one, RP_SEED=random draws from the
// clock. origIndexOf is shuffled with the same seed/engine as the chunks, so it stays in
// lock-step and lets us score the reconstruction against the true order at the end (C2).
void shuffleChunks(Problem& p, const Config& cfg) {
    const unsigned seed = cfg.seed;
    vector<vector<uint8_t>>& chunks = p.chunks;
    p.origIndexOf.resize(chunks.size());
    for (size_t i = 0; i < p.origIndexOf.size(); ++i) p.origIndexOf[i] = static_cast<int>(i);
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));
    shuffle(p.origIndexOf.begin() + 1, p.origIndexOf.end(), default_random_engine(seed));
    shuffle(p.pool.begin(), p.pool.end(), default_random_engine(seed + 1));
    shuffle(p.poolFull.begin(), p.poolFull.end(), default_random_engine(seed + 1));

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
    int invalidCount = 0;
    for (int i = 0; i < static_cast<int>(p.chunks.size()); ++i) {
        p.metas.push_back(computeChunkMeta(i, p.chunks[i], p.profile, p.chunks[i].size() < p.chunks[0].size()));
        if (!p.metas.back().valid) {
            cerr << "  Chunk " << i << ": INVALID (no consistent frame run found)\n";
            ++invalidCount;
        }
    }
    if (invalidCount > 0)
        cerr << invalidCount << " invalid chunks — reconstruction may fail\n";
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
    if (p.chunkSize != Config::kDefaultChunkSize) cout << "Chunk size: " << p.chunkSize << " bytes\n";
    p.chunks = scanner.chunksFromFirstFrame(p.chunkSize);

    int cbrKbps = 0;
    const bool cbr = detectCbr(p.chunks, cfg, cbrKbps);
    prepareTrueOrder(p, cbr, cbrKbps, cfg);
    shuffleChunks(p, cfg);
    if (!cfg.startPinned && !placeStart(p)) return false;

    cout << "Total chunks: " << p.chunks.size() << "\n";
    RunReport::set("chunk_size", static_cast<long long>(p.chunkSize));
    RunReport::set("seed", static_cast<long long>(cfg.seed));
    RunReport::set("chunks", p.chunks.size());
    cout << "Shuffle seed: " << cfg.seed << (cfg.randomSeed ? " (random)" : " (fixed default; RP_SEED to override)") << "\n";

    if (!deriveStreamProfile(p, cbr, cbrKbps)) return false;
    computeMetas(p);
    return true;
}

void printChunkTable(const vector<ChunkMeta>& metas) {
    cout << "\nChunk metadata:\n";
    cout << "  idx  headOff  tailOvfl  frames\n";
    for (int i = 0; i < static_cast<int>(metas.size()); ++i) {
        cout << "  " << i
             << "  " << metas[i].headOffset
             << "  " << metas[i].tailOverflow
             << "  " << metas[i].frames.size()
             << (metas[i].valid ? "" : "  INVALID") << "\n";
    }
}
