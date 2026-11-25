// BoomBap Master - DSP core.
// Plain C++17, no JUCE dependency, so it can be unit-tested and reused by the
// offline command-line tool as well as the plugin.
//
// Signal chain (per sample, stereo):
//   input trim -> sub cut -> saturation -> 12-bit/26 kHz sampler stage
//   -> glue compressor -> tone EQ -> drift (wow/flutter) -> mono bass
//   -> [pre meter] -> level (manual / auto ride / locked) -> crackle
//   -> true-peak limiter -> [post meter]
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bb
{
constexpr double kPi = 3.14159265358979323846;

inline double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
inline double linToDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }
inline double clampd (double v, double lo, double hi) { return std::min (std::max (v, lo), hi); }

//==============================================================================
// Biquads (RBJ cookbook), transposed direct form II
struct BiquadCoefs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline BiquadCoefs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
{
    BiquadCoefs c;
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
    c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

inline BiquadCoefs makeLowpass (double fs, double f, double q)
{
    const double w = 2.0 * kPi * f / fs, cw = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al);
}

inline BiquadCoefs makeHighpass (double fs, double f, double q)
{
    const double w = 2.0 * kPi * f / fs, cw = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al);
}

inline BiquadCoefs makePeak (double fs, double f, double q, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * f / fs, cw = std::cos (w), al = std::sin (w) / (2.0 * q);
    return normalise (1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
}

inline BiquadCoefs makeHighShelf (double fs, double f, double gainDb)
{
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w = 2.0 * kPi * f / fs, cw = std::cos (w), sw = std::sin (w);
    const double al = sw / 2.0 * std::sqrt (2.0); // shelf slope S = 1
    const double beta = 2.0 * std::sqrt (A) * al;
    return normalise (A * ((A + 1) + (A - 1) * cw + beta),
                      -2 * A * ((A - 1) + (A + 1) * cw),
                      A * ((A + 1) + (A - 1) * cw - beta),
                      (A + 1) - (A - 1) * cw + beta,
                      2 * ((A - 1) - (A + 1) * cw),
                      (A + 1) - (A - 1) * cw - beta);
}

struct Biquad
{
    BiquadCoefs c;
    double z1 = 0, z2 = 0;

    inline double process (double x)
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

//==============================================================================
struct Rng
{
    uint32_t s = 2463534242u;
    void seed (uint32_t v) { s = v ? v : 1u; }
    inline uint32_t nextU() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline double uni() { return nextU() * (1.0 / 4294967296.0); }   // [0,1)
    inline double bi()  { return uni() * 2.0 - 1.0; }                // [-1,1)
};

struct Smooth
{
    double v = 0, a = 1;
    void setup (double ms, double fs) { a = 1.0 - std::exp (-1.0 / (0.001 * ms * fs)); }
    inline double next (double target) { v += a * (target - v); return v; }
};

// Two cascaded one-pole low-passes on white noise, normalised to unit variance.
// Used for natural-sounding slow random movement (no regular LFO).
struct NoiseLP
{
    Rng rng;
    double a = 0.1, g = 1.0, y1 = 0, y2 = 0;

    void setup (double fc, double fs, uint32_t seed)
    {
        rng.seed (seed);
        a = 1.0 - std::exp (-2.0 * kPi * fc / fs);
        const double r = (1.0 - a) * (1.0 - a);
        const double oneMinusR = a * (2.0 - a);                       // 1 - r, computed stably
        const double G = a * a * a * a * (1.0 + r) / (oneMinusR * oneMinusR * oneMinusR);
        g = 1.0 / std::sqrt (G / 3.0);                                // white uniform var = 1/3
        y1 = y2 = 0;
    }
    inline double next()
    {
        y1 += a * (rng.bi() - y1);
        y2 += a * (y1 - y2);
        return clampd (y2 * g, -3.5, 3.5);
    }
};

//==============================================================================
// ITU-R BS.1770 / EBU R128 loudness: K-weighting, 400 ms blocks, 100 ms hop,
// absolute (-70) and relative (-10 LU) gating via a histogram, so a running
// integrated value is cheap to compute at any moment.
class LoudnessMeter
{
public:
    void prepare (double fs)
    {
        hop_ = std::max (1, (int) std::lround (fs * 0.1));
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            BiquadCoefs c;
            c.b0 = (Vh + Vb * K / Q + K * K) / a0;
            c.b1 = 2.0 * (K * K - Vh) / a0;
            c.b2 = (Vh - Vb * K / Q + K * K) / a0;
            c.a1 = 2.0 * (K * K - 1.0) / a0;
            c.a2 = (1.0 - K / Q + K * K) / a0;
            s1L.c = s1R.c = c;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            BiquadCoefs c;
            c.b0 = 1.0; c.b1 = -2.0; c.b2 = 1.0;
            c.a1 = 2.0 * (K * K - 1.0) / a0;
            c.a2 = (1.0 - K / Q + K * K) / a0;
            s2L.c = s2R.c = c;
        }
        histE.assign (kBins, 0.0);
        histN.assign (kBins, 0);
        reset();
    }

    void reset()
    {
        s1L.reset(); s1R.reset(); s2L.reset(); s2R.reset();
        acc = 0; cnt = 0; ring.fill (0.0); p = 0; filled = 0; gatedBlocks = 0;
        std::fill (histE.begin(), histE.end(), 0.0);
        std::fill (histN.begin(), histN.end(), 0);
        momentary_ = shortTerm_ = -120.0;
    }

    // Clear the integrated history only (keeps filter state, so no glitch).
    void resetIntegrated()
    {
        std::fill (histE.begin(), histE.end(), 0.0);
        std::fill (histN.begin(), histN.end(), 0);
        gatedBlocks = 0;
    }

    inline void push (double l, double r)
    {
        const double a = s2L.process (s1L.process (l));
        const double b = s2R.process (s1R.process (r));
        acc += a * a + b * b;
        if (++cnt < hop_) return;

        ring[(size_t) p] = acc / hop_;
        acc = 0; cnt = 0;
        p = (p + 1) % kRing;
        if (filled < kRing) ++filled;

        if (filled >= 4)
        {
            double e = 0;
            for (int i = 1; i <= 4; ++i) e += ring[(size_t) ((p - i + kRing) % kRing)];
            e *= 0.25;
            momentary_ = toLufs (e);
            const int bin = (int) std::floor ((momentary_ + 70.0) * 10.0);
            if (bin >= 0)
            {
                const int b2 = std::min (bin, kBins - 1);
                histE[(size_t) b2] += e;
                histN[(size_t) b2] += 1;
                ++gatedBlocks;
            }
        }
        if (filled >= kRing)
        {
            double e = 0;
            for (double v : ring) e += v;
            shortTerm_ = toLufs (e / kRing);
        }
    }

    double momentaryLufs() const { return momentary_; }
    double shortTermLufs() const { return shortTerm_; }
    // seconds of programme that have passed the absolute gate
    double gatedSeconds() const { return gatedBlocks * 0.1; }

    double integratedLufs() const
    {
        double sumE = 0; long long n = 0;
        for (int i = 0; i < kBins; ++i) { sumE += histE[(size_t) i]; n += histN[(size_t) i]; }
        if (n == 0) return -120.0;
        const double gate = toLufs (sumE / (double) n) - 10.0;
        double s2 = 0; long long n2 = 0;
        for (int i = 0; i < kBins; ++i)
        {
            if ((double) i / 10.0 - 70.0 + 0.05 >= gate) { s2 += histE[(size_t) i]; n2 += histN[(size_t) i]; }
        }
        return n2 > 0 ? toLufs (s2 / (double) n2) : -120.0;
    }

    static double toLufs (double e) { return e > 1.0e-12 ? -0.691 + 10.0 * std::log10 (e) : -120.0; }

private:
    static constexpr int kBins = 1000, kRing = 30;
    Biquad s1L, s1R, s2L, s2R;
    int hop_ = 4800, cnt = 0, p = 0, filled = 0;
    long long gatedBlocks = 0;
    double acc = 0, momentary_ = -120.0, shortTerm_ = -120.0;
    std::array<double, kRing> ring {};
    std::vector<double> histE;
    std::vector<long long> histN;
};

//==============================================================================
// 8x oversampled true-peak estimator: 32-tap Kaiser-windowed sinc per phase.
// (4x under-reads peaks of high-frequency content by up to ~0.7 dB, 8x by ~0.17 dB.)
// push() returns the largest interpolated value around the sample that was
// pushed kDelay calls earlier.
inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 80; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < 1.0e-15 * sum) break;
    }
    return sum;
}

