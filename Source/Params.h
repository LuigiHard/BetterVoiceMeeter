#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <limits>

// Lock-free parameter shared between the UI (writer) and the audio thread (reader).
struct Param
{
    static constexpr float noSkew = std::numeric_limits<float>::quiet_NaN();

    Param (juce::String paramId, float minV, float maxV, float def, float skewCentre = noSkew)
        : id (std::move (paramId)), range (minV, maxV), defaultValue (def), value (def)
    {
        if (! std::isnan (skewCentre) && skewCentre > minV && skewCentre < maxV)
            range.setSkewForCentre (skewCentre);
    }

    float get() const noexcept          { return value.load (std::memory_order_relaxed); }
    void set (float v) noexcept         { value.store (juce::jlimit (range.start, range.end, v), std::memory_order_relaxed); }
    bool on() const noexcept            { return get() >= 0.5f; }
    void reset() noexcept               { set (defaultValue); }

    juce::String id;
    juce::NormalisableRange<float> range;
    float defaultValue;
    std::atomic<float> value;
};

// A named collection of params that can be saved/loaded as XML attributes.
struct ParamGroup
{
    virtual ~ParamGroup() = default;

    juce::Array<Param*> params;

    Param& add (Param& p) { params.add (&p); return p; }

    void saveTo (juce::XmlElement& xml) const
    {
        for (auto* p : params)
            xml.setAttribute (p->id, (double) p->get());
    }

    void loadFrom (const juce::XmlElement& xml)
    {
        for (auto* p : params)
            if (xml.hasAttribute (p->id))
                p->set ((float) xml.getDoubleAttribute (p->id, p->defaultValue));
    }

    void resetAll() { for (auto* p : params) p->reset(); }
};

constexpr int kNumHwStrips   = 8;   // hardware input strips (Focusrite channels)
constexpr int kNumVirtStrips = 2;   // virtual input strips (any WASAPI capture device)
constexpr int kNumStrips     = kNumHwStrips + kNumVirtStrips;
constexpr int kNumHwBuses    = 3;   // A1..A3 -> Focusrite output pairs
constexpr int kNumVirtBuses  = 2;   // B1..B2 -> any WASAPI playback device (e.g. VB-Cable)
constexpr int kNumBuses      = kNumHwBuses + kNumVirtBuses;
constexpr int kNumFx         = 2;   // FX returns: 0 = reverb, 1 = delay

inline juce::String busName (int b)
{
    return b < kNumHwBuses ? "A" + juce::String (b + 1) : "B" + juce::String (b - kNumHwBuses + 1);
}

