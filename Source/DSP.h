#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <complex>

namespace bvm
{
inline float dbToGain (float db) noexcept       { return db <= -59.9f ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept        { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }
inline float timeCoeff (float ms, double sr)    { return ms <= 0.0f ? 0.0f : (float) std::exp (-1.0 / (ms * 0.001 * sr)); }

// RBJ biquad, transposed direct form II.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void reset() noexcept { z1 = z2 = 0; }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void setNormalised (double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
    {
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); b2 = (float) (nb2 / na0);
        a1 = (float) (na1 / na0); a2 = (float) (na2 / na0);
    }

    void setBypass() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void setHighPass (double sr, double f, double q = 0.7071)
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr, c = std::cos (w), a = std::sin (w) / (2 * q);
        setNormalised ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a);
    }

    void setLowPass (double sr, double f, double q = 0.7071)
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr, c = std::cos (w), a = std::sin (w) / (2 * q);
        setNormalised ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
    }

    void setPeak (double sr, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40), w = juce::MathConstants<double>::twoPi * f / sr;
        const double c = std::cos (w), a = std::sin (w) / (2 * q);
        setNormalised (1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A);
    }

    void setLowShelf (double sr, double f, double gainDb, double s = 1.0)
    {
        const double A = std::pow (10.0, gainDb / 40), w = juce::MathConstants<double>::twoPi * f / sr;
        const double c = std::cos (w), a = std::sin (w) / 2 * std::sqrt ((A + 1 / A) * (1 / s - 1) + 2), sq = 2 * std::sqrt (A) * a;
        setNormalised (A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
                       (A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
    }

    void setHighShelf (double sr, double f, double gainDb, double s = 1.0)
    {
        const double A = std::pow (10.0, gainDb / 40), w = juce::MathConstants<double>::twoPi * f / sr;
        const double c = std::cos (w), a = std::sin (w) / 2 * std::sqrt ((A + 1 / A) * (1 / s - 1) + 2), sq = 2 * std::sqrt (A) * a;
        setNormalised (A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
                       (A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
    }

    // Magnitude response at frequency f (for drawing EQ curves).
    double magnitudeAt (double f, double sr) const
    {
        const double w = juce::MathConstants<double>::twoPi * f / sr;
        const std::complex<double> z = std::polar (1.0, -w), zz = z * z;
        const auto num = (double) b0 + (double) b1 * z + (double) b2 * zz;
        const auto den = 1.0 + (double) a1 * z + (double) a2 * zz;
        return std::abs (num / den);
    }
};

// Noise gate / downward expander with hold. Linked stereo detection is done by the caller.
struct Gate
{
    float env = 0, gain = 1, holdCounter = 0;

    void reset() noexcept { env = 0; gain = 1; holdCounter = 0; }

    // Returns gain to apply for this sample given detector level |x|.
    float process (float level, float threshLin, float floorGain, float attCoef, float relCoef,
                   float holdSamples, float envRel) noexcept
    {
        env = level > env ? level : env * envRel + level * (1 - envRel);

        float target;
        if (env >= threshLin) { target = 1.0f; holdCounter = holdSamples; }
        else if (holdCounter > 0) { target = 1.0f; holdCounter -= 1.0f; }
        else target = floorGain;

        const float coef = target > gain ? attCoef : relCoef;
        gain = target + (gain - target) * coef;
        return gain;
    }
};

// Feed-forward compressor with soft knee; works in the dB domain.
struct Compressor
{
    float envDb = -120.0f, grDb = 0.0f;

    void reset() noexcept { envDb = -120.0f; grDb = 0.0f; }

    float computeGrDb (float inDb, float thresh, float ratio, float knee) const noexcept
    {
        const float over = inDb - thresh;
        if (knee > 0.0f && std::abs (over) * 2.0f <= knee)
        {
            const float x = over + knee * 0.5f;
            return (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
        }
        return over > 0.0f ? over * (1.0f / ratio - 1.0f) : 0.0f;
    }

    // Returns linear gain for this sample (detector = linked peak level).
    float process (float level, float thresh, float ratio, float knee, float attCoef, float relCoef) noexcept
    {
        const float inDb = bvm::gainToDb (level);
        const float target = computeGrDb (inDb, thresh, ratio, knee); // <= 0
        const float coef = target < grDb ? attCoef : relCoef;
        grDb = target + (grDb - target) * coef;
        return std::pow (10.0f, grDb * 0.05f);
    }
};

// Simple brickwall-ish peak limiter for bus outputs (instant attack, smooth release).
struct Limiter
{
    float gain = 1.0f;
    void reset() noexcept { gain = 1.0f; }

    float process (float peak, float ceiling, float relCoef) noexcept
    {
        const float needed = peak > ceiling ? ceiling / peak : 1.0f;
        gain = needed < gain ? needed : needed + (gain - needed) * relCoef;
        return gain;
    }
};

// Stereo feedback delay with lowpass in the loop.
struct StereoDelay
{
    juce::AudioBuffer<float> buffer;
    int writePos = 0;
    Biquad lpL, lpR;
    double sampleRate = 48000;

    void prepare (double sr)
    {
        sampleRate = sr;
        buffer.setSize (2, (int) (sr * 2.0) + 1);
        buffer.clear();
        writePos = 0;
        lpL.reset(); lpR.reset();
    }

    void process (float* l, float* r, int n, float timeMs, float feedback, float toneHz)
    {
        lpL.setLowPass (sampleRate, toneHz); lpR.setLowPass (sampleRate, toneHz);
        const int size = buffer.getNumSamples();
        const int delay = juce::jlimit (1, size - 1, (int) (timeMs * 0.001 * sampleRate));
        auto* bl = buffer.getWritePointer (0);
        auto* br = buffer.getWritePointer (1);

        for (int i = 0; i < n; ++i)
        {
            int readPos = writePos - delay;
            if (readPos < 0) readPos += size;
            const float dl = bl[readPos], dr = br[readPos];
            // Slight ping-pong: feed each side into the other.
            bl[writePos] = l[i] + lpL.process (dr * feedback);
            br[writePos] = r[i] + lpR.process (dl * feedback);
            l[i] = dl;
            r[i] = dr;
            if (++writePos >= size) writePos = 0;
        }
    }
};
} // namespace bvm
