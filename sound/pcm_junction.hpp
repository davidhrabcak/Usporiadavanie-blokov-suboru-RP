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

} // namespace PcmJunction

#endif
