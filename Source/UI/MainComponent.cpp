#include "MainComponent.h"
#include "Widgets.h"

namespace
{
constexpr int stripWidth = 122;
constexpr int fxWidth = 112;
constexpr int busWidth = 100;
constexpr int headerHeight = 46;

juce::String fmtDb (float v)    { return v <= -59.9f ? juce::String ("-inf") : (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String fmtHz (float v)    { return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String ((int) v) + " Hz"; }
juce::String fmtMs (float v)    { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String ((int) v) + " ms"; }

// ---------------------------------------------------------------------------------------------
// Channel presets: one-click starting points for common mic / instrument sources.
void applyStripPreset (StripParams& p, const juce::String& name)
{
    for (auto* x : { p.trim.get(), p.polarity.get(), p.tone.get(), p.hpfFreq.get(), p.gateThresh.get(), p.gateRange.get(),
                     p.gateAttack.get(), p.gateHold.get(), p.gateRelease.get(), p.compThresh.get(), p.compRatio.get(),
                     p.compAttack.get(), p.compRelease.get(), p.compKnee.get(), p.compMakeup.get(), p.compAuto.get(),
                     p.eqOn.get(), p.eqLowFreq.get(), p.eqLowGain.get(), p.eqMidFreq.get(), p.eqMidGain.get(), p.eqMidQ.get(),
                     p.eqHighFreq.get(), p.eqHighGain.get(), p.deessThresh.get(), p.drive.get(), p.sendReverb.get(),
                     p.sendDelay.get() })
        x->reset();

    auto set = [] (std::unique_ptr<Param>& prm, float v) { prm->set (v); };

    if (name == "Vocal")
    {
        set (p.hpfFreq, 90); set (p.gateThresh, -55); set (p.gateRange, -25);
        set (p.eqLowFreq, 120); set (p.eqLowGain, -2); set (p.eqMidFreq, 350); set (p.eqMidGain, -3); set (p.eqMidQ, 1.2f);
        set (p.eqHighFreq, 8000); set (p.eqHighGain, 3);
        set (p.compThresh, -22); set (p.compRatio, 3); set (p.compAttack, 8); set (p.compRelease, 120);
        set (p.deessThresh, -28);
    }
    else if (name == "Podcast / Stream")
    {
        set (p.hpfFreq, 80); set (p.gateThresh, -50); set (p.gateRange, -30);
        set (p.eqLowFreq, 100); set (p.eqLowGain, 2); set (p.eqMidFreq, 3000); set (p.eqMidGain, 2);
        set (p.eqHighFreq, 10000); set (p.eqHighGain, 2);
        set (p.compThresh, -26); set (p.compRatio, 4); set (p.compAttack, 5); set (p.compRelease, 100);
        set (p.deessThresh, -30);
    }
    else if (name == "Electric Guitar (DI)")
    {
        set (p.hpfFreq, 70); set (p.gateThresh, -60); set (p.gateRange, -40); set (p.gateRelease, 200);
        set (p.eqMidFreq, 800); set (p.eqMidGain, 2); set (p.eqMidQ, 0.8f); set (p.eqHighFreq, 5000); set (p.eqHighGain, -3);
        set (p.compThresh, -18); set (p.compRatio, 4); set (p.compAttack, 15); set (p.compRelease, 200);
        set (p.drive, 0.25f); set (p.sendReverb, -18);
    }
    else if (name == "Acoustic Guitar")
    {
        set (p.hpfFreq, 80); set (p.eqMidFreq, 250); set (p.eqMidGain, -3); set (p.eqHighFreq, 10000); set (p.eqHighGain, 2);
        set (p.compThresh, -18); set (p.compRatio, 2.5f); set (p.compAttack, 12); set (p.sendReverb, -20);
    }
    else if (name == "Bass (DI)")
    {
        set (p.hpfFreq, 30); set (p.eqLowFreq, 80); set (p.eqLowGain, 3); set (p.eqMidFreq, 700); set (p.eqMidGain, -2);
        set (p.compThresh, -20); set (p.compRatio, 4); set (p.compAttack, 20); set (p.compRelease, 150);
        set (p.drive, 0.15f);
    }
    else if (name == "Keys / Synth")
    {
        set (p.hpfFreq, 25); set (p.compThresh, -12); set (p.compRatio, 2);
    }
}

const juce::StringArray presetNames { "Flat", "Vocal", "Podcast / Stream", "Electric Guitar (DI)", "Acoustic Guitar",
                                      "Bass (DI)", "Keys / Synth" };

void styleCaption (juce::Label& l, juce::Colour c, float size = 12.0f)
{
    l.setFont (theme::font (size, true));
    l.setColour (juce::Label::textColourId, c);
    l.setJustificationType (juce::Justification::centred);
    l.setInterceptsMouseClicks (false, false);
}

void paintPanel (juce::Graphics& g, juce::Rectangle<int> b, juce::Colour accent)
{
    auto r = b.toFloat().reduced (2.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (theme::border);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (r.removeFromTop (3.0f).reduced (6.0f, 0), 1.5f);
}

std::unique_ptr<Knob> makeGateKnob (Param& p, juce::Colour c)
{
    // Clockwise = gate more (threshold rises from off at -80 dB).
    auto k = std::make_unique<Knob> (p, "GATE", c, [] (float v) { return -80.0f + v; }, [] (float pv) { return pv + 80.0f; },
                                     juce::NormalisableRange<double> (0.0, 70.0));
    k->setFormatter ([] (float v) { return v <= -79.5f ? juce::String ("OFF") : juce::String ((int) v) + " dB"; });
    return k;
}

std::unique_ptr<Knob> makeCompKnob (Param& p, juce::Colour c)
{
    // Clockwise = compress more (threshold falls from 0 dB).
    auto k = std::make_unique<Knob> (p, "COMP", c, [] (float v) { return -v; }, [] (float pv) { return -pv; },
                                     juce::NormalisableRange<double> (0.0, 50.0));
    k->setFormatter ([] (float v) { return v >= -0.1f ? juce::String ("OFF") : juce::String ((int) v) + " dB"; });
    return k;
}
} // namespace

// =============================================================================================
class StripComponent : public juce::Component
{
public:
    StripComponent (Engine& e, MainComponent& mc, int index) : engine (e), main (mc), s (index), p (*e.strips[(size_t) index])
    {
        accent = s < kNumHwStrips ? theme::hw : theme::virt;

        name.setText (p.label, juce::dontSendNotification);
        name.setFont (theme::font (13.0f, true));
        name.setColour (juce::Label::textColourId, accent);
        name.setJustificationType (juce::Justification::centred);
        name.setEditable (false, true, false);
        name.setTooltip ("Double-click to rename");
        name.onTextChange = [this] { p.label = name.getText(); };
        addAndMakeVisible (name);

        source.setButtonText (engine.describeSource (s));
        source.setTooltip ("Choose input source");
        source.onClick = [this] { showSourceMenu(); };
        addAndMakeVisible (source);

        pad = std::make_unique<XYPad> (*p.pan, *p.tone, accent);
        pad->setTooltip ("Drag: X = pan, Y = tone (warm/bright). Double-click to reset.");
        addAndMakeVisible (*pad);

        gate = makeGateKnob (*p.gateThresh, accent);
        comp = makeCompKnob (*p.compThresh, accent);
        rev = std::make_unique<Knob> (*p.sendReverb, "REVERB", theme::fx);
        dly = std::make_unique<Knob> (*p.sendDelay, "DELAY", theme::fx);
        for (auto* k : { rev.get(), dly.get() })
            k->setFormatter ([] (float v) { return v <= -59.9f ? juce::String ("OFF") : fmtDb (v); });
        for (auto* k : { gate.get(), comp.get(), rev.get(), dly.get() })
            addAndMakeVisible (*k);

        gateLed = std::make_unique<DynamicsLed> (e.stripMeters[(size_t) s], true);
        compLed = std::make_unique<DynamicsLed> (e.stripMeters[(size_t) s], false);
        addAndMakeVisible (*gateLed);
        addAndMakeVisible (*compLed);

        for (int b = 0; b < kNumBuses; ++b)
        {
            auto* t = routeButtons.add (new ParamToggle (*p.routes[(size_t) b], busName (b), b < kNumHwBuses ? theme::busA : theme::busB));
            t->setTooltip ("Send to bus " + busName (b));
            addAndMakeVisible (t);
        }

        fader = std::make_unique<FaderMeter> (*p.fader, e.stripMeters[(size_t) s], accent);
        addAndMakeVisible (*fader);

        solo = std::make_unique<ParamToggle> (*p.solo, "S", theme::solo);
        mute = std::make_unique<ParamToggle> (*p.mute, "M", theme::mute);
        fxBtn.setTooltip ("Open channel EQ / dynamics / presets");
        fxBtn.onClick = [this] { main.openDetail (s); };
        for (juce::Component* c : { (juce::Component*) solo.get(), (juce::Component*) mute.get(), (juce::Component*) &fxBtn })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override { paintPanel (g, getLocalBounds(), accent); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (7, 6);
        r.removeFromTop (3);
        name.setBounds (r.removeFromTop (20));
        source.setBounds (r.removeFromTop (18));
        r.removeFromTop (5);
        pad->setBounds (r.removeFromTop (70));
        r.removeFromTop (4);

        auto knobRow = [&] (Knob& a, Knob& b)
        {
            auto row = r.removeFromTop (60);
            a.setBounds (row.removeFromLeft (row.getWidth() / 2));
            b.setBounds (row);
        };
        knobRow (*gate, *comp);
        auto leds = r.removeFromTop (4);
        gateLed->setBounds (leds.removeFromLeft (leds.getWidth() / 2).reduced (8, 0));
        compLed->setBounds (leds.reduced (8, 0));
        r.removeFromTop (2);
        knobRow (*rev, *dly);
        r.removeFromTop (4);

        auto row1 = r.removeFromTop (22), row2 = r.removeFromTop (22);
        const int w1 = row1.getWidth() / kNumHwBuses, w2 = row2.getWidth() / kNumVirtBuses;
        for (int b = 0; b < kNumBuses; ++b)
            routeButtons[b]->setBounds (b < kNumHwBuses ? row1.removeFromLeft (w1).reduced (1)
                                                        : row2.removeFromLeft (w2).reduced (1));

        auto bottom = r.removeFromBottom (24);
        const int bw = bottom.getWidth() / 3;
        solo->setBounds (bottom.removeFromLeft (bw).reduced (1));
        mute->setBounds (bottom.removeFromLeft (bw).reduced (1));
        fxBtn.setBounds (bottom.reduced (1));
        r.removeFromTop (6);
        fader->setBounds (r);
    }

private:
    void showSourceMenu()
    {
        juce::PopupMenu m;
        if (s < kNumHwStrips)
        {
            const auto names = engine.getInputChannelNames();
            juce::PopupMenu mono, stereo;
            for (int i = 0; i < names.size(); ++i)
                mono.addItem (1000 + i, names[i], true, ! p.stereo->on() && (int) p.sourceL->get() == i);
            for (int i = 0; i + 1 < names.size(); i += 2)
                stereo.addItem (2000 + i, names[i] + " + " + names[i + 1], true,
                                p.stereo->on() && (int) p.sourceL->get() == i);
            m.addSectionHeader ("Input channel");
            m.addSubMenu ("Mono", mono);
            m.addSubMenu ("Stereo pair", stereo);
            m.addSeparator();
            m.addItem (1, "Hide this strip");
        }
        else
        {
            const int v = s - kNumHwStrips;
            const auto devices = ExternalBridge::getDeviceNames (ExternalBridge::Direction::input);
            m.addSectionHeader ("Windows recording device (WASAPI)");
            m.addItem (3000, "None", true, engine.inBridgeNames[(size_t) v].isEmpty());
            for (int i = 0; i < devices.size(); ++i)
                m.addItem (3001 + i, devices[i], true, engine.inBridgeNames[(size_t) v] == devices[i]);

            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source),
                             [this, devices, v] (int id)
                             {
                                 if (id == 3000) engine.setInBridgeDevice (v, {});
                                 else if (id > 3000) engine.setInBridgeDevice (v, devices[id - 3001]);
                             });
            return;
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&source), [this] (int id)
        {
            if (id == 1)
            {
                p.active->set (0.0f);
                if (engine.onLayoutChanged) engine.onLayoutChanged();
                return;
            }
            if (id >= 2000)      { p.stereo->set (1.0f); p.sourceL->set ((float) (id - 2000)); p.sourceR->set ((float) (id - 1999)); }
            else if (id >= 1000) { p.stereo->set (0.0f); p.sourceL->set ((float) (id - 1000)); p.sourceR->set (-1.0f); }
            else return;
            source.setButtonText (engine.describeSource (s));
        });
    }

    Engine& engine;
    MainComponent& main;
    int s;
    StripParams& p;
    juce::Colour accent;

    juce::Label name;
    juce::TextButton source, fxBtn { "FX" };
    std::unique_ptr<XYPad> pad;
    std::unique_ptr<Knob> gate, comp, rev, dly;
    std::unique_ptr<DynamicsLed> gateLed, compLed;
    juce::OwnedArray<ParamToggle> routeButtons;
    std::unique_ptr<FaderMeter> fader;
    std::unique_ptr<ParamToggle> solo, mute;
};

// =============================================================================================
class FxReturnComponent : public juce::Component
{
public:
    FxReturnComponent (Engine& e, int index) : f (index), p (*e.fx[(size_t) index])
    {
        styleCaption (title, theme::fx, 13.0f);
        title.setText (f == 0 ? "REVERB" : "DELAY", juce::dontSendNotification);
        addAndMakeVisible (title);

        if (f == 0)
        {
            k1 = std::make_unique<Knob> (*p.size, "SIZE", theme::fx);
            k2 = std::make_unique<Knob> (*p.damping, "DAMP", theme::fx);
            k3 = std::make_unique<Knob> (*p.width, "WIDTH", theme::fx);
            for (auto* k : { k1.get(), k2.get(), k3.get() })
                k->setFormatter ([] (float v) { return juce::String ((int) (v * 100)) + "%"; });
        }
        else
        {
            k1 = std::make_unique<Knob> (*p.timeMs, "TIME", theme::fx);
            k2 = std::make_unique<Knob> (*p.feedback, "FDBK", theme::fx);
            k3 = std::make_unique<Knob> (*p.tone, "TONE", theme::fx);
            k1->setFormatter (fmtMs);
            k2->setFormatter ([] (float v) { return juce::String ((int) (v * 100)) + "%"; });
            k3->setFormatter (fmtHz);
        }
        for (auto* k : { k1.get(), k2.get(), k3.get() })
            addAndMakeVisible (*k);

        for (int b = 0; b < kNumBuses; ++b)
            addAndMakeVisible (routeButtons.add (new ParamToggle (*p.routes[(size_t) b], busName (b),
                                                                  b < kNumHwBuses ? theme::busA : theme::busB)));

        fader = std::make_unique<FaderMeter> (*p.returnGain, e.fxMeters[(size_t) f], theme::fx);
        mute = std::make_unique<ParamToggle> (*p.mute, "MUTE", theme::mute);
        addAndMakeVisible (*fader);
        addAndMakeVisible (*mute);
    }