class TruePeakDetector
{
public:
    static constexpr int kTaps = 32;
    static constexpr int kOS = 8;
    static constexpr int kDelay = kTaps / 2;

    TruePeakDetector()
    {
        const double H = kTaps / 2.0, beta = 8.0, i0b = besselI0 (beta);
        for (int ph = 1; ph < kOS; ++ph)
        {
            const double f = (double) ph / kOS;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double t = (double) (k - (kDelay - 1)) - f;   // distance to the interpolated point
                const double sinc = std::sin (kPi * t) / (kPi * t);
                const double u = t / H;
                const double w = std::fabs (u) < 1.0 ? besselI0 (beta * std::sqrt (1.0 - u * u)) / i0b : 0.0;
                taps[(size_t) ph][(size_t) k] = sinc * w;
                sum += sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) taps[(size_t) ph][(size_t) k] /= sum;
        }
        hist.fill (0.0);
    }

    void reset() { hist.fill (0.0); pos = 0; }

    inline double push (double x)
    {
        hist[(size_t) pos] = x;
        pos = (pos + 1) & (kTaps - 1);
        double chron[kTaps];
        for (int k = 0; k < kTaps; ++k) chron[k] = hist[(size_t) ((pos + k) & (kTaps - 1))];
        double mx = std::fabs (chron[kDelay - 1]);
        for (int ph = 1; ph < kOS; ++ph)
        {
            double y = 0;
            for (int k = 0; k < kTaps; ++k) y += taps[(size_t) ph][(size_t) k] * chron[k];
            mx = std::max (mx, std::fabs (y));
        }
        return mx;
    }

