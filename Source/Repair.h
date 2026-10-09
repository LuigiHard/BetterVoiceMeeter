#pragma once
#include <JuceHeader.h>
#include "DSP.h"
#include <atomic>

// Noise-repair tools for problem sources (faulty cables, loose jacks, ground loops).
namespace bvm
{
//==============================================================================
// Removes clicks, crackle and short dropouts. A bad cable produces sudden
// discontinuities; these show up as spikes in the signal's second difference
// relative to its recent level. Detected regions are replaced by a straight-line
// interpolation. Latency: `delay` samples (0.67 ms at 48 kHz).
class Declicker
{
public:
    static constexpr int size = 64, mask = size - 1, delay = 32, maxRepair = 20;

    void reset() noexcept
    {
        std::fill (std::begin (bufL), std::end (bufL), 0.0f);
        std::fill (std::begin (bufR), std::end (bufR), 0.0f);
        count = 0;
        env = 0.05f; // start insensitive, settle down to the signal's level
        regionStart = -1;
        repairs = 0;
    }

    // Linked stereo. amount 0..1 sets detection sensitivity.
    void process (float* l, float* r, int n, float amount) noexcept
    {
        const float k = juce::jmap (amount, 0.0f, 1.0f, 14.0f, 3.5f);
        for (int i = 0; i < n; ++i)
        {
            bufL[count & mask] = l[i];
            bufR[count & mask] = r[i];

            const float dl = bufL[count & mask] - 2.0f * bufL[(count - 1) & mask] + bufL[(count - 2) & mask];
            const float dr = bufR[count & mask] - 2.0f * bufR[(count - 1) & mask] + bufR[(count - 2) & mask];
            const float d = juce::jmax (std::abs (dl), std::abs (dr));
            const bool click = d > k * env + 3.0e-5f;

            // Level tracker always runs, but clicks only count up to the threshold so they can't mask themselves.
            env = env * 0.9995f + juce::jmin (d, k * env) * 0.0005f;
            if (click && regionStart < 0)
            {
                regionStart = count - 3;
                regionEnd = count + 4;
                ++repairs;
            }
            else if (click && count + 4 - regionStart <= maxRepair)
                regionEnd = count + 4;

            if (regionStart >= 0 && count == regionEnd + 1)
            {
                // Bridge the damaged samples with a line between the good neighbours.
                const int64_t a = regionStart - 1, b = count;
                const float la = bufL[a & mask], lb = bufL[b & mask], ra = bufR[a & mask], rb = bufR[b & mask];
                const float span = (float) (b - a);
                for (int64_t j = regionStart; j <= regionEnd; ++j)
                {
                    const float t = (float) (j - a) / span;
                    bufL[j & mask] = la + (lb - la) * t;
                    bufR[j & mask] = ra + (rb - ra) * t;
                }
                regionStart = -1;
            }

            l[i] = bufL[(count - delay) & mask];
            r[i] = bufR[(count - delay) & mask];
            ++count;
        }
    }

    int getRepairCount() const noexcept { return repairs; }

private:
    float bufL[size] {}, bufR[size] {};
    int64_t count = 0, regionStart = -1, regionEnd = -1;
    float env = 0.05f;
    int repairs = 0;
};

//==============================================================================
// Mains hum remover: narrow notches at the fundamental (50 or 60 Hz) and 4 harmonics.
class HumFilter
{
public:
    static constexpr int harmonics = 5;

    void reset() noexcept
    {
        for (auto& ch : filters)
            for (auto& f : ch) f.reset();
    }

    void process (float* l, float* r, int n, float mainsHz, double sr) noexcept
    {
        if (mainsHz != currentHz || sr != currentSr)
        {
            currentHz = mainsHz;
            currentSr = sr;
            for (int h = 0; h < harmonics; ++h)
                for (auto& ch : filters)
                    ch[(size_t) h].setNotch (sr, mainsHz * (h + 1), 8.0 * (h + 1)); // ~6 Hz wide each
        }
        for (int i = 0; i < n; ++i)
        {
            float xl = l[i], xr = r[i];
            for (int h = 0; h < harmonics; ++h)
            {
                xl = filters[0][(size_t) h].process (xl);
                xr = filters[1][(size_t) h].process (xr);
            }
            l[i] = xl;
            r[i] = xr;
        }
    }

private:
    std::array<std::array<Biquad, harmonics>, 2> filters;
    float currentHz = 0;
    double currentSr = 0;
};

//==============================================================================
// Spectral denoiser (STFT, 256-point FFT, 75% overlap). Steady noise such as hiss,
// static and buzz is subtracted per frequency bin. The noise profile is either
// tracked automatically (minimum statistics) or learned from a noise-only sample.
// Latency: fftSize = 256 samples (5.3 ms at 48 kHz).
class SpectralDenoiser
{
public:
    static constexpr int order = 8, fftSize = 1 << order, hop = fftSize / 4, latency = fftSize;
    static constexpr int numBins = fftSize / 2 + 1;

    enum Status { autoTracking = 0, learning = 1, learned = 2 };