    void paint (juce::Graphics& g) override { paintPanel (g, getLocalBounds(), theme::fx); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (7, 6);
        r.removeFromTop (3);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        auto row = r.removeFromTop (56);
        k1->setBounds (row.removeFromLeft (row.getWidth() / 2));
        k2->setBounds (row);
        k3->setBounds (r.removeFromTop (56).withSizeKeepingCentre (r.getWidth() / 2, 56));
        r.removeFromTop (8);
        auto row1 = r.removeFromTop (22), row2 = r.removeFromTop (22);
        const int w1 = row1.getWidth() / kNumHwBuses, w2 = row2.getWidth() / kNumVirtBuses;
        for (int b = 0; b < kNumBuses; ++b)
            routeButtons[b]->setBounds (b < kNumHwBuses ? row1.removeFromLeft (w1).reduced (1)
                                                        : row2.removeFromLeft (w2).reduced (1));
        mute->setBounds (r.removeFromBottom (24).reduced (1));
        r.removeFromTop (6);
        fader->setBounds (r);
    }

private:
    int f;
    FxParams& p;
    juce::Label title;
    std::unique_ptr<Knob> k1, k2, k3;
    juce::OwnedArray<ParamToggle> routeButtons;
    std::unique_ptr<FaderMeter> fader;
    std::unique_ptr<ParamToggle> mute;
};

// =============================================================================================
class BusComponent : public juce::Component
{
public:
    BusComponent (Engine& e, int index) : engine (e), b (index), p (*e.buses[(size_t) index])
    {
        accent = b < kNumHwBuses ? theme::busA : theme::busB;
        styleCaption (title, accent, 15.0f);
        title.setText (busName (b), juce::dontSendNotification);
        addAndMakeVisible (title);

        output.onClick = [this] { showOutputMenu(); };
        output.setTooltip (b < kNumHwBuses ? "Hardware output pair" : "Windows playback device (e.g. VB-Cable 'CABLE Input')");
        addAndMakeVisible (output);
        refreshOutputText();

        lo = std::make_unique<Knob> (*p.eqLowG, "LO", accent);
        mid = std::make_unique<Knob> (*p.eqMidG, "MID", accent);
        hi = std::make_unique<Knob> (*p.eqHighG, "HI", accent);
        for (auto* k : { lo.get(), mid.get(), hi.get() })
        {
            k->setFormatter ([] (float v) { return (v > 0 ? "+" : "") + juce::String (v, 1); });
            addAndMakeVisible (*k);
        }

        eq = std::make_unique<ParamToggle> (*p.eqOn, "EQ", accent);
        lim = std::make_unique<ParamToggle> (*p.limiter, "LIM", accent);
        mono = std::make_unique<ParamToggle> (*p.mono, "MONO", accent);
        mute = std::make_unique<ParamToggle> (*p.mute, "MUTE", theme::mute);
        lim->setTooltip ("Output limiter (protects ears & stream from clipping)");
        for (auto* t : { eq.get(), lim.get(), mono.get(), mute.get() })
            addAndMakeVisible (*t);

        fader = std::make_unique<FaderMeter> (*p.fader, e.busMeters[(size_t) b], accent);
        addAndMakeVisible (*fader);
    }