private:
    std::array<std::array<double, kTaps>, kOS> taps {};
    std::array<double, kTaps> hist {};
    int pos = 0;
};

// Sliding-window minimum (monotonic queue on fixed arrays, no allocation while running)
class SlidingMin
{
public:
    void prepare (int windowSize)
    {
        W = std::max (1, windowSize);
        cap = W + 2;
        vals.assign ((size_t) cap, 1.0);
        idxs.assign ((size_t) cap, 0);
        reset();
    }
    void reset() { head = tail = 0; count = 0; n = 0; }

    inline double push (double v)
    {
        while (count > 0 && vals[(size_t) ((tail - 1 + cap) % cap)] >= v)
        {
            tail = (tail - 1 + cap) % cap;
            --count;
        }
        vals[(size_t) tail] = v;
        idxs[(size_t) tail] = n;
        tail = (tail + 1) % cap;
        ++count;
        while (idxs[(size_t) head] <= n - W)
        {
            head = (head + 1) % cap;
            --count;
        }
        ++n;
        return vals[(size_t) head];
    }

private:
    int W = 1, cap = 3, head = 0, tail = 0, count = 0;
    long long n = 0;
    std::vector<double> vals;
    std::vector<long long> idxs;
};

// Look-ahead true-peak limiter. Guarantees the oversampled peak stays at or
// under the ceiling. Fast attack via look-ahead + boxcar, slower release.
class TruePeakLimiter
{
public:
    void prepare (double fs)
    {
        N = std::max (8, (int) std::lround (0.002 * fs));   // 2 ms look-ahead
        D = N + TruePeakDetector::kDelay;                          // + detector delay
        dL.assign ((size_t) D, 0.0);
        dR.assign ((size_t) D, 0.0);
        box.assign ((size_t) (N + 1), 1.0);
        sm.prepare (N + 1);
        relCoef = 1.0 - std::exp (-1.0 / (0.080 * fs));
        reset();
    }

