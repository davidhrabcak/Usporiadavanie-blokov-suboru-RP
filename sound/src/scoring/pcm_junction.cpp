#include "scoring/pcm_junction.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <utility>

#include "mp3/Header.hpp"

#define MINIMP3_FLOAT_OUTPUT
#include "third_party/minimp3.h"

namespace PcmJunction {

// ---------------------------------------------------------------------------------------
// Spectral analysis

static constexpr int NFFT = 1024;
static constexpr int HOP  = 576;   // one granule

static void fft(std::vector<std::complex<double>>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> wv(1);
            for (int k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * wv;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wv *= wl;
            }
        }
    }
}

// Log power spectrum (dB, NFFT/2 bins) of x[0, NFFT), Hann-windowed.
static std::vector<double> logSpecAt(const float* x) {
    static const std::vector<double> hann = [] {   // initialised once, also under threads
        std::vector<double> h(NFFT);
        for (int i = 0; i < NFFT; ++i) h[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (NFFT - 1));
        return h;
    }();
    std::vector<std::complex<double>> a(NFFT);
    for (int i = 0; i < NFFT; ++i) a[i] = x[i] * hann[i];
    fft(a);
    std::vector<double> L(NFFT / 2);
    for (int k = 0; k < NFFT / 2; ++k) L[k] = 10.0 * std::log10(std::norm(a[k]) + 1e-3);
    return L;
}

static double meanAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0;
    for (size_t k = 0; k < a.size(); ++k) s += std::fabs(a[k] - b[k]);
    return s / a.size();
}

static double rms(const std::vector<float>& x, int a, int b) {
    a = std::max(a, 0); b = std::min(b, (int)x.size());
    if (b <= a) return 0;
    double s = 0;
    for (int i = a; i < b; ++i) s += (double)x[i] * x[i];
    return std::sqrt(s / (b - a));
}

// ---------------------------------------------------------------------------------------
// LeftContext. Sample grid: window t covers [J + t*HOP - NFFT, J + t*HOP), J = the junction
// sample (first output sample of the junction frame). t <= 0 is pure pre-junction audio, t >= 1
// overlaps the candidate's output. Flux = max over t >= 1 of the mean |dB difference| between
// windows t and t-1, divided by the median of the same over the pre side (floored at 0.5).

struct LeftContext::Impl {
    bool dead = false;                 // no candidate can give a valid window (Metrics{})
    std::vector<uint8_t> tail;         // left bytes from the junction frame's header on
    long boundary = 0;                 // boundary offset within `tail`
    mp3dec_t dec;                      // decoder state after the pre frames
    int channels = 0;
    int J = 0;                         // pre samples = junction sample
    std::vector<float> preTail;        // last NFFT pre samples
    // pre-side analysis (valid only when J allows >= 3 pre windows)
    bool preOk = false;
    bool silent = false;
    std::vector<double> lastPre;       // log spectrum of window t = 0
    double base = 0;                   // median pre-side flux
};

LeftContext::LeftContext(const std::vector<const std::vector<uint8_t>*>& left, int startOffset)
    : p_(new Impl) {
    Impl& I = *p_;
    std::vector<uint8_t> buf;
    for (const auto* s : left) buf.insert(buf.end(), s->begin(), s->end());
    const long Lb = (long)buf.size();

    // header walk over the frames lying wholly in the left side
    std::vector<std::pair<long, int>> walk;
    long pos = startOffset;
    while (pos + 4 <= Lb) {
        Header h;
        if (!Header::parse(&buf[pos], h)) { I.dead = true; break; }
        const int len = h.getFrameLength();
        if (len <= 4) { I.dead = true; break; }
        if (pos + len > Lb) break;          // the junction frame: straddles the boundary
        walk.push_back({pos, len});
        pos += len;
    }
    if (I.dead) return;
    const size_t jf = walk.size();
    I.tail.assign(buf.begin() + pos, buf.end());
    I.boundary = Lb - pos;

    size_t first = 0;
    for (size_t i = jf; i-- > 0;)
        if (Lb - walk[i].first >= PRE_BYTES && jf - i >= (size_t)PRE_FRAMES) { first = i; break; }

    mp3dec_init(&I.dec);
    std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME), mono;
    bool started = false;
    for (size_t wi = first; wi < jf; ++wi) {
        const long fp = walk[wi].first;
        const int len = walk[wi].second;
        Header h;
        Header::parse(&buf[fp], h);   // the walk above already accepted this frame
        mp3dec_frame_info_t info;
        std::memset(&info, 0, sizeof(info));
        const int ns = mp3dec_decode_frame(&I.dec, &buf[fp], len, pcm.data(), &info);
        if (info.hz == 0 || info.frame_offset != 0) { I.dead = true; return; }  // walk stops before the junction
        if (ns > 0) {
            if (!started) { started = true; I.channels = info.channels; }
            for (int i = 0; i < ns; ++i) {
                float m = 0;
                for (int c = 0; c < info.channels; ++c) m += pcm[i * info.channels + c];
                mono.push_back(m / info.channels);
            }
        } else if (started) {
            const int spf = (h.getVersionID() == 0b11) ? 1152 : 576;
            mono.insert(mono.end(), spf, 0.0f);
        }
    }
    // junctionSample > 0 needs output before the junction frame
    if (!started || mono.empty()) { I.dead = true; return; }
    I.J = (int)mono.size();
    const int J = I.J;

    auto winStart = [&](int t) { return J + t * HOP - NFFT; };
    int tMin = 0;
    while (winStart(tMin - 1) >= 0) --tMin;
    if (tMin > -2) return;   // preOk stays false: every candidate gets an invalid (but decoded) window
    I.preOk = true;
    I.preTail.assign(mono.end() - NFFT, mono.end());
    I.silent = rms(mono, J - 2 * HOP, J) < 1e-4;
    std::vector<std::vector<double>> L;   // windows t = tMin..0
    for (int t = tMin; t <= 0; ++t) L.push_back(logSpecAt(&mono[winStart(t)]));
    std::vector<double> preFlux;
    for (size_t k = 1; k < L.size(); ++k) preFlux.push_back(meanAbsDiff(L[k], L[k - 1]));
    std::sort(preFlux.begin(), preFlux.end());
    I.base = std::max(preFlux[preFlux.size() / 2], 0.5);
    I.lastPre = std::move(L.back());
}