    void paint (juce::Graphics& g) override { paintPanel (g, getLocalBounds(), accent); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 6);
        r.removeFromTop (3);
        title.setBounds (r.removeFromTop (20));
        output.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        auto knobs = r.removeFromTop (54);
        const int kw = knobs.getWidth() / 3;
        lo->setBounds (knobs.removeFromLeft (kw));
        mid->setBounds (knobs.removeFromLeft (kw));
        hi->setBounds (knobs);
        r.removeFromTop (2);
        auto row = r.removeFromTop (22);
        eq->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (1));
        lim->setBounds (row.reduced (1));
        row = r.removeFromBottom (22);
        mono->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (1));
        mute->setBounds (row.reduced (1));
        r.removeFromTop (6);
        fader->setBounds (r);
    }

private:
    void refreshOutputText()
    {
        if (b < kNumHwBuses)
        {
            const int pair = (int) p.outPair->get();
            const auto names = engine.getOutputChannelNames();
            output.setButtonText (pair < 0 ? juce::String ("(off)")
                                  : pair * 2 + 1 < names.size() ? "Out " + juce::String (pair * 2 + 1) + "/" + juce::String (pair * 2 + 2)
                                                                : juce::String ("(n/a)"));
            output.setTooltip (pair >= 0 && pair * 2 + 1 < names.size() ? names[pair * 2] + " + " + names[pair * 2 + 1]
                                                                        : juce::String ("Hardware output pair"));
        }
        else
        {
            auto n = engine.outBridgeNames[(size_t) (b - kNumHwBuses)];
            output.setButtonText (n.isEmpty() ? juce::String ("(none)") : n.upToFirstOccurrenceOf (" (", false, false));
            output.setTooltip (n.isEmpty() ? juce::String ("Choose a playback device, e.g. VB-Cable 'CABLE Input'") : n);
        }
    }

    void showOutputMenu()
    {
        juce::PopupMenu m;
        if (b < kNumHwBuses)
        {
            const auto names = engine.getOutputChannelNames();
            m.addSectionHeader ("Hardware output pair");
            m.addItem (1, "Off", true, p.outPair->get() < 0);
            for (int i = 0; i + 1 < names.size(); i += 2)
                m.addItem (100 + i / 2, names[i] + " + " + names[i + 1], true, (int) p.outPair->get() == i / 2);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&output), [this] (int id)
            {
                if (id == 1) p.outPair->set (-1.0f);
                else if (id >= 100) p.outPair->set ((float) (id - 100));
                refreshOutputText();
            });
        }
        else
        {
            const int v = b - kNumHwBuses;
            const auto devices = ExternalBridge::getDeviceNames (ExternalBridge::Direction::output);
            m.addSectionHeader ("Windows playback device (WASAPI)");
            m.addItem (1, "None", true, engine.outBridgeNames[(size_t) v].isEmpty());
            for (int i = 0; i < devices.size(); ++i)
                m.addItem (100 + i, devices[i], true, engine.outBridgeNames[(size_t) v] == devices[i]);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&output), [this, devices, v] (int id)
            {
                if (id == 1) engine.setOutBridgeDevice (v, {});
                else if (id >= 100) engine.setOutBridgeDevice (v, devices[id - 100]);
                refreshOutputText();
            });
        }
    }

    Engine& engine;
    int b;
    BusParams& p;
    juce::Colour accent;
    juce::Label title;
    juce::TextButton output;
    std::unique_ptr<Knob> lo, mid, hi;
    std::unique_ptr<ParamToggle> eq, lim, mono, mute;
    std::unique_ptr<FaderMeter> fader;
};

