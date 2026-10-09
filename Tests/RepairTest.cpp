// Offline checks for the noise-repair DSP. Prints measurements; exits non-zero on failure.
#include <JuceHeader.h>
#include "../Source/Repair.h"
#include <cstdio>
#include <random>

namespace
{
constexpr double sr = 48000.0;
int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++failures;
}

float rms (const std::vector<float>& x, size_t a, size_t b)
{
    double s = 0;
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return (float) std::sqrt (s / (double) (b - a));
}

float db (float v) { return 20.0f * std::log10 (std::max (v, 1.0e-12f)); }

// Level of one frequency component (Goertzel).
float toneLevel (const std::vector<float>& x, size_t a, size_t b, double f)
{
    const double w = juce::MathConstants<double>::twoPi * f / sr, c = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return (float) (2.0 * std::sqrt (std::max (p, 0.0)) / (double) (b - a));
}

template <typename Fn>
std::vector<float> runStereo (const std::vector<float>& in, Fn&& fn, int block = 64)
{
    std::vector<float> l (in), r (in);
    for (size_t i = 0; i < in.size(); i += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - i);
        fn (l.data() + i, r.data() + i, n);
    }
    return l;
}
} // namespace

int main()
{
    std::mt19937 rng (1234);
    std::normal_distribution<float> gauss (0.0f, 1.0f);
    const size_t N = (size_t) sr * 4;

    std::printf ("Denoiser: transparency\n");
    {
        bvm::SpectralDenoiser dn;
        dn.setProfile (std::vector<float> (bvm::SpectralDenoiser::numBins, 0.0f)); // zero noise -> unity gain
        std::vector<float> x (N);
        for (size_t i = 0; i < N; ++i) x[i] = 0.3f * std::sin (0.05f * (float) i) + 0.05f * gauss (rng);
        auto y = runStereo (x, [&] (float* l, float* r, int n) { dn.process (l, r, n, 1.0f, false); });
        float maxErr = 0;
        const size_t lat = bvm::SpectralDenoiser::latency;
        for (size_t i = 4096; i < N; ++i) maxErr = std::max (maxErr, std::abs (y[i] - x[i - lat]));
        std::printf ("  max reconstruction error %.2e (latency %d samples)\n", maxErr, (int) lat);
        check (maxErr < 1.0e-4f, "unity-gain path reconstructs the input exactly (delayed)");
    }

    std::printf ("Denoiser: static/hiss reduction\n");
    {
        // 1 s noise only (learn), then 1 s noise only, then 2 s noise + 1 kHz tone.
        std::vector<float> clean (N, 0.0f), x (N);
        for (size_t i = (size_t) sr * 2; i < N; ++i) clean[i] = 0.1f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) sr);
        for (size_t i = 0; i < N; ++i) x[i] = clean[i] + 0.003f * gauss (rng); // static ~ -50 dBFS
        for (bool learned : { false, true })
        {
            bvm::SpectralDenoiser dn;
            size_t pos = 0;
            auto y = runStereo (x, [&] (float* l, float* r, int n)
            {
                dn.process (l, r, n, 0.8f, learned && pos < (size_t) sr);
                pos += (size_t) n;
            });
            const size_t s0 = (size_t) (sr * 1.2), s1 = (size_t) (sr * 1.9);
            const float before = rms (x, s0, s1), after = rms (y, s0, s1);
            const float toneIn = toneLevel (x, (size_t) sr * 3, N, 1000.0), toneOut = toneLevel (y, (size_t) sr * 3, N, 1000.0);
            std::printf ("  %s profile: noise %.1f -> %.1f dBFS (%.1f dB less), 1 kHz tone %.2f -> %.2f dB\n",
                         learned ? "learned" : "auto   ", db (before), db (after), db (before) - db (after), db (toneIn), db (toneOut));
            check (db (before) - db (after) > 12.0f, learned ? "learned profile removes >12 dB of static" : "auto profile removes >12 dB of static");
            check (std::abs (db (toneIn) - db (toneOut)) < 1.0f, "wanted signal level preserved within 1 dB");
        }
    }

    std::printf ("De-crackle: clicks and dropouts\n");
    {
        std::vector<float> clean (N), damaged (N);
        for (size_t i = 0; i < N; ++i)
            clean[i] = 0.25f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) sr)
                     + 0.1f * std::sin (juce::MathConstants<float>::twoPi * 1330.0f * (float) i / (float) sr);
        damaged = clean;
        int events = 0;
        for (size_t i = 2000; i + 20 < N; i += 4801, ++events)
        {
            if (events % 3 == 0) damaged[i] += 0.6f;                              // single-sample spike
            else if (events % 3 == 1) { damaged[i] -= 0.4f; damaged[i + 1] += 0.3f; } // crackle
            else for (size_t j = i; j < i + 6; ++j) damaged[j] = 0.0f;             // dropout
        }
        bvm::Declicker dc;
        auto y = runStereo (damaged, [&] (float* l, float* r, int n) { dc.process (l, r, n, 0.6f); });
        const size_t d = bvm::Declicker::delay;
        float errBefore = 0, errAfter = 0;
        for (size_t i = 1000; i + d < N; ++i)
        {
            errBefore = std::max (errBefore, std::abs (damaged[i] - clean[i]));
            errAfter = std::max (errAfter, std::abs (y[i + d] - clean[i]));
        }
        std::printf ("  %d damage events, %d repairs; worst error %.3f -> %.3f\n", events, dc.getRepairCount(), errBefore, errAfter);
        check (errAfter < errBefore * 0.25f, "worst-case damage reduced by >75%");

        bvm::Declicker clean1;
        runStereo (clean, [&] (float* l, float* r, int n) { clean1.process (l, r, n, 0.6f); });
        std::printf ("  false repairs on clean signal: %d\n", clean1.getRepairCount());
        check (clean1.getRepairCount() == 0, "no repairs on an undamaged signal");
    }

    std::printf ("Hum filter (60 Hz)\n");
    {
        std::vector<float> x (N);
        for (size_t i = 0; i < N; ++i)
        {
            const float t = (float) i / (float) sr;
            x[i] = 0.05f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * t)
                 + 0.02f * std::sin (juce::MathConstants<float>::twoPi * 180.0f * t)
                 + 0.1f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * t);
        }
        bvm::HumFilter hum;
        auto y = runStereo (x, [&] (float* l, float* r, int n) { hum.process (l, r, n, 60.0f, sr); });
        const size_t a = (size_t) sr, b = N;
        const float h60 = db (toneLevel (x, a, b, 60)) - db (toneLevel (y, a, b, 60));
        const float h180 = db (toneLevel (x, a, b, 180)) - db (toneLevel (y, a, b, 180));
        const float k1 = db (toneLevel (x, a, b, 1000)) - db (toneLevel (y, a, b, 1000));
        std::printf ("  60 Hz -%.1f dB, 180 Hz -%.1f dB, 1 kHz change %.2f dB\n", h60, h180, k1);
        check (h60 > 30 && h180 > 30, "hum fundamental and harmonic removed by >30 dB");
        check (std::abs (k1) < 0.5f, "voice range untouched (<0.5 dB)");
    }

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