LeftContext::~LeftContext() { delete p_; }

Metrics LeftContext::score(const std::vector<uint8_t>& cand) const {
    const Impl& I = *p_;
    Metrics m;
    if (I.dead) return m;
    std::vector<uint8_t> buf(I.tail);
    buf.insert(buf.end(), cand.begin(), cand.end());

    // header walk from the junction frame on, with the post-side stop rule
    std::vector<std::pair<long, int>> walk;
    for (long pos = 0; pos + 4 <= (long)buf.size();) {
        Header h;
        if (!Header::parse(&buf[pos], h)) break;
        const int len = h.getFrameLength();
        if (len <= 4 || pos + len > (long)buf.size()) break;
        walk.push_back({pos, len});
        pos += len;
    }
    size_t last = walk.size();
    for (size_t i = 0; i < walk.size(); ++i)
        if (walk[i].first + walk[i].second - I.boundary >= POST_BYTES && i + 1 >= (size_t)POST_FRAMES) { last = i + 1; break; }
    if (last == 0) return m;   // no junction frame

    mp3dec_t dec = I.dec;
    std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);
    // x = last NFFT pre samples followed by the post samples; sample s of the full window is
    // x[s - (J - NFFT)].
    std::vector<float> x(I.preTail);
    int postFailed = 0;
    for (size_t wi = 0; wi < last; ++wi) {
        const long fp = walk[wi].first;
        const int len = walk[wi].second;
        Header h;
        Header::parse(&buf[fp], h);   // the walk above already accepted this frame
        mp3dec_frame_info_t info;
        std::memset(&info, 0, sizeof(info));
        const int ns = mp3dec_decode_frame(&dec, &buf[fp], len, pcm.data(), &info);
        if (info.hz == 0 || info.frame_offset != 0) {
            if (wi == 0) return m;   // junction frame not decoded -> no junction sample
            break;
        }
        if (ns > 0) {
            for (int i = 0; i < ns; ++i) {
                float s = 0;
                for (int c = 0; c < info.channels; ++c) s += pcm[i * info.channels + c];
                x.push_back(s / info.channels);
            }
        } else {
            const int spf = (h.getVersionID() == 0b11) ? 1152 : 576;
            x.insert(x.end(), spf, 0.0f);
            ++postFailed;
        }
    }
    m.postFailed = postFailed;
    if (!I.preOk) return m;
    const int J = I.J, off = J - NFFT;               // full-window sample s -> x[s - off]
    const int N = off + (int)x.size();
    auto winStart = [&](int t) { return J + t * HOP - NFFT; };
    int tMax = 0;
    while (winStart(tMax + 1) + NFFT <= N) ++tMax;
    if (tMax < 2) return m;
    m.silent = I.silent;

    double postMax = 0;
    std::vector<double> prev = I.lastPre;
    for (int t = 1; t <= tMax; ++t) {
        std::vector<double> cur = logSpecAt(&x[winStart(t) - off]);
        postMax = std::max(postMax, meanAbsDiff(cur, prev));
        prev = std::move(cur);
    }
    m.flux = postMax / I.base;
    m.valid = true;
    return m;
}

} // namespace PcmJunction