// =============================================================================================
// EQ curve display for the detail panel (HPF + 3 bands + tone tilt).
class EqCurve : public juce::Component, private juce::Timer
{
public:
    explicit EqCurve (StripParams& sp, juce::Colour c) : p (sp), accent (c) { startTimerHz (20); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 4.0f);

        const double sr = 48000.0;
        auto xForF = [&] (double f) { return r.getX() + (float) (std::log (f / 20.0) / std::log (1000.0)) * r.getWidth(); };
        auto yForDb = [&] (double db) { return r.getCentreY() - (float) (db / 18.0) * r.getHeight() * 0.5f; };

        g.setFont (theme::font (9.0f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            g.setColour (theme::border.withAlpha (0.5f));
            g.drawVerticalLine ((int) xForF (f), r.getY(), r.getBottom());
            g.setColour (theme::dim);
            g.drawText (fmtHz ((float) f), (int) xForF (f) + 2, (int) r.getBottom() - 12, 50, 10, juce::Justification::left);
        }
        for (double db : { -12.0, -6.0, 0.0, 6.0, 12.0 })
        {
            g.setColour (db == 0.0 ? theme::dim.withAlpha (0.6f) : theme::border.withAlpha (0.5f));
            g.drawHorizontalLine ((int) yForDb (db), r.getX(), r.getRight());
        }

        bvm::Biquad hpf, low, mid, high, tl, th;
        const bool hpfOn = p.hpfFreq->get() > 21.0f, eqOn = p.eqOn->on();
        hpf.setHighPass (sr, p.hpfFreq->get());
        low.setLowShelf (sr, p.eqLowFreq->get(), p.eqLowGain->get());
        mid.setPeak (sr, p.eqMidFreq->get(), p.eqMidQ->get(), p.eqMidGain->get());
        high.setHighShelf (sr, p.eqHighFreq->get(), p.eqHighGain->get());
        tl.setLowShelf (sr, 300.0, -p.tone->get() * 6.0, 0.7);
        th.setHighShelf (sr, 3000.0, p.tone->get() * 6.0, 0.7);

        juce::Path path;
        for (int x = 0; x <= (int) r.getWidth(); x += 2)
        {
            const double f = 20.0 * std::pow (1000.0, x / (double) r.getWidth());
            double mag = tl.magnitudeAt (f, sr) * th.magnitudeAt (f, sr);
            if (hpfOn) mag *= hpf.magnitudeAt (f, sr);
            if (eqOn) mag *= low.magnitudeAt (f, sr) * mid.magnitudeAt (f, sr) * high.magnitudeAt (f, sr);
            const float y = juce::jlimit (r.getY(), r.getBottom(), yForDb (20.0 * std::log10 (juce::jmax (1.0e-6, mag))));
            if (x == 0) path.startNewSubPath (r.getX(), y);
            else path.lineTo (r.getX() + (float) x, y);
        }
        auto fill = path;
        fill.lineTo (r.getRight(), r.getCentreY());
        fill.lineTo (r.getX(), r.getCentreY());
        fill.closeSubPath();
        g.setColour (accent.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (path, juce::PathStrokeType (2.0f));
    }

private:
    void timerCallback() override
    {
        float sum = 0;
        for (auto* x : { p.hpfFreq.get(), p.eqLowFreq.get(), p.eqLowGain.get(), p.eqMidFreq.get(), p.eqMidGain.get(),
                         p.eqMidQ.get(), p.eqHighFreq.get(), p.eqHighGain.get(), p.tone.get(), p.eqOn.get() })
            sum = sum * 1.37f + x->get();
        if (sum != last) { last = sum; repaint(); }
    }

    StripParams& p;
    juce::Colour accent;
    float last = 0;
};

// =============================================================================================
// Full channel editor: trim, polarity, HPF, gate, 3-band EQ, de-esser, compressor, drive, sends.
class DetailPanel : public juce::Component
{
public:
    DetailPanel (Engine& e, MainComponent& mc, int index)
        : main (mc), s (index), p (*e.strips[(size_t) index]), meter (e.stripMeters[(size_t) index]),
          inputMeter (e.stripInputMeters[(size_t) index]), curve (p, index < kNumHwStrips ? theme::hw : theme::virt)
    {
        accent = s < kNumHwStrips ? theme::hw : theme::virt;
        setWantsKeyboardFocus (true);

        title.setText (p.label + "  -  Channel Strip", juce::dontSendNotification);
        styleCaption (title, accent, 17.0f);
        title.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (title);

        closeBtn.onClick = [this] { main.closeDetail(); };
        addAndMakeVisible (closeBtn);

        presetBtn.onClick = [this]
        {
            juce::PopupMenu m;
            for (int i = 0; i < presetNames.size(); ++i)
                m.addItem (i + 1, presetNames[i]);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn), [this] (int id)
            {
                if (id > 0) applyStripPreset (p, presetNames[id - 1]);
            });
        };
        presetBtn.setTooltip ("Load a starting point for a mic or instrument");
        addAndMakeVisible (presetBtn);