    SpectralDenoiser()
    {
        for (int i = 0; i < fftSize; ++i) // sqrt of periodic Hann: perfect reconstruction at 75% overlap
            window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / fftSize));
        reset();
    }

    void reset() noexcept
    {
        for (auto& c : ch)
        {
            c.in.fill (0); c.out.fill (0); c.accum.fill (0);
            c.gain.fill (1.0f);
        }
        autoNoise.fill (1.0e6f);   // falls to the real floor on the first frames
        smoothPow.fill (-1.0f);
        rover = inFill;
    }

    void clearProfile() noexcept
    {
        hasProfile = false;
        status.store (autoTracking);
    }

    // Message thread (state save/restore). Races with the audio thread are benign.
    std::vector<float> getProfile() const { return hasProfile ? std::vector<float> (profile.begin(), profile.end()) : std::vector<float>(); }
    void setProfile (const std::vector<float>& p)
    {
        if ((int) p.size() != numBins) return;
        std::copy (p.begin(), p.end(), profile.begin());
        hasProfile = true;
        status.store (learned);
    }

    // amount 0..1 (0 = bypass). learnNow: while true the noise profile is captured.
    void process (float* l, float* r, int n, float amount, bool learnNow) noexcept
    {
        if (learnNow && ! wasLearning)
        {
            learnSum.fill (0);
            learnFrames = 0;
            status.store (learning);
        }
        else if (! learnNow && wasLearning)
        {
            if (learnFrames > 8)
            {
                for (int k = 0; k < numBins; ++k)
                    profile[(size_t) k] = learnSum[(size_t) k] / (float) learnFrames;
                hasProfile = true;
            }
            status.store (hasProfile ? learned : autoTracking);
        }
        wasLearning = learnNow;
        currentAmount = amount;

        float* io[2] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                ch[(size_t) c].in[(size_t) rover] = io[c][i];
                io[c][i] = ch[(size_t) c].out[(size_t) (rover - inFill)];
            }
            if (++rover >= fftSize)
            {
                rover = inFill;
                processFrame();
            }
        }
    }

    std::atomic<int> status { autoTracking };

private:
    struct Channel
    {
        std::array<float, fftSize> in {}, out {}, accum {};
        std::array<float, fftSize * 2> spec {};
        std::array<float, numBins> mag {}, gain {};
    };

    void processFrame() noexcept
    {
        const float amount = currentAmount;
        for (auto& c : ch)
        {
            for (int i = 0; i < fftSize; ++i)
                c.spec[(size_t) i] = c.in[(size_t) i] * window[(size_t) i];
            std::fill (c.spec.begin() + fftSize, c.spec.end(), 0.0f);
            fft.performRealOnlyForwardTransform (c.spec.data());
            for (int k = 0; k < numBins; ++k)
                c.mag[(size_t) k] = std::hypot (c.spec[(size_t) (2 * k)], c.spec[(size_t) (2 * k + 1)]);
        }

        // Noise estimate (shared between channels).
        for (int k = 0; k < numBins; ++k)
        {
            const float m = 0.5f * (ch[0].mag[(size_t) k] + ch[1].mag[(size_t) k]);
            if (wasLearning)
                learnSum[(size_t) k] += m;
            // Minimum statistics on time-smoothed power: follows the noise floor, ignores speech peaks.
            auto& ps = smoothPow[(size_t) k];
            ps = ps < 0.0f ? m * m : ps * 0.85f + m * m * 0.15f; // seeded from the first frame
            auto& a = autoNoise[(size_t) k];
            a = ps < a ? ps : a * 1.0015f; // ~5 dB/s rise
        }
        if (wasLearning)
            ++learnFrames;

        const float over = 1.0f + 1.5f * amount;                     // over-subtraction
        const float floorGain = bvm::dbToGain (-6.0f - 24.0f * amount); // max reduction 6..30 dB

        for (auto& c : ch)
        {
            if (amount > 0.0f)
            {
                float raw[numBins];
                for (int k = 0; k < numBins; ++k)
                {
                    const float noise = hasProfile ? profile[(size_t) k] : std::sqrt (autoNoise[(size_t) k] * 2.0f); // min-stats bias
                    raw[k] = juce::jmax (floorGain, 1.0f - over * noise / (c.mag[(size_t) k] + 1.0e-9f));
                }
                for (int k = 0; k < numBins; ++k)
                {
                    const float g = 0.25f * raw[juce::jmax (0, k - 1)] + 0.5f * raw[k] + 0.25f * raw[juce::jmin (numBins - 1, k + 1)];
                    auto& s = c.gain[(size_t) k];
                    s = g > s ? s * 0.4f + g * 0.6f : s * 0.8f + g * 0.2f; // quick open, slower close (less "musical noise")
                }
            }
            else
                c.gain.fill (1.0f);

            for (int k = 0; k < numBins; ++k)
            {
                const float g = c.gain[(size_t) k];
                c.spec[(size_t) (2 * k)] *= g;
                c.spec[(size_t) (2 * k + 1)] *= g;
                if (k > 0 && k < fftSize / 2)
                {
                    c.spec[(size_t) (2 * (fftSize - k))] *= g;
                    c.spec[(size_t) (2 * (fftSize - k) + 1)] *= g;
                }
            }
            fft.performRealOnlyInverseTransform (c.spec.data());

            for (int i = 0; i < fftSize; ++i)
                c.accum[(size_t) i] += c.spec[(size_t) i] * window[(size_t) i] * 0.5f; // sum of Hann at 75% overlap = 2
            std::copy (c.accum.begin(), c.accum.begin() + hop, c.out.begin());
            std::copy (c.accum.begin() + hop, c.accum.end(), c.accum.begin());
            std::fill (c.accum.end() - hop, c.accum.end(), 0.0f);
            std::copy (c.in.begin() + hop, c.in.end(), c.in.begin());
        }
    }

    juce::dsp::FFT fft { order };
    std::array<float, fftSize> window {};
    std::array<Channel, 2> ch;
    std::array<float, numBins> autoNoise {}, smoothPow {}, profile {}, learnSum {};
    static constexpr int inFill = fftSize - hop;
    int rover = inFill, learnFrames = 0;
    bool hasProfile = false, wasLearning = false;
    float currentAmount = 0;
};
} // namespace bvm
