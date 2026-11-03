#ifndef RP_PCM_JUNCTION_HPP
#define RP_PCM_JUNCTION_HPP

#include <cstdint>
#include <vector>

// Full-decode junction scoring (PLAN_full_pcm_decode.md). Decodes a short window of chunks
// all the way to PCM with the vendored minimp3 (requantisation, stereo, IMDCT, synthesis) and
// scores how continuous the audio is across the boundary between the left and right side.
//
// Unlike Milestone 2 (main_data_decoder), this decodes the frame that STRADDLES the chunk
// boundary - and the following frames whose bit-reservoir pointer reaches back across it -
// exactly as a real player would hear them if the candidate successor were placed there.

namespace PcmJunction {

// One frame of the window, in decode order.
struct FrameRec {
    long byteStart = 0;     // offset of the frame header in the concatenated window
    long byteEnd   = 0;     // one past the frame's last byte
    int  sampleStart = -1;  // start of this frame's output in Window::mono (-1: not decoded)
    int  samples   = 0;     // per-channel samples produced (0: reservoir underflow etc.)
};

struct Window {
    bool  valid = false;
    int   channels = 0;
    int   hz = 0;
    std::vector<float> mono;         // decoded output, channel-averaged, roughly [-1, 1]
    std::vector<float> interleaved;  // raw decoder output (for the correctness gate)
    std::vector<FrameRec> frames;
    long  boundaryByte = 0;          // byte offset where the right side starts
    int   junctionFrame  = -1;       // index into frames: first frame ending past boundaryByte
    int   junctionSample = -1;       // where the junction frame's output starts in `mono`
    int   postFailed = 0;            // junction-or-later frames that produced no output
};

// Window extent, independent of the chunk size. Decoding starts at the latest frame that leaves
// >= PRE_BYTES and >= PRE_FRAMES whole frames before the boundary (warm-up for the reservoir,
// IMDCT overlap and synthesis filterbank, plus the pre-side flux baseline), and stops once the
// decoded frames reach >= POST_BYTES past the boundary and >= POST_FRAMES junction-or-later
// frames. At 1 KB chunks (3 left chunks + 1 candidate) this is exactly the whole window, i.e.
// the configuration the correctness gate validated. Without the cap, large chunks (4-8 KB)
// made every decode several times dearer and let the post-side max-flux run over seconds of
// natural music transients, diluting the junction spike.
constexpr long PRE_BYTES   = 3072;
constexpr int  PRE_FRAMES  = 8;
constexpr long POST_BYTES  = 1024;
constexpr int  POST_FRAMES = 4;

// Left-side chunks a window needs to be measurable at all. The analysis wants >= 3 pre-junction
// FFT windows (NFFT + 2 hops = 2176 samples), i.e. ~2 decoded frames after a warm-up frame whose
// reservoir reaches back before the window, plus the partial frame the window starts in: ~4
// frame lengths. True-order coverage at 1 KB chunks (2026-10-26): 192 kbps 3 chunks 99%, 2
// chunks 30%; 320 kbps / 48 kHz 4 chunks 99.7%; 320 kbps / 44.1 kHz 3 chunks 0-0.2%, 5 chunks
// 95-98%. Callers keep their own minimum (the DFS 3, edge locking 2) on top of this.
inline int leftChunksNeeded(int frameLen, long chunkSize) {
    return static_cast<int>((4L * frameLen + chunkSize - 1) / chunkSize);
}

// Decodes `segs` (chunk byte arrays, in placement order) as one continuous byte stream,
// starting at the frame header at `startOffset` in segs[0], with a fresh decoder. The first
// `boundarySeg` segments are the left side (already-placed path) and the rest the right side
// (the candidate). Frames are walked by their own header lengths and fed to the decoder one
// at a time; walking stops at the first invalid header or a frame that runs past the end.
Window decodeWindow(const std::vector<const std::vector<uint8_t>*>& segs, int boundarySeg,
                    int startOffset);

// Junction continuity metrics, all "lower = more continuous". `valid` is false when there
// isn't enough decoded audio on both sides to measure anything.
struct Metrics {
    bool   valid  = false;
    bool   silent = false;   // pre side is digitally (near-)silent - nothing to compare
    double flux     = 0;     // peak post-side spectral flux / median pre-side flux
    double specDist = 0;     // RMS log-spectrum distance, last 2 pre windows vs first 2 post
    double rmsJump  = 0;     // max |delta log RMS| across the junction, per granule
    double hfBurst  = 0;     // log ratio of >11 kHz energy post vs pre (positive part)
    double peak     = 0;     // log ratio of peak |sample| post vs pre (positive part)
    int    postFailed = 0;
};
Metrics junctionMetrics(const Window& w);

// Convenience: decodeWindow + junctionMetrics.
Metrics scoreJunction(const std::vector<const std::vector<uint8_t>*>& segs, int boundarySeg,
                      int startOffset);

// The left side of a window, decoded and analysed once, for scoring many candidates.
// Everything before the junction frame (the frame straddling the boundary) depends only on the
// left chunks: the decoder state after them, the pre-junction samples and spectra, the flux
// baseline. score(cand) resumes a copy of that decoder at the junction frame and only decodes
// and analyses the post side. score(cand) is bit-identical to
// scoreJunction({left..., &cand}, left.size(), startOffset) - just 2-4x cheaper per candidate
// (the pre side is >= 3 KB / 8 frames, the post side ~1 KB / 4 frames).
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