        auto knob = [this] (Param& prm, const char* cap, std::function<juce::String (float)> fmt, juce::Colour c = {})
        {
            auto* k = knobs.add (new Knob (prm, cap, c.isTransparent() ? accent : c));
            k->setFormatter (std::move (fmt));
            addAndMakeVisible (k);
            return k;
        };

        // INPUT
        trim = knob (*p.trim, "TRIM", fmtDb);
        hpf = knob (*p.hpfFreq, "LOW CUT", [] (float v) { return v <= 21.0f ? juce::String ("OFF") : fmtHz (v); });
        drive = knob (*p.drive, "DRIVE", [] (float v) { return v < 0.005f ? juce::String ("OFF") : juce::String ((int) (v * 100)) + "%"; });
        polarity = std::make_unique<ParamToggle> (*p.polarity, juce::String::fromUTF8 ("\xc3\x98 PHASE"), accent);
        addAndMakeVisible (*polarity);
        inMeter = std::make_unique<LevelMeter> (inputMeter);
        addAndMakeVisible (*inMeter);

        // GATE
        gThresh = knob (*p.gateThresh, "THRESH", [] (float v) { return v <= -79.5f ? juce::String ("OFF") : fmtDb (v); });
        gRange = knob (*p.gateRange, "RANGE", [] (float v) { return juce::String ((int) v) + " dB"; });
        gAttack = knob (*p.gateAttack, "ATTACK", fmtMs);
        gHold = knob (*p.gateHold, "HOLD", fmtMs);
        gRelease = knob (*p.gateRelease, "RELEASE", fmtMs);
        gateLed = std::make_unique<DynamicsLed> (meter, true);
        addAndMakeVisible (*gateLed);

