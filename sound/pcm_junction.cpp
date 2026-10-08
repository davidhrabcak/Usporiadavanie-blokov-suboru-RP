#include "pcm_junction.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#include "Header.hpp"
#include "frame_scanner.hpp"   // packHeaderBytes

#define MINIMP3_FLOAT_OUTPUT
#include "third_party/minimp3.h"

namespace PcmJunction {

Window decodeWindow(const std::vector<const std::vector<uint8_t>*>& segs, int boundarySeg,
                    int startOffset) {
    Window w;
    std::vector<uint8_t> buf;
    for (int s = 0; s < (int)segs.size(); ++s) {
        if (s == boundarySeg) w.boundaryByte = (long)buf.size();
        buf.insert(buf.end(), segs[s]->begin(), segs[s]->end());
    }
    if (boundarySeg >= (int)segs.size()) w.boundaryByte = (long)buf.size();

    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME);

    long pos = startOffset;
    bool started = false; // first frame with output seen - the timeline starts there
    while (pos + 4 <= (long)buf.size()) {
        const uint32_t raw = packHeaderBytes(&buf[pos]);
        if (!Mp3FrameScanner::isValidHeader(raw)) break; // (Header's ctor would log to stderr)
        bool ok = false;
        Header h(raw, ok);
        if (!ok) break;
        const int len = h.getFrameLength();
        if (len <= 4 || pos + len > (long)buf.size()) break;

        // Feed exactly one frame: minimp3's `frame_size == mp3_bytes` path, so it never has to
        // resync on the following header (which would reset the reservoir/overlap state).
        mp3dec_frame_info_t info;
        std::memset(&info, 0, sizeof(info));
        const int ns = mp3dec_decode_frame(&dec, &buf[pos], len, pcm.data(), &info);
        if (info.hz == 0 || info.frame_offset != 0) break; // decoder didn't accept the frame

        FrameRec fr;
        fr.byteStart = pos;
        fr.byteEnd   = pos + len;
        const bool isPost = fr.byteEnd > w.boundaryByte;
        if (isPost && w.junctionFrame < 0) w.junctionFrame = (int)w.frames.size();

        if (ns > 0) {
            if (!started) { started = true; w.channels = info.channels; w.hz = info.hz; }
            fr.sampleStart = (int)w.mono.size();
            fr.samples = ns;
            for (int i = 0; i < ns; ++i) {
                float m = 0;
                for (int c = 0; c < info.channels; ++c) m += pcm[i * info.channels + c];
                w.mono.push_back(m / info.channels);
            }
            w.interleaved.insert(w.interleaved.end(), pcm.begin(), pcm.begin() + ns * info.channels);
        } else if (started) {
            // Decoded nothing (reservoir underflow). Keep the timeline: a real player would
            // output silence/garbage here, and the gap itself is a discontinuity.
            const int spf = (h.getVersionID() == 0b11) ? 1152 : 576;
            fr.sampleStart = (int)w.mono.size();
            w.mono.insert(w.mono.end(), spf, 0.0f);
            w.interleaved.insert(w.interleaved.end(), (size_t)spf * std::max(1, w.channels), 0.0f);
            if (isPost) ++w.postFailed;
        }
        if (w.junctionFrame == (int)w.frames.size()) w.junctionSample = fr.sampleStart;
        w.frames.push_back(fr);
        pos += len;
    }
    w.valid = started && w.junctionSample > 0;
    return w;
}

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