struct StripParams : ParamGroup
{
    explicit StripParams (int index)
    {
        const auto p = "s" + juce::String (index) + "_";
        auto mk = [&] (const char* name, float mn, float mx, float def, float skew = Param::noSkew)
        {
            return std::make_unique<Param> (p + name, mn, mx, def, skew);
        };

        fader       = mk ("fader", -60.0f, 12.0f, 0.0f, -9.0f);
        trim        = mk ("trim", -24.0f, 36.0f, 0.0f, 0.0f);
        mute        = mk ("mute", 0, 1, 0);
        solo        = mk ("solo", 0, 1, 0);
        polarity    = mk ("polarity", 0, 1, 0);
        pan         = mk ("pan", -1.0f, 1.0f, 0.0f);
        tone        = mk ("tone", -1.0f, 1.0f, 0.0f);
        hpfFreq     = mk ("hpf", 20.0f, 400.0f, 20.0f, 80.0f);
        gateThresh  = mk ("gateThresh", -80.0f, -10.0f, -80.0f);
        gateRange   = mk ("gateRange", -80.0f, 0.0f, -60.0f);
        gateAttack  = mk ("gateAttack", 0.1f, 50.0f, 1.0f, 5.0f);
        gateHold    = mk ("gateHold", 0.0f, 500.0f, 50.0f, 100.0f);
        gateRelease = mk ("gateRelease", 5.0f, 1000.0f, 120.0f, 150.0f);
        compThresh  = mk ("compThresh", -50.0f, 0.0f, 0.0f);
        compRatio   = mk ("compRatio", 1.0f, 20.0f, 3.0f, 4.0f);
        compAttack  = mk ("compAttack", 0.1f, 100.0f, 8.0f, 10.0f);
        compRelease = mk ("compRelease", 10.0f, 1000.0f, 120.0f, 150.0f);
        compKnee    = mk ("compKnee", 0.0f, 18.0f, 6.0f);
        compMakeup  = mk ("compMakeup", 0.0f, 24.0f, 0.0f);
        compAuto    = mk ("compAuto", 0, 1, 1);
        eqOn        = mk ("eqOn", 0, 1, 1);
        eqLowFreq   = mk ("eqLowF", 40.0f, 500.0f, 120.0f, 150.0f);
        eqLowGain   = mk ("eqLowG", -15.0f, 15.0f, 0.0f);
        eqMidFreq   = mk ("eqMidF", 200.0f, 8000.0f, 1000.0f, 1200.0f);
        eqMidGain   = mk ("eqMidG", -15.0f, 15.0f, 0.0f);
        eqMidQ      = mk ("eqMidQ", 0.3f, 6.0f, 1.0f, 1.2f);
        eqHighFreq  = mk ("eqHighF", 1500.0f, 16000.0f, 6000.0f, 5000.0f);
        eqHighGain  = mk ("eqHighG", -15.0f, 15.0f, 0.0f);
        deessThresh = mk ("deess", -60.0f, 0.0f, 0.0f);
        drive       = mk ("drive", 0.0f, 1.0f, 0.0f);
        sendReverb  = mk ("sendRev", -60.0f, 6.0f, -60.0f, -15.0f);
        sendDelay   = mk ("sendDly", -60.0f, 6.0f, -60.0f, -15.0f);
        stereo      = mk ("stereo", 0, 1, 0);
        sourceL     = mk ("srcL", -1.0f, 63.0f, -1.0f);
        sourceR     = mk ("srcR", -1.0f, 63.0f, -1.0f);
        active      = mk ("active", 0, 1, 0);

        for (int b = 0; b < kNumBuses; ++b)
            routes[(size_t) b] = mk (("bus" + juce::String (b)).toRawUTF8(), 0, 1, b == 0 ? 1.0f : 0.0f);

        for (auto* ptr : { fader.get(), trim.get(), mute.get(), solo.get(), polarity.get(), pan.get(), tone.get(),
                           hpfFreq.get(), gateThresh.get(), gateRange.get(), gateAttack.get(), gateHold.get(),
                           gateRelease.get(), compThresh.get(), compRatio.get(), compAttack.get(), compRelease.get(),
                           compKnee.get(), compMakeup.get(), compAuto.get(), eqOn.get(), eqLowFreq.get(),
                           eqLowGain.get(), eqMidFreq.get(), eqMidGain.get(), eqMidQ.get(), eqHighFreq.get(),
                           eqHighGain.get(), deessThresh.get(), drive.get(), sendReverb.get(), sendDelay.get(),
                           stereo.get(), sourceL.get(), sourceR.get(), active.get() })
            add (*ptr);

        for (auto& r : routes)
            add (*r);
    }

    std::unique_ptr<Param> fader, trim, mute, solo, polarity, pan, tone, hpfFreq,
        gateThresh, gateRange, gateAttack, gateHold, gateRelease,
        compThresh, compRatio, compAttack, compRelease, compKnee, compMakeup, compAuto,
        eqOn, eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain,
        deessThresh, drive, sendReverb, sendDelay, stereo, sourceL, sourceR, active;
    std::array<std::unique_ptr<Param>, kNumBuses> routes;

    // Not saved as a param: display name (message thread only).
    juce::String label;
};