    void reset()
    {
        std::fill (dL.begin(), dL.end(), 0.0);
        std::fill (dR.begin(), dR.end(), 0.0);
        std::fill (box.begin(), box.end(), 1.0);
        boxSum = (double) (N + 1);
        bpos = 0; dpos = 0; mRel = 1.0; lastGain = 1.0;
        tpL.reset(); tpR.reset(); sm.reset();
    }

    // The limiter aims kMarginDb below the requested ceiling: gain ramps across the
    // interpolation window and the finite-length detector together can read ~0.05 dB low.
    static constexpr double kMarginDb = 0.1;
    void setCeilingDb (double db) { ceil = dbToLin (db - kMarginDb); }
    int latency() const { return D; }
    double lastGainDb() const { return linToDb (lastGain); }
    long long clampHits = 0;   // diagnostic: times the safety clamp had to act

    inline void process (double& l, double& r)
    {
        const double pk = std::max (tpL.push (l), tpR.push (r));
        const double req = pk > ceil ? ceil / pk : 1.0;
        const double m = sm.push (req);
        mRel = std::min (m, mRel + (1.0 - mRel) * relCoef);

        boxSum += mRel - box[(size_t) bpos];
        box[(size_t) bpos] = mRel;
        bpos = (bpos + 1) % (N + 1);
        const double g = std::min (1.0, boxSum / (double) (N + 1));
        lastGain = g;

        const double oL = dL[(size_t) dpos], oR = dR[(size_t) dpos];
        dL[(size_t) dpos] = l; dR[(size_t) dpos] = r;
        dpos = (dpos + 1) % D;

        // last-resort safety clamp (a no-op while the limiter is doing its job)
        if (std::fabs (oL * g) > ceil * 1.0000001 || std::fabs (oR * g) > ceil * 1.0000001) ++clampHits;
        l = clampd (oL * g, -ceil, ceil);
        r = clampd (oR * g, -ceil, ceil);
    }

private:
    int N = 96, D = 102, bpos = 0, dpos = 0;
    double ceil = 0.891, relCoef = 0.0003, mRel = 1.0, boxSum = 97.0, lastGain = 1.0;
    std::vector<double> dL, dR, box;
    TruePeakDetector tpL, tpR;
    SlidingMin sm;
};

//==============================================================================
// Anti-derivative anti-aliased tanh (first order ADAA)
struct AdaaTanh
{
    double x1 = 0, F1 = 0;
    bool primed = false;
    static double lncosh (double x)
    {
        const double ax = std::fabs (x);
        return ax + std::log1p (std::exp (-2.0 * ax)) - 0.6931471805599453;
    }
    inline double process (double x)
    {
        const double F = lncosh (x);
        if (! primed) { primed = true; x1 = x; F1 = F; }
        const double dx = x - x1;
        const double y = std::fabs (dx) < 1.0e-6 ? std::tanh (0.5 * (x + x1)) : (F - F1) / dx;
        x1 = x; F1 = F;
        return y;
    }
    void reset() { x1 = 0; F1 = 0; primed = false; }
};

struct Saturator
{
    AdaaTanh adaa;
    double dcx = 0, dcy = 0;
    inline double process (double x, double drive, double bias)
    {
        const double y = (adaa.process (x * drive + bias) - std::tanh (bias)) / drive;
        const double o = y - dcx + 0.9995 * dcy;   // DC blocker (bias makes a little DC)
        dcx = y; dcy = o;
        return o;
    }
    void reset() { adaa.reset(); dcx = dcy = 0; }
};

//==============================================================================
// SP-1200-style sampler stage: band-limit, hold at 26.04 kHz, 12-bit quantise,
// analogue-style reconstruction filter.
struct Sp12Channel
{
    Biquad aa1, aa2, aa3, recon;
    double acc = 0, held = 0, ratio = 0.5;