// Log power spectrum (dB, NFFT/2 bins) of mono[start, start+NFFT), Hann-windowed.
static std::vector<double> logSpec(const std::vector<float>& x, int start) {
    static std::vector<double> hann;
    if (hann.empty()) {
        hann.resize(NFFT);
        for (int i = 0; i < NFFT; ++i) hann[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (NFFT - 1));
    }
    std::vector<std::complex<double>> a(NFFT);
    for (int i = 0; i < NFFT; ++i) a[i] = x[start + i] * hann[i];
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

static double peakAbs(const std::vector<float>& x, int a, int b) {
    a = std::max(a, 0); b = std::min(b, (int)x.size());
    double p = 0;
    for (int i = a; i < b; ++i) p = std::max(p, (double)std::fabs(x[i]));
    return p;
}

Metrics junctionMetrics(const Window& w) {
    Metrics m;
    if (!w.valid) return m;
    m.postFailed = w.postFailed;
    const int J = w.junctionSample;
    const int N = (int)w.mono.size();
    const auto& x = w.mono;

    // Window grid aligned so window t covers [J + t*HOP - NFFT, J + t*HOP): t <= 0 is pure
    // pre-junction audio, t >= 1 overlaps the candidate's output.
    auto winStart = [&](int t) { return J + t * HOP - NFFT; };
    int tMin = 0;
    while (winStart(tMin - 1) >= 0) --tMin;
    int tMax = 0;
    while (winStart(tMax + 1) + NFFT <= N) ++tMax;
    if (tMax < 2 || tMin > -2) return m; // need >= 3 pre windows and 2 post windows

    const double preRms = rms(x, J - 2 * HOP, J);
    if (preRms < 1e-4) { m.silent = true; }

    std::vector<std::vector<double>> L;
    for (int t = tMin; t <= tMax; ++t) L.push_back(logSpec(x, winStart(t)));
    auto at = [&](int t) -> const std::vector<double>& { return L[t - tMin]; };

    // flux
    std::vector<double> preFlux;
    for (int t = tMin + 1; t <= 0; ++t) preFlux.push_back(meanAbsDiff(at(t), at(t - 1)));
    std::sort(preFlux.begin(), preFlux.end());
    const double base = std::max(preFlux[preFlux.size() / 2], 0.5);
    double postMax = 0;
    for (int t = 1; t <= tMax; ++t) postMax = std::max(postMax, meanAbsDiff(at(t), at(t - 1)));
    m.flux = postMax / base;

    // specDist: last 2 pre windows vs first 2 fully-post windows (window t is fully post once
    // t*HOP >= NFFT, i.e. t >= 2 for NFFT=1024/HOP=576).
    const int tp0 = (NFFT + HOP - 1) / HOP;
    if (tp0 + 1 <= tMax) {
        double s = 0;
        const size_t nb = at(0).size();
        for (size_t k = 0; k < nb; ++k) {
            const double pre  = 0.5 * (at(-1)[k] + at(0)[k]);
            const double post = 0.5 * (at(tp0)[k] + at(tp0 + 1)[k]);
            s += (pre - post) * (pre - post);
        }
        m.specDist = std::sqrt(s / nb);
    } else {
        m.specDist = meanAbsDiff(at(0), at(tMax));
    }

    // rmsJump: last pre granule vs each of the first two post granules
    const double eps = 1e-5;
    const double r0 = std::log(rms(x, J - HOP, J) + eps);
    double jump = 0;
    for (int g = 0; g < 2; ++g) {
        if (J + (g + 1) * HOP > N) break;
        jump = std::max(jump, std::fabs(std::log(rms(x, J + g * HOP, J + (g + 1) * HOP) + eps) - r0));
    }
    m.rmsJump = jump;

    // hfBurst: energy above ~11 kHz (bins > NFFT/4 at 44.1/48 kHz), pre vs post windows
    auto hfEnergy = [&](const std::vector<double>& Ls) {
        double e = 0;
        for (size_t k = Ls.size() / 2; k < Ls.size(); ++k) e += std::pow(10.0, Ls[k] / 10.0);
        return e;
    };
    double hfPre = 0, hfPost = 0;
    hfPre  = hfEnergy(at(-1)) + hfEnergy(at(0));
    hfPost = hfEnergy(at(1)) + hfEnergy(at(2));
    m.hfBurst = std::max(0.0, std::log((hfPost + 1) / (hfPre + 1)));

    // peak
    const double pPre = peakAbs(x, J - 2 * HOP, J), pPost = peakAbs(x, J, J + 2 * HOP);
    m.peak = std::max(0.0, std::log((pPost + 1e-4) / (pPre + 1e-4)));

    m.valid = true;
    return m;
}

Metrics scoreJunction(const std::vector<const std::vector<uint8_t>*>& segs, int boundarySeg,
                      int startOffset) {
    return junctionMetrics(decodeWindow(segs, boundarySeg, startOffset));
}

} // namespace PcmJunction