struct BusParams : ParamGroup
{
    explicit BusParams (int index)
    {
        const auto p = "b" + juce::String (index) + "_";
        auto mk = [&] (const char* name, float mn, float mx, float def, float skew = Param::noSkew)
        {
            return std::make_unique<Param> (p + name, mn, mx, def, skew);
        };

        fader     = mk ("fader", -60.0f, 12.0f, 0.0f, -9.0f);
        mute      = mk ("mute", 0, 1, 0);
        mono      = mk ("mono", 0, 1, 0);
        limiter   = mk ("limiter", 0, 1, 1);
        limitCeil = mk ("limitCeil", -12.0f, 0.0f, -0.3f);
        eqOn      = mk ("eqOn", 0, 1, 0);
        eqLowG    = mk ("eqLowG", -12.0f, 12.0f, 0.0f);
        eqMidG    = mk ("eqMidG", -12.0f, 12.0f, 0.0f);
        eqHighG   = mk ("eqHighG", -12.0f, 12.0f, 0.0f);
        outPair   = mk ("outPair", -1.0f, 31.0f, index < kNumHwBuses ? (float) index : -1.0f);

        for (auto* ptr : { fader.get(), mute.get(), mono.get(), limiter.get(), limitCeil.get(), eqOn.get(),
                           eqLowG.get(), eqMidG.get(), eqHighG.get(), outPair.get() })
            add (*ptr);
    }

    std::unique_ptr<Param> fader, mute, mono, limiter, limitCeil, eqOn, eqLowG, eqMidG, eqHighG, outPair;
};

struct FxParams : ParamGroup
{
    explicit FxParams (int index)
    {
        const auto p = "fx" + juce::String (index) + "_";
        auto mk = [&] (const char* name, float mn, float mx, float def, float skew = Param::noSkew)
        {
            return std::make_unique<Param> (p + name, mn, mx, def, skew);
        };

        returnGain = mk ("ret", -60.0f, 6.0f, 0.0f, -12.0f);
        mute       = mk ("mute", 0, 1, 0);
        size       = mk ("size", 0.0f, 1.0f, 0.5f);     // reverb room size
        damping    = mk ("damp", 0.0f, 1.0f, 0.5f);     // reverb damping
        width      = mk ("width", 0.0f, 1.0f, 1.0f);
        timeMs     = mk ("time", 20.0f, 1500.0f, 350.0f, 300.0f); // delay time
        feedback   = mk ("fb", 0.0f, 0.95f, 0.35f);
        tone       = mk ("tone", 1000.0f, 16000.0f, 6000.0f, 5000.0f); // delay lowpass

        for (int b = 0; b < kNumBuses; ++b)
            routes[(size_t) b] = mk (("bus" + juce::String (b)).toRawUTF8(), 0, 1, b == 0 ? 1.0f : 0.0f);

        for (auto* ptr : { returnGain.get(), mute.get(), size.get(), damping.get(), width.get(), timeMs.get(),
                           feedback.get(), tone.get() })
            add (*ptr);
        for (auto& r : routes)
            add (*r);
    }

    std::unique_ptr<Param> returnGain, mute, size, damping, width, timeMs, feedback, tone;
    std::array<std::unique_ptr<Param>, kNumBuses> routes;
};

// Atomic stereo meter written by the audio thread, read (and decayed) by the UI.
struct Meter
{
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f }, gainReduction { 0.0f }, gateGain { 1.0f };

    void push (float l, float r) noexcept
    {
        if (l > peakL.load (std::memory_order_relaxed)) peakL.store (l, std::memory_order_relaxed);
        if (r > peakR.load (std::memory_order_relaxed)) peakR.store (r, std::memory_order_relaxed);
    }

    // UI side: read and reset (the meter component applies its own ballistics).
    std::pair<float, float> take() noexcept
    {
        return { peakL.exchange (0.0f), peakR.exchange (0.0f) };
    }
};