        // EQ
        eqOn = std::make_unique<ParamToggle> (*p.eqOn, "EQ ON", accent);
        addAndMakeVisible (*eqOn);
        addAndMakeVisible (curve);
        lowF = knob (*p.eqLowFreq, "LOW FREQ", fmtHz);
        lowG = knob (*p.eqLowGain, "LOW", fmtDb);
        midF = knob (*p.eqMidFreq, "MID FREQ", fmtHz);
        midG = knob (*p.eqMidGain, "MID", fmtDb);
        midQ = knob (*p.eqMidQ, "MID Q", [] (float v) { return juce::String (v, 2); });
        highF = knob (*p.eqHighFreq, "HIGH FREQ", fmtHz);
        highG = knob (*p.eqHighGain, "HIGH", fmtDb);

        // DYNAMICS
        cThresh = knob (*p.compThresh, "THRESH", [] (float v) { return v >= -0.1f ? juce::String ("OFF") : fmtDb (v); });
        cRatio = knob (*p.compRatio, "RATIO", [] (float v) { return juce::String (v, 1) + ":1"; });
        cAttack = knob (*p.compAttack, "ATTACK", fmtMs);
        cRelease = knob (*p.compRelease, "RELEASE", fmtMs);
        cKnee = knob (*p.compKnee, "KNEE", fmtDb);
        cMakeup = knob (*p.compMakeup, "MAKEUP", fmtDb);
        deess = knob (*p.deessThresh, "DE-ESS", [] (float v) { return v >= -0.5f ? juce::String ("OFF") : fmtDb (v); });
        compAuto = std::make_unique<ParamToggle> (*p.compAuto, "AUTO GAIN", accent);
        addAndMakeVisible (*compAuto);
        compLed = std::make_unique<DynamicsLed> (meter, false);
        addAndMakeVisible (*compLed);

        // SENDS / OUTPUT
        auto offDb = [] (float v) { return v <= -59.9f ? juce::String ("OFF") : fmtDb (v); };
        sRev = knob (*p.sendReverb, "REVERB", offDb, theme::fx);
        sDly = knob (*p.sendDelay, "DELAY", offDb, theme::fx);
        pan = knob (*p.pan, "PAN", [] (float v)
        {
            return std::abs (v) < 0.01f ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String ((int) std::abs (v * 100));
        });
        tone = knob (*p.tone, "TONE", [] (float v)
        {
            return std::abs (v) < 0.01f ? juce::String ("0") : (v < 0 ? "Warm " : "Bright ") + juce::String ((int) std::abs (v * 100));
        });
        outMeter = std::make_unique<LevelMeter> (meter);
        addAndMakeVisible (*outMeter);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));
        auto r = panelBounds.toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (accent.withAlpha (0.6f));
        g.drawRoundedRectangle (r, 8.0f, 1.5f);

        g.setFont (theme::font (11.0f, true));
        for (auto& [area, text] : sections)
        {
            g.setColour (theme::panelHi.withAlpha (0.5f));
            g.fillRoundedRectangle (area.toFloat(), 5.0f);
            g.setColour (theme::dim);
            g.drawText (text, area.reduced (8, 4).removeFromTop (14), juce::Justification::left);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelBounds.contains (e.getPosition()))
            main.closeDetail();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { main.closeDetail(); return true; }
        return false;
    }

    void resized() override
    {
        panelBounds = getLocalBounds().withSizeKeepingCentre (juce::jmin (getWidth() - 40, 980), juce::jmin (getHeight() - 30, 560));
        auto r = panelBounds.reduced (16, 12);

        auto top = r.removeFromTop (30);
        closeBtn.setBounds (top.removeFromRight (30).reduced (2));
        top.removeFromRight (8);
        presetBtn.setBounds (top.removeFromRight (140).reduced (2));
        title.setBounds (top);
        r.removeFromTop (8);

        sections.clear();
        auto section = [&] (juce::Rectangle<int> area, const char* text)
        {
            sections.push_back ({ area, text });
            auto inner = area.reduced (8, 6);
            inner.removeFromTop (16);
            return inner;
        };
        auto placeRow = [] (juce::Rectangle<int> row, std::initializer_list<juce::Component*> comps)
        {
            const int w = row.getWidth() / (int) comps.size();
            for (auto* c : comps)
                c->setBounds (row.removeFromLeft (w));
        };

        auto upper = r.removeFromTop (r.getHeight() / 2 - 4);
        r.removeFromTop (8);
        auto lower = r;

        // Upper row: INPUT | GATE | EQ curve + bands
        auto input = section (upper.removeFromLeft (170), "INPUT");
        inMeter->setBounds (input.removeFromRight (12).reduced (0, 4));
        input.removeFromRight (6);
        polarity->setBounds (input.removeFromBottom (24).reduced (4, 0));
        placeRow (input.removeFromTop (input.getHeight() / 2), { trim });
        placeRow (input, { hpf, drive });
        upper.removeFromLeft (8);

        auto gateArea = section (upper.removeFromLeft (230), "GATE");
        gateLed->setBounds (gateArea.removeFromBottom (6).reduced (20, 0));
        gateArea.removeFromBottom (4);
        auto gTop = gateArea.removeFromTop (gateArea.getHeight() / 2);
        placeRow (gTop, { gThresh, gRange });
        placeRow (gateArea, { gAttack, gHold, gRelease });
        upper.removeFromLeft (8);

        auto eqArea = section (upper, "EQUALIZER");
        auto eqTop = eqArea.removeFromTop (eqArea.getHeight() / 2 + 6);
        eqOn->setBounds (eqTop.removeFromLeft (64).withSizeKeepingCentre (60, 24));
        eqTop.removeFromLeft (6);
        curve.setBounds (eqTop.reduced (0, 2));
        placeRow (eqArea, { lowF, lowG, midF, midG, midQ, highF, highG });

        // Lower row: DYNAMICS | SENDS & OUTPUT
        auto dyn = section (lower.removeFromLeft (lower.getWidth() - 330), "DYNAMICS  (COMPRESSOR + DE-ESSER)");
        auto dynBottom = dyn.removeFromBottom (26);
        compAuto->setBounds (dynBottom.removeFromLeft (110).reduced (2));
        dynBottom.removeFromLeft (10);
        compLed->setBounds (dynBottom.removeFromLeft (200).withSizeKeepingCentre (200, 6));
        placeRow (dyn.reduced (0, 10), { cThresh, cRatio, cAttack, cRelease, cKnee, cMakeup, deess });
        lower.removeFromLeft (8);

        auto out = section (lower, "SENDS & OUTPUT");
        outMeter->setBounds (out.removeFromRight (12).reduced (0, 4));
        out.removeFromRight (6);
        auto outTop = out.removeFromTop (out.getHeight() / 2);
        placeRow (outTop, { sRev, sDly });
        placeRow (out, { pan, tone });
    }