    void setup (double fs)
    {
        ratio = 26040.0 / fs;
        // 6th-order Butterworth anti-alias at 11.5 kHz (three sections). Steep enough that the
        // character stays the same at 44.1, 48, 88.2 and 96 kHz host rates.
        aa1.c = makeLowpass (fs, 11500.0, 0.5176);
        aa2.c = makeLowpass (fs, 11500.0, 0.7071);
        aa3.c = makeLowpass (fs, 11500.0, 1.9319);
        recon.c = makeLowpass (fs, 11500.0, 0.7071);
        reset();
    }
    void reset() { aa1.reset(); aa2.reset(); aa3.reset(); recon.reset(); acc = 0; held = 0; }

    static inline double quantise12 (double x)
    {
        const double q = std::floor (x * 2048.0 + 0.5) / 2048.0;
        return clampd (q, -1.0, 2047.0 / 2048.0);
    }

    inline double process (double x)
    {
        const double a = aa3.process (aa2.process (aa1.process (x)));
        acc += ratio;
        if (acc >= 1.0) { acc -= 1.0; held = quantise12 (a); }
        return recon.process (held);
    }
};

//==============================================================================
// Gentle stereo-linked glue compressor. Peak detector with a high-passed
// sidechain, soft knee, 30 ms attack, dual-stage "auto" release, and a soft
// ceiling on gain reduction so it can never be overdone.
class GlueComp
{
public:
    void prepare (double fs)
    {
        scL.c = scR.c = makeHighpass (fs, 90.0, 0.7071);
        aA = std::exp (-1.0 / (0.030 * fs));
        rF = std::exp (-1.0 / (0.070 * fs));
        rS = std::exp (-1.0 / (0.450 * fs));
        reset();
    }
    void reset() { scL.reset(); scR.reset(); grF = grS = 0; lastGr = 0; }

    // returns the linear gain to apply
    inline double process (double l, double r, double threshDb, double ratio, double maxGrDb)
    {
        const double lev = std::max (std::fabs (scL.process (l)), std::fabs (scR.process (r)));
        const double over = linToDb (lev) - threshDb;
        const double W = 6.0;
        double x;
        if (2.0 * over < -W) x = 0.0;
        else if (2.0 * std::fabs (over) <= W) x = (over + W / 2) * (over + W / 2) / (2.0 * W);
        else x = over;
        const double target = x * (1.0 - 1.0 / ratio);

        grF = target > grF ? aA * grF + (1 - aA) * target : rF * grF + (1 - rF) * target;
        grS = target > grS ? aA * grS + (1 - aA) * target : rS * grS + (1 - rS) * target;
        double gr = 0.5 * (grF + grS);
        gr = maxGrDb * std::tanh (gr / maxGrDb);
        lastGr = gr;
        return dbToLin (-gr);
    }
    double lastGrDb() const { return lastGr; }

private:
    Biquad scL, scR;
    double aA = 0.99, rF = 0.99, rS = 0.99, grF = 0, grS = 0, lastGr = 0;
};

//==============================================================================
// Fractional delay line, 4-point Hermite interpolation
struct DelayLine
{
    std::vector<double> buf;
    int mask = 0, w = 0;
    void prepare (int pow2Size) { buf.assign ((size_t) pow2Size, 0.0); mask = pow2Size - 1; w = 0; }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0); w = 0; }

    inline double process (double x, double delay)
    {
        buf[(size_t) w] = x;
        const double p = (double) w - delay;
        const double fl = std::floor (p);
        const int i0 = (int) fl;
        const double f = p - fl;
        const double ym1 = buf[(size_t) ((i0 - 1) & mask)], y0 = buf[(size_t) (i0 & mask)];
        const double y1 = buf[(size_t) ((i0 + 1) & mask)], y2 = buf[(size_t) ((i0 + 2) & mask)];
        const double c1 = 0.5 * (y1 - ym1);
        const double c2 = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
        w = (w + 1) & mask;
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
};

// Sparse vinyl-style clicks plus a very quiet dull noise floor
struct Crackle
{
    Rng rng;
    double clickVal = 0, hpState = 0, hissLp = 0;
    double decay = 0.9, hpCoef = 0.2, hissCoef = 0.3, pClick = 0;

