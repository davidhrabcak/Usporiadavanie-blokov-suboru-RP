#ifndef RP_PCM_JUNCTION_HPP
#define RP_PCM_JUNCTION_HPP

#include <cstdint>
#include <vector>

// Full-decode junction scoring (HOW_IT_WORKS 4.7). Decodes a short window of chunks all the way
// to PCM with the vendored minimp3 (requantisation, stereo, IMDCT, synthesis) and scores how
// continuous the audio is across the boundary between the left side (the path so far) and the
// candidate chunk on the right.
//
// Unlike the partial decoder (main_data_decoder), this decodes the frame that STRADDLES the
// chunk boundary - and the following frames whose bit-reservoir pointer reaches back across it -
// exactly as a real player would hear them if the candidate successor were placed there.

namespace PcmJunction {

// Window extent, independent of the chunk size. Decoding starts at the latest frame that leaves
// >= PRE_BYTES and >= PRE_FRAMES whole frames before the boundary (warm-up for the reservoir,
// IMDCT overlap and synthesis filterbank, plus the pre-side flux baseline), and stops once the
// decoded frames reach >= POST_BYTES past the boundary and >= POST_FRAMES junction-or-later
// frames. Without the cap, large chunks (4-8 KB) made every decode several times dearer and let
// the post-side max-flux run over seconds of natural music transients, diluting the junction
// spike.
constexpr long PRE_BYTES   = 3072;
constexpr int  PRE_FRAMES  = 8;
constexpr long POST_BYTES  = 1024;
constexpr int  POST_FRAMES = 4;

// Left-side chunks a window needs to be measurable at all. The analysis wants >= 3 pre-junction
// FFT windows (NFFT + 2 hops = 2176 samples), i.e. ~2 decoded frames after a warm-up frame whose
// reservoir reaches back before the window, plus the partial frame the window starts in: ~4
// frame lengths. True-order coverage at 1 KB chunks (2026-10-26): 192 kbps 3 chunks 99%, 2
// chunks 30%; 320 kbps / 48 kHz 4 chunks 99.7%. Callers keep their own minimum (the DFS 3, edge
// locking 2) on top of this.
inline int leftChunksNeeded(int frameLen, long chunkSize) {
    return static_cast<int>((4L * frameLen + chunkSize - 1) / chunkSize);
}

// `valid` is false when there isn't enough decoded audio on both sides to measure anything.
struct Metrics {
    bool   valid  = false;
    bool   silent = false;   // pre side is digitally (near-)silent - nothing to compare
    double flux   = 0;       // peak post-side spectral flux / median pre-side flux (lower = more continuous)
    int    postFailed = 0;   // junction-or-later frames that produced no output (reservoir underflow)
};

// The left side of a window, decoded and analysed once, for scoring many candidates.
// `left` are the chunk byte arrays in placement order, decoded as one continuous byte stream
// from the frame header at `startOffset` in left[0], with a fresh decoder; frames are walked by
// their own header lengths and fed to the decoder one at a time. Everything before the junction
// frame (the frame straddling the boundary) depends only on the left chunks: the decoder state
// after them, the last pre-junction samples and spectrum, the flux baseline. score(cand) resumes
// a copy of that decoder at the junction frame and only decodes and analyses the post side.
class LeftContext {
public:
    LeftContext(const std::vector<const std::vector<uint8_t>*>& left, int startOffset);
    ~LeftContext();
    LeftContext(const LeftContext&) = delete;
    LeftContext& operator=(const LeftContext&) = delete;
    Metrics score(const std::vector<uint8_t>& cand) const;
private:
    struct Impl;
    Impl* p_;
};

} // namespace PcmJunction

#endif