private:
    MainComponent& main;
    int s;
    StripParams& p;
    Meter& meter;
    Meter& inputMeter;
    juce::Colour accent;
    juce::Rectangle<int> panelBounds;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> sections;

    juce::Label title;
    juce::TextButton closeBtn { "X" }, presetBtn { "PRESETS..." };
    juce::OwnedArray<Knob> knobs;
    Knob *trim, *hpf, *drive, *gThresh, *gRange, *gAttack, *gHold, *gRelease, *lowF, *lowG, *midF, *midG, *midQ, *highF,
        *highG, *cThresh, *cRatio, *cAttack, *cRelease, *cKnee, *cMakeup, *deess, *sRev, *sDly, *pan, *tone;
    std::unique_ptr<ParamToggle> polarity, eqOn, compAuto;
    std::unique_ptr<DynamicsLed> gateLed, compLed;
    std::unique_ptr<LevelMeter> inMeter, outMeter;
    EqCurve curve;
};

// =============================================================================================
// Settings window: ASIO/WASAPI device selection + which hardware strips are shown.
class SettingsComponent : public juce::Component
{
public:
    explicit SettingsComponent (Engine& e)
        : engine (e), selector (e.deviceManager, 0, 256, 0, 256, false, false, false, false)
    {
        addAndMakeVisible (selector);

        styleCaption (stripsLabel, theme::text, 13.0f);
        stripsLabel.setJustificationType (juce::Justification::centredLeft);
        stripsLabel.setText ("Visible hardware strips:", juce::dontSendNotification);
        addAndMakeVisible (stripsLabel);

        for (int i = 0; i < kNumHwStrips; ++i)
        {
            auto* t = stripToggles.add (new ParamToggle (*e.strips[(size_t) i]->active, juce::String (i + 1), theme::hw));
            t->onClick = [this, t]
            {
                t->param.set (t->getToggleState() ? 1.0f : 0.0f);
                if (engine.onLayoutChanged) engine.onLayoutChanged();
            };
            addAndMakeVisible (t);
        }

        autoBtn.onClick = [this] { engine.autoAssignStrips(); };
        addAndMakeVisible (autoBtn);

        help.setMultiLine (true);
        help.setReadOnly (true);
        help.setColour (juce::TextEditor::backgroundColourId, theme::panel);
        help.setColour (juce::TextEditor::outlineColourId, theme::border);
        help.setColour (juce::TextEditor::textColourId, theme::dim);
        help.setFont (theme::font (12.0f));
        help.setText ("Low latency tips:\n"
                      " - Use the 'Focusrite USB ASIO' driver and the smallest buffer that stays click-free (32-128 samples).\n"
                      " - Turn OFF 'Direct Monitor' / 'Low latency' mixes in Focusrite Control so you don't hear yourself twice.\n"
                      " - Preamp gain, 48V phantom, INST and AIR are analog hardware settings: set them on the interface / Focusrite Control.\n"
                      " - B1/B2 go to Windows devices: pick VB-Cable 'CABLE Input', then choose 'CABLE Output' as the mic in Discord/OBS.");
        addAndMakeVisible (help);
        setSize (640, 640);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::bg); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        help.setBounds (r.removeFromBottom (92));
        r.removeFromBottom (8);
        auto row = r.removeFromBottom (28);
        stripsLabel.setBounds (row.removeFromLeft (170));
        autoBtn.setBounds (row.removeFromRight (190).reduced (2));
        for (auto* t : stripToggles)
            t->setBounds (row.removeFromLeft (32).reduced (2));
        r.removeFromBottom (8);
        selector.setBounds (r);
    }

private:
    Engine& engine;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label stripsLabel;
    juce::OwnedArray<ParamToggle> stripToggles;
    juce::TextButton autoBtn { "AUTO-ASSIGN FROM INTERFACE" };
    juce::TextEditor help;
};