    void setup (double fs, uint32_t seed)
    {
        rng.seed (seed);
        decay = std::exp (-1.0 / (0.00025 * fs));
        hpCoef = 1.0 - std::exp (-2.0 * kPi * 1500.0 / fs);
        hissCoef = 1.0 - std::exp (-2.0 * kPi * 5000.0 / fs);
        pClick = 1.0 / fs;       // multiplied by clicks-per-second below
        clickVal = hpState = hissLp = 0;
    }

    // amount 0..1.  Click peaks top out around -52 dBFS, hiss around -66 dBFS rms.
    inline double process (double amount)
    {
        if (amount <= 1.0e-4) { clickVal = 0; return 0.0; }
        const double rate = 2.0 + 8.0 * amount;   // clicks per second
        if (rng.uni() < rate * pClick)
        {
            const double u = rng.uni();
            const double sign = rng.uni() < 0.5 ? -1.0 : 1.0;
            clickVal += sign * (0.05 + 0.95 * u * u * u) * dbToLin (-52.0);
        }
        clickVal *= decay;
        hpState += hpCoef * (clickVal - hpState);
        const double click = (clickVal - hpState) * 2.0;

        hissLp += hissCoef * (rng.bi() - hissLp);
        const double hiss = hissLp * dbToLin (-60.0);
        return amount * (click + hiss);
    }
};

//==============================================================================
struct Params
{
    double inputTrimDb = 0.0;
    bool   subCut = true;
    bool   monoBass = true;
    double monoFreq = 110.0;
    bool   satOn = true;
    double satAmount = 0.25;
    bool   sp12On = true;
    double sp12Mix = 1.0;
    bool   glueOn = true;
    double glueAmount = 0.5;
    double glueThreshDb = -14.0;
    double tone = 0.5;
    double drift = 0.25;
    double crackle = 0.0;
    int    levelMode = 1;          // 0 manual, 1 auto ride, 2 locked
    double targetLufs = -14.0;
    double lockedGainDb = 0.0;
    double outGainDb = 0.0;
    double ceilingDb = -1.0;
};

class MasterChain
{
public:
    static constexpr int kCtrl = 32;   // control-rate interval in samples

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        driftBase = (int) std::ceil (0.0008 * fs);

        subL.c = subR.c = makeHighpass (fs, 25.0, 0.7071);
        monoHp1.c = monoHp2.c = makeHighpass (fs, 110.0, 0.7071);

        spActive = fs > 26040.0 * 1.05;
        spL.setup (fs); spR.setup (fs);

        glue.prepare (fs);

        satL.reset(); satR.reset();
        toneBellL.c = toneBellR.c = makePeak (fs, 180.0, 0.6, 0.0);
        toneShelfL.c = toneShelfR.c = makeHighShelf (fs, 7000.0, 0.0);

        dlL.prepare (2048); dlR.prepare (2048);
        wow.setup (0.8, fs, 0x1234567u);
        wowR.setup (1.1, fs, 0x7654321u);
        flutter.setup (9.0, fs, 0x2468aceu);
        wobble.setup (0.35, fs / kCtrl, 0x0badf00du);

        crackL.setup (fs, 0xc0ffeeu);
        crackR.setup (fs, 0xfacade1u);

        limiter.prepare (fs);
        preMeter.prepare (fs);
        postMeter.prepare (fs);

        subMix.setup (20, fs); satMix.setup (20, fs); spMix.setup (20, fs);
        glueOnSm.setup (30, fs); monoSm.setup (20, fs);
        driftSm.setup (250, fs); crackSm.setup (50, fs);
        autoCoef = 1.0 - std::exp (-1.0 / (1.5 * fs));
        fastCoef = 1.0 - std::exp (-1.0 / (0.08 * fs));
        autoHop = std::max (1, (int) std::lround (fs * 0.1));

