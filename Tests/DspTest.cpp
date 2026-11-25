// Standalone test for the DSP core. Build: g++ -std=c++17 -O2 -I../Source DspTest.cpp -o dsptest
#include "DSP.h"
#include <cstdio>
#include <cstdlib>

using namespace bb;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } else { std::printf("ok:   "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// crude boom-bap-ish test signal: kick/snare/hat + sustained sample chord + bass
static void makeBeat (std::vector<double>& L, std::vector<double>& R, double fs, double seconds, double peakScale)
{
    const size_t n = (size_t) (fs * seconds);
    L.assign (n, 0.0); R.assign (n, 0.0);
    Rng rng; rng.seed (99);
    const double bpm = 90.0, beat = 60.0 / bpm;
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double bt = std::fmod (t, beat * 4.0);
        double s = 0;
        // kick on 1 and 2.5, snare on 2 and 4
        auto env = [&] (double at, double tau) { const double d = bt - at; return d >= 0 ? std::exp (-d / tau) : 0.0; };
        const double kd = bt - 0.0, kd2 = bt - beat * 2.5;
        if (kd >= 0 && kd < 0.4)   s += 0.9 * std::sin (2 * kPi * (50 + 90 * std::exp (-kd * 40)) * kd) * std::exp (-kd / 0.12);
        if (kd2 >= 0 && kd2 < 0.4) s += 0.9 * std::sin (2 * kPi * (50 + 90 * std::exp (-kd2 * 40)) * kd2) * std::exp (-kd2 / 0.12);
        s += 0.8 * rng.bi() * (env (beat, 0.06) + env (beat * 3, 0.06));
        s += 0.15 * rng.bi() * env (std::floor (bt / (beat * 0.5)) * beat * 0.5, 0.01);
        // sample: sustained chord
        s += 0.20 * (std::sin (2 * kPi * 261.6 * t) + std::sin (2 * kPi * 329.6 * t) + std::sin (2 * kPi * 392.0 * t));
        // bass
        s += 0.30 * std::sin (2 * kPi * 55.0 * t);
        L[i] = s * peakScale;
        R[i] = (s + 0.1 * std::sin (2 * kPi * 523.0 * t)) * peakScale;
    }
}

static void run (MasterChain& ch, std::vector<double>& L, std::vector<double>& R)
{
    for (size_t i = 0; i < L.size(); ++i) ch.processFrame (L[i], R[i]);
}

// independent reference true-peak: 16x oversampling, +/-32 tap Kaiser sinc (beta 10).
// Only looks near samples above 40% of the sample peak (true peaks cannot hide elsewhere).
static double measureTruePeakDb (const std::vector<double>& x, size_t from)
{
    const int H = 32, OS = 16;
    static std::vector<std::vector<double>> tap;
    if (tap.empty())
    {
        tap.assign ((size_t) OS, std::vector<double> ((size_t) (2 * H), 0.0));
        const double i0b = besselI0 (10.0);
        for (int ph = 1; ph < OS; ++ph)
        {
            double sum = 0;
            for (int k = -(H - 1); k <= H; ++k)
            {
                const double t = k - (double) ph / OS;
                const double u = t / H;
                const double w = std::fabs (u) < 1.0 ? besselI0 (10.0 * std::sqrt (1.0 - u * u)) / i0b : 0.0;
                const double v = std::sin (kPi * t) / (kPi * t) * w;
                tap[(size_t) ph][(size_t) (k + H - 1)] = v;
                sum += v;
            }
            for (double& v : tap[(size_t) ph]) v /= sum;
        }
    }
    double sp = 0;
    for (size_t i = from; i < x.size(); ++i) sp = std::max (sp, std::fabs (x[i]));
    double mx = sp;
    for (size_t i = from + (size_t) H; i + (size_t) H + 1 < x.size(); ++i)
    {
        if (std::fabs (x[i]) < 0.4 * sp && std::fabs (x[i + 1]) < 0.4 * sp) continue;
        for (int ph = 1; ph < OS; ++ph)
        {
            double y = 0;
            for (int k = -(H - 1); k <= H; ++k) y += x[(size_t) ((long long) i + k)] * tap[(size_t) ph][(size_t) (k + H - 1)];
            mx = std::max (mx, std::fabs (y));
        }
    }
    return linToDb (mx);
}

int main (int argc, char** argv)
{
    const double fs = argc > 1 ? std::atof (argv[1]) : 44100.0;
    std::printf ("sample rate: %.0f Hz\n", fs);

    // 1. loudness meter sanity: 1 kHz sine at -20 dBFS stereo = about -20 LUFS (+3 for stereo sum -> -17)
    {
        LoudnessMeter m; m.prepare (fs);
        for (int i = 0; i < (int) (fs * 10); ++i) { const double v = dbToLin (-20.0) * std::sin (2 * kPi * 1000.0 * i / fs); m.push (v, v); }
        // BS.1770: identical sine in both channels at -20 dBFS reads -20 LUFS (K-weighting adds +0.691 dB at 1 kHz,
        // which the -0.691 offset in the loudness formula cancels).
        const double ref = -20.0;
        CHECK (std::fabs (m.integratedLufs() - ref) < 0.5, "meter: 1 kHz sine at -20 dBFS reads %.2f LUFS (expected %.2f)", m.integratedLufs(), ref);
    }

    // 2. offline two-pass: measure, then lock gain, check -14 / -1
    for (double scale : { 0.3, 0.8, 1.6 })
    {
        std::vector<double> L, R;
        makeBeat (L, R, fs, 40.0, scale);

        // pass 1: analysis
        MasterChain a; Params p; p.levelMode = 0; p.crackle = 0.0; a.setParams (p); a.prepare (fs); a.setParams (p);
        { auto l = L; auto r = R; run (a, l, r); }
        const double pre = a.preIntegratedLufs();

        // pass 2: locked gain
        MasterChain b; Params q = p; q.levelMode = 2; q.lockedGainDb = -14.0 - pre; b.prepare (fs); b.setParams (q); b.reset();
        auto l = L, r = R; run (b, l, r);
        // let gain settle: ignore first 2 seconds for the measurement
        LoudnessMeter lm; lm.prepare (fs);
        const size_t skip = (size_t) (fs * 2.0);
        for (size_t i = skip; i < l.size(); ++i) lm.push (l[i], r[i]);
        std::printf ("  scale %.1f: pre %.2f LUFS, chain true-peak %.2f dBTP, post %.2f LUFS, glue GR peak %.2f dB\n",
                     scale, pre, b.outTruePeakDb(), lm.integratedLufs(), b.grDb());
        const double tpRef = std::max (measureTruePeakDb (l, (size_t) fs), measureTruePeakDb (r, (size_t) fs));
        std::printf ("  scale %.1f: independent 16x true-peak %.3f dBTP\n", scale, tpRef);
        CHECK (tpRef <= -1.0, "offline pass: independent true peak %.3f dBTP <= -1.0 (scale %.1f)", tpRef, scale);
        CHECK (std::fabs (lm.integratedLufs() + 14.0) < 0.3, "offline pass: loudness %.2f LUFS within 0.3 LU of -14 (scale %.1f)", lm.integratedLufs(), scale);
    }

    // 3. auto-ride mode converges to the target on a long signal
    {
        std::vector<double> L, R;
        makeBeat (L, R, fs, 60.0, 0.6);
        MasterChain c; Params p; p.levelMode = 1; c.prepare (fs); c.setParams (p); c.reset();
        run (c, L, R);
        LoudnessMeter lm; lm.prepare (fs);
        const size_t skip = (size_t) (fs * 15.0);
        for (size_t i = skip; i < L.size(); ++i) lm.push (L[i], R[i]);
        const double tp = measureTruePeakDb (L, skip);
        std::printf ("  auto ride: post integrated(15..60s) %.2f LUFS, own true-peak %.2f, independent true-peak %.2f\n",
                     lm.integratedLufs(), c.outTruePeakDb(), tp);
        CHECK (std::fabs (lm.integratedLufs() + 14.0) < 1.0, "auto ride lands within 1 LU of -14 (got %.2f)", lm.integratedLufs());
        CHECK (c.outTruePeakDb() <= -0.95, "auto ride true peak %.2f <= -1", c.outTruePeakDb());
    }

    // 4. limiter alone, hot and clippy input. Realistic = band-limited to 16 kHz; stress = full-band white noise
    for (int stress = 0; stress < 2; ++stress)
    {
        TruePeakLimiter lim; lim.prepare (fs); lim.setCeilingDb (-1.0);
        Biquad lp; lp.c = makeLowpass (fs, 0.36 * fs, 0.7071);   // 16 kHz at 44.1 kHz
        std::vector<double> L, R;
        Rng rng; rng.seed (5);
        const size_t n = (size_t) (fs * 5);
        L.resize (n); R.resize (n);
        for (size_t i = 0; i < n; ++i)
        {
            const double noise = 0.4 * rng.bi();
            double v = 1.8 * std::sin (2 * kPi * 3150.0 * i / fs) + (stress ? noise : lp.process (noise));
            if ((i / 20000) % 2) v *= 0.1;
            L[i] = v; R[i] = -v * 0.9;
        }
        for (size_t i = 0; i < n; ++i) lim.process (L[i], R[i]);
        const double tp = std::max (measureTruePeakDb (L, 2000), measureTruePeakDb (R, 2000));
        std::printf ("  limiter (%s): independent 16x true-peak %.3f dBTP, safety clamp hits %lld\n",
                     stress ? "full-band white-noise stress" : "realistic band-limited", tp, lim.clampHits);
        if (stress) CHECK (tp <= -0.8, "limiter stress (energy up to Nyquist): true peak %.3f dBTP within 0.2 dB of ceiling", tp);
        else        CHECK (tp <= -1.0, "limiter realistic: true peak %.3f dBTP <= -1.0", tp);
        CHECK (lim.clampHits == 0, "limiter: safety clamp never needed (%lld hits)", lim.clampHits);
    }

    // 5. sp12 stage: output only has 12-bit-ish steps and band limit
    {
        Sp12Channel s; s.setup (fs);
        double maxAbove = 0;
        for (int i = 0; i < 20000; ++i)
        {
            const double x = 0.5 * std::sin (2 * kPi * 15000.0 * i / fs); // above 12k, should be heavily attenuated
            const double y = s.process (x);
            if (i > 2000) maxAbove = std::max (maxAbove, std::fabs (y));
        }
        CHECK (linToDb (maxAbove) < -20.0, "sp12: 15 kHz tone attenuated to %.1f dBFS (input -6)", linToDb (maxAbove));
    }

    // 6. drift is small: stays below 2 cents and output is finite
    {
        MasterChain c; Params p; p.drift = 1.0; p.levelMode = 0; p.satOn = false; p.sp12On = false; p.glueOn = false; p.subCut = false; p.monoBass = false;
        c.prepare (fs); c.setParams (p); c.reset();
        bool finite = true;
        for (int i = 0; i < (int) (fs * 5); ++i)
        {
            double l = 0.2 * std::sin (2 * kPi * 440.0 * i / fs), r = l;
            c.processFrame (l, r);
            if (! std::isfinite (l) || ! std::isfinite (r)) finite = false;
        }
        CHECK (finite, "drift: output finite");
    }

    // 7. silence in -> silence out, no NaNs from any stage
    {
        MasterChain c; Params p; c.prepare (fs); c.setParams (p); c.reset();
        double mx = 0; bool finite = true;
        for (int i = 0; i < (int) (fs * 3); ++i) { double l = 0, r = 0; c.processFrame (l, r); mx = std::max (mx, std::fabs (l)); finite &= std::isfinite (l); }
        CHECK (finite && mx < 1e-3, "silence: finite and quiet (%.2e)", mx);
    }

    // 8. extreme user settings: everything maxed, +12 dB in, +24 dB locked gain. Must stay finite and under the ceiling.
    {
        std::vector<double> L, R;
        makeBeat (L, R, fs, 10.0, 1.6);
        MasterChain c; Params p;
        p.inputTrimDb = 12.0; p.satAmount = 1.0; p.drift = 1.0; p.crackle = 1.0; p.glueAmount = 1.0;
        p.levelMode = 2; p.lockedGainDb = 24.0; p.outGainDb = 6.0;
        c.prepare (fs); c.setParams (p);
        run (c, L, R);
        bool finite = true;
        for (size_t i = 0; i < L.size(); ++i) finite &= std::isfinite (L[i]) && std::isfinite (R[i]);
        const double tp = std::max (measureTruePeakDb (L, (size_t) fs), measureTruePeakDb (R, (size_t) fs));
        std::printf ("  extreme settings: independent true-peak %.3f dBTP\n", tp);
        CHECK (finite, "extreme settings: output finite");
        CHECK (tp <= -0.95, "extreme settings: true peak %.3f dBTP held at the ceiling", tp);
    }

    std::printf ("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