// =============================================================================================
MainComponent::MainComponent (Engine& e) : engine (e)
{
    setLookAndFeel (&lnf);
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    for (auto* b : { &setupButton, &autoButton, &loadButton, &saveButton })
        addAndMakeVisible (b);
    setupButton.onClick = [this] { openSettings(); };
    autoButton.onClick = [this] { engine.autoAssignStrips(); };
    autoButton.setTooltip ("Create one strip per interface input (pairs Loopback channels into a stereo strip)");
    loadButton.onClick = [this] { loadPreset(); };
    saveButton.onClick = [this] { savePreset(); };

    stripViewport.setViewedComponent (&stripHolder, false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);

    engine.onLayoutChanged = [this] { rebuild(); };
    rebuild();
    setWantsKeyboardFocus (true);
    setSize (1500, 800);
    startTimerHz (4);
}

MainComponent::~MainComponent()
{
    engine.onLayoutChanged = nullptr;
    detail.reset();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::rebuild()
{
    closeDetail();
    stripComps.clear();
    fxComps.clear();
    busComps.clear();

    for (int s = 0; s < kNumStrips; ++s)
        if (engine.strips[(size_t) s]->active->on())
            stripHolder.addAndMakeVisible (stripComps.add (new StripComponent (engine, *this, s)));
    for (int f = 0; f < kNumFx; ++f)
        addAndMakeVisible (fxComps.add (new FxReturnComponent (engine, f)));
    for (int b = 0; b < kNumBuses; ++b)
        addAndMakeVisible (busComps.add (new BusComponent (engine, b)));

    resized();
}

void MainComponent::openDetail (int s)
{
    detail = std::make_unique<DetailPanel> (engine, *this, s);
    addAndMakeVisible (*detail);
    detail->setBounds (getLocalBounds().withTrimmedTop (headerHeight));
    detail->grabKeyboardFocus();
}

void MainComponent::closeDetail()
{
    if (detail != nullptr)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this),
                                          d = detail.release()] { delete d; if (safe) safe->repaint(); });
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && detail != nullptr) { closeDetail(); return true; }
    return false;
}

void MainComponent::timerCallback()
{
    auto text = engine.getLatencyText() + "   |   CPU " + juce::String (engine.getCpuUsage() * 100.0, 1) + "%";
    if (auto* d = engine.deviceManager.getCurrentAudioDevice())
        text = d->getName() + "   |   " + text;
    if (text != statusText)
    {
        statusText = text;
        repaint (headerArea);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);

    auto h = headerArea;
    g.setColour (theme::panel);
    g.fillRect (h);
    g.setColour (theme::border);
    g.drawHorizontalLine (h.getBottom() - 1, 0.0f, (float) getWidth());

    auto titleArea = h.removeFromLeft (300).reduced (14, 0);
    g.setFont (theme::font (20.0f, true));
    g.setColour (theme::text);
    g.drawText ("BETTER", titleArea, juce::Justification::centredLeft);
    g.setColour (theme::hw);
    g.drawText ("VOICEMEETER", titleArea.withTrimmedLeft (78), juce::Justification::centredLeft);

    g.setFont (theme::font (12.0f));
    g.setColour (theme::dim);
    g.drawText (statusText, h.withTrimmedRight (450), juce::Justification::centred);

    g.setFont (theme::font (11.0f, true));
    g.setColour (theme::dim);
    if (! fxArea.isEmpty())
        g.drawText ("FX RETURNS", fxArea.getX(), headerArea.getBottom() + 2, fxArea.getWidth(), 14, juce::Justification::centred);
    if (! masterArea.isEmpty())
        g.drawText ("MASTER SECTION   -   A = HARDWARE OUT   B = VIRTUAL OUT", masterArea.getX(), headerArea.getBottom() + 2,
                    masterArea.getWidth(), 14, juce::Justification::centred);
    g.drawText ("INPUTS", stripViewport.getX(), headerArea.getBottom() + 2, stripViewport.getWidth(), 14, juce::Justification::centred);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (headerHeight);
    {
        auto h = headerArea.reduced (8, 9);
        for (auto* b : { &saveButton, &loadButton, &autoButton, &setupButton })
        {
            b->setBounds (h.removeFromRight (b == &autoButton || b == &setupButton ? 120 : 70));
            h.removeFromRight (6);
        }
    }

    r.removeFromTop (16);
    r = r.reduced (4, 0).withTrimmedBottom (4);

    masterArea = r.removeFromRight (busWidth * kNumBuses);
    for (int b = 0; b < busComps.size(); ++b)
        busComps[b]->setBounds (masterArea.withWidth (busWidth).translated (b * busWidth, 0));
    r.removeFromRight (8);

    fxArea = r.removeFromRight (fxWidth * kNumFx);
    for (int f = 0; f < fxComps.size(); ++f)
        fxComps[f]->setBounds (fxArea.withWidth (fxWidth).translated (f * fxWidth, 0));
    r.removeFromRight (8);

    stripViewport.setBounds (r);
    const int contentW = juce::jmax (r.getWidth(), stripComps.size() * stripWidth);
    const bool scroll = contentW > r.getWidth();
    const int contentH = r.getHeight() - (scroll ? stripViewport.getScrollBarThickness() : 0);
    stripHolder.setSize (contentW, contentH);
    for (int i = 0; i < stripComps.size(); ++i)
        stripComps[i]->setBounds (i * stripWidth, 0, stripWidth, contentH);

    if (detail != nullptr)
        detail->setBounds (getLocalBounds().withTrimmedTop (headerHeight));
}

void MainComponent::openSettings()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new SettingsComponent (engine));
    o.dialogTitle = "Audio Setup";
    o.dialogBackgroundColour = theme::bg;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::loadPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Load mixer preset",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.bvm");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (fc.getResult().existsAsFile())
                                  engine.loadPreset (fc.getResult());
                          });
}

void MainComponent::savePreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Save mixer preset",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile ("My Mix.bvm"), "*.bvm");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f != juce::File())
                                  engine.savePreset (f.withFileExtension ("bvm"));
                          });
}