        reset();
    }

    void reset()
    {
        subL.reset(); subR.reset(); monoHp1.reset(); monoHp2.reset();
        spL.reset(); spR.reset(); glue.reset();
        satL.reset(); satR.reset();
        toneBellL.reset(); toneBellR.reset(); toneShelfL.reset(); toneShelfR.reset();
        dlL.reset(); dlR.reset();
        limiter.reset();
        preMeter.reset(); postMeter.reset();
        tpOutL.reset(); tpOutR.reset();
        outPeak = 0; grHold = 0;
        ctrlCount = 0; autoCnt = 0;
        gainDb = 0; modeGainTarget = 0; needSnap = true;
        satDrive = 1.0; satBias = 0.0;
        subMix.v = P.subCut ? 1.0 : 0.0;
        satMix.v = P.satOn ? 1.0 : 0.0;
        spMix.v = P.sp12On ? P.sp12Mix : 0.0;
        glueOnSm.v = P.glueOn ? 1.0 : 0.0;
        monoSm.v = P.monoBass ? 1.0 : 0.0;
        driftSm.v = P.drift;
        crackSm.v = P.crackle;
    }

    void setParams (const Params& p)
    {
        if (p.monoFreq != P.monoFreq)
            monoHp1.c = monoHp2.c = makeHighpass (fs, clampd (p.monoFreq, 40.0, 400.0), 0.7071);
        P = p;
        trimLin = dbToLin (P.inputTrimDb);
        limiter.setCeilingDb (P.ceilingDb);
        if (P.levelMode != 1 || autoCnt == 0) updateGainTarget();
        if (needSnap) { gainDb = modeGainTarget; needSnap = false; }
    }

    int latencySamples() const { return driftBase + limiter.latency(); }

    // Clears integrated measurements, the gain-reduction hold and the peak hold.
    void resetMeters()
    {
        preMeter.resetIntegrated();
        postMeter.resetIntegrated();
        outPeak = 0;
        grHold = 0;
    }

    double preIntegratedLufs() const { return preMeter.integratedLufs(); }
    double postIntegratedLufs() const { return postMeter.integratedLufs(); }
    double postShortTermLufs() const { return postMeter.shortTermLufs(); }
    double preGatedSeconds() const { return preMeter.gatedSeconds(); }
    double outTruePeakDb() const { return linToDb (outPeak); }
    double currentGainDb() const { return gainDb + P.outGainDb; }
    double rideGainDb() const { return gainDb; }   // gain before the output trim (what "Lock" stores)
    double grDb() const { return grHold; }
    double limiterGainDb() const { return limiter.lastGainDb(); }

    inline void processFrame (double& inL, double& inR)
    {
        if (ctrlCount == 0) controlTick();
        if (++ctrlCount >= kCtrl) ctrlCount = 0;

        double l = inL * trimLin, r = inR * trimLin;

        // 1. sub cut (25 Hz, 12 dB/oct): keeps inaudible rumble out of the headroom
        {
            const double m = subMix.next (P.subCut ? 1.0 : 0.0);
            const double hl = subL.process (l), hr = subR.process (r);
            l += m * (hl - l); r += m * (hr - r);
        }

        // 2. light saturation (ADAA tanh with a touch of asymmetry)
        {
            const double m = satMix.next (P.satOn ? 1.0 : 0.0);
            const double sl = satL.process (l, satDrive, satBias);
            const double sr = satR.process (r, satDrive, satBias);
            l += m * (sl - l); r += m * (sr - r);
        }

        // 3. 12-bit / 26.04 kHz sampler stage
        if (spActive)
        {
            const double m = spMix.next (P.sp12On ? P.sp12Mix : 0.0);
            const double wl = spL.process (l), wr = spR.process (r);
            l += m * (wl - l); r += m * (wr - r);
        }

        // 4. glue compressor
        {
            const double on = glueOnSm.next (P.glueOn ? 1.0 : 0.0);
            const double ratio = 1.2 + 1.2 * P.glueAmount;
            const double maxGr = 1.5 + 2.5 * P.glueAmount;
            double g = glue.process (l, r, P.glueThreshDb, ratio, maxGr);
            g = std::pow (g, on);
            l *= g; r *= g;
            const double gr = -linToDb (g);
            grHold = std::max (gr, grHold * 0.99997);
        }

        // 5. tone voicing
        l = toneShelfL.process (toneBellL.process (l));
        r = toneShelfR.process (toneBellR.process (r));

        // 6. drift: slow random wow + tiny flutter as modulated delay
        {
            const double amt = driftSm.next (P.drift);
            const double nw = wow.next(), nwr = wowR.next(), nf = flutter.next();
            const double dWowL = 100.0e-6 * amt * nw;
            const double dWowR = 100.0e-6 * amt * (nw + 0.15 * nwr);
            const double dFl = 2.5e-6 * amt * nf;
            l = dlL.process (l, (double) driftBase + fs * (dWowL + dFl));
            r = dlR.process (r, (double) driftBase + fs * (dWowR + dFl));
        }

        // 7. mono bass (side channel high-passed) so the low end is always centred
        {
            const double m = monoSm.next (P.monoBass ? 1.0 : 0.0);
            const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
            const double sHp = monoHp2.process (monoHp1.process (side));
            const double s = side + m * (sHp - side);
            l = mid + s; r = mid - s;
        }

        // pre-gain measurement
        preMeter.push (l, r);
        if (++autoCnt >= autoHop) { autoCnt = 0; updateGainTarget(); }

        // 8. level
        {
            gainDb += (P.levelMode == 1 ? autoCoef : fastCoef) * (modeGainTarget - gainDb);
            const double g = dbToLin (gainDb + P.outGainDb);
            l *= g; r *= g;
        }

        // 9. vinyl crackle / noise floor (absolute level, independent of loudness)
        {
            const double a = crackSm.next (P.crackle);
            l += crackL.process (a);
            r += crackR.process (a);
        }

        // 10. true-peak limiter
        limiter.process (l, r);

        postMeter.push (l, r);
        outPeak = std::max (outPeak, std::max (tpOutL.push (l), tpOutR.push (r)));

        inL = l; inR = r;
    }

    void process (float* l, float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            double a = l[i], b = r[i];
            processFrame (a, b);
            l[i] = (float) a; r[i] = (float) b;
        }
    }

private:
    void updateGainTarget()
    {
        if (P.levelMode == 0) modeGainTarget = 0.0;
        else if (P.levelMode == 2) modeGainTarget = P.lockedGainDb;
        else
        {
            const double pre = preMeter.integratedLufs();
            if (preMeter.gatedSeconds() >= 1.0 && pre > -69.0)
                modeGainTarget = clampd (P.targetLufs - pre, -24.0, 24.0);
        }
    }

    void controlTick()
    {
        const double w = wobble.next();            // very slow analogue-style wander
        const double amt = driftSm.v;
        satDrive = (1.0 + 1.5 * P.satAmount) * (1.0 + 0.06 * amt * w);
        satBias = 0.08 * P.satAmount;

        const double shelfDb = -3.0 * P.tone + 0.25 * amt * w;
        toneShelfL.c = toneShelfR.c = makeHighShelf (fs, 7000.0, shelfDb);
        toneBellL.c = toneBellR.c = makePeak (fs, 180.0, 0.6, 1.0 * P.tone);
    }

    double fs = 48000.0;
    Params P;
    double trimLin = 1.0;
    int driftBase = 39, ctrlCount = 0, autoCnt = 0, autoHop = 4800;
    bool spActive = true;

    Biquad subL, subR, monoHp1, monoHp2, toneBellL, toneBellR, toneShelfL, toneShelfR;
    Saturator satL, satR;
    double satDrive = 1.0, satBias = 0.0;
    Sp12Channel spL, spR;
    GlueComp glue;
    DelayLine dlL, dlR;
    NoiseLP wow, wowR, flutter, wobble;
    Crackle crackL, crackR;
    TruePeakLimiter limiter;
    LoudnessMeter preMeter, postMeter;
    TruePeakDetector tpOutL, tpOutR;
    Smooth subMix, satMix, spMix, glueOnSm, monoSm, driftSm, crackSm;

    double gainDb = 0, modeGainTarget = 0, autoCoef = 1e-5, fastCoef = 1e-3;
    double outPeak = 0, grHold = 0;
    bool needSnap = true;
};

} // namespace bb
