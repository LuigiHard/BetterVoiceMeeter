#pragma once
#include <JuceHeader.h>
#include <optional>
#include "../Params.h"
#include "../DSP.h"
#include "Theme.h"

// Slider bound to a Param. Optional transforms map slider value <-> param value
// (e.g. a "Comp" knob turning clockwise lowers the threshold). Polls the param so it
// stays in sync when the same param is edited elsewhere (detail panel, presets).
class ParamSlider : public juce::Slider, private juce::Timer
{
public:
    using Fn = std::function<float (float)>;

    ParamSlider (Param& p, SliderStyle style, juce::Colour accent, Fn toParamFn = {}, Fn fromParamFn = {},
                 std::optional<juce::NormalisableRange<double>> sliderRange = {})
        : param (p), toParam (std::move (toParamFn)), fromParam (std::move (fromParamFn))
    {
        setSliderStyle (style);
        setTextBoxStyle (NoTextBox, false, 0, 0);
        setColour (rotarySliderFillColourId, accent);
        setColour (thumbColourId, accent);
        setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);

        if (sliderRange.has_value())
            setNormalisableRange (*sliderRange);
        else
            setNormalisableRange ({ p.range.start, p.range.end,
                                    [r = p.range] (double, double, double v) { return (double) r.convertFrom0to1 ((float) v); },
                                    [r = p.range] (double, double, double v) { return (double) r.convertTo0to1 ((float) v); } });

        setDoubleClickReturnValue (true, currentFromParam (p.defaultValue));
        setValue (currentFromParam (p.get()), juce::dontSendNotification);
        onValueChange = [this] { param.set (toParam ? toParam ((float) getValue()) : (float) getValue()); };
        startTimerHz (12);
    }

    std::function<juce::String (float paramValue)> formatter;

    juce::String getTextFromValue (double v) override
    {
        const float pv = toParam ? toParam ((float) v) : (float) v;
        return formatter ? formatter (pv) : juce::String (pv, 1);
    }

    Param& param;

private:
    double currentFromParam (float v) const { return fromParam ? fromParam (v) : v; }

    void timerCallback() override
    {
        if (isMouseButtonDown()) return;
        const double v = currentFromParam (param.get());
        if (std::abs (v - getValue()) > 1.0e-4)
            setValue (v, juce::dontSendNotification);
    }

    Fn toParam, fromParam;
};

// Toggle button bound to a 0/1 Param.
class ParamToggle : public juce::TextButton, private juce::Timer
{
public:
    ParamToggle (Param& p, const juce::String& text, juce::Colour onColour) : juce::TextButton (text), param (p)
    {
        setClickingTogglesState (true);
        setColour (buttonOnColourId, onColour);
        setToggleState (p.on(), juce::dontSendNotification);
        onClick = [this] { param.set (getToggleState() ? 1.0f : 0.0f); };
        startTimerHz (12);
    }

    Param& param;

private:
    void timerCallback() override
    {
        if (getToggleState() != param.on())
            setToggleState (param.on(), juce::dontSendNotification);
    }
};

// Knob with a caption and value readout underneath.
class Knob : public juce::Component
{
public:
    Knob (Param& p, const juce::String& caption, juce::Colour accent,
          ParamSlider::Fn toParam = {}, ParamSlider::Fn fromParam = {},
          std::optional<juce::NormalisableRange<double>> range = {})
        : slider (p, juce::Slider::RotaryHorizontalVerticalDrag, accent, std::move (toParam), std::move (fromParam), range),
          name (caption)
    {
        addAndMakeVisible (slider);
        slider.setPopupDisplayEnabled (true, true, nullptr);
        slider.onValueChange = [this, prev = slider.onValueChange] { prev(); repaint(); };
    }

    void setFormatter (std::function<juce::String (float)> f) { slider.formatter = std::move (f); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto bottom = r.removeFromBottom (24);
        g.setColour (theme::dim);
        g.setFont (theme::font (10.5f, true));
        g.drawText (name, bottom.removeFromTop (12), juce::Justification::centred);
        g.setColour (theme::text);
        g.setFont (theme::font (10.5f));
        g.drawText (slider.getTextFromValue (slider.getValue()), bottom, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromBottom (24);
        const int s = juce::jmin (r.getWidth(), r.getHeight());
        slider.setBounds (r.withSizeKeepingCentre (s, s));
    }

    ParamSlider slider;

private:
    juce::String name;
};

// Stereo peak meter with ballistics and peak hold.
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    explicit LevelMeter (Meter& m) : meter (m) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 2.0f);
        const float w = (r.getWidth() - 3.0f) / 2.0f;
        drawBar (g, { r.getX() + 1, r.getY() + 1, w, r.getHeight() - 2 }, levels[0], holds[0]);
        drawBar (g, { r.getX() + 2 + w, r.getY() + 1, w, r.getHeight() - 2 }, levels[1], holds[1]);
    }

    static float dbToPos (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 72.0f); } // -60..+12

private:
    void drawBar (juce::Graphics& g, juce::Rectangle<float> r, float db, float hold)
    {
        const float pos = dbToPos (db);
        if (pos > 0)
        {
            auto bar = r.withTop (r.getBottom() - r.getHeight() * pos);
            juce::ColourGradient grad (theme::meterLow, 0, r.getBottom(), theme::meterHigh, 0, r.getY(), false);
            grad.addColour (dbToPos (-12.0f), theme::meterLow);
            grad.addColour (dbToPos (-3.0f), theme::meterMid);
            grad.addColour (dbToPos (0.0f), theme::meterHigh);
            g.setGradientFill (grad);
            g.fillRect (bar);
        }
        if (hold > -59.0f)
        {
            g.setColour (hold > 0.0f ? theme::meterHigh : theme::text);
            g.fillRect (r.getX(), r.getBottom() - r.getHeight() * dbToPos (hold), r.getWidth(), 1.5f);
        }
    }

    void timerCallback() override
    {
        auto [l, r] = meter.take();
        const float in[2] = { bvm::gainToDb (l), bvm::gainToDb (r) };
        for (int i = 0; i < 2; ++i)
        {
            levels[i] = in[i] > levels[i] ? in[i] : juce::jmax (-70.0f, levels[i] - 1.6f);
            if (in[i] >= holds[i]) { holds[i] = in[i]; holdTimer[i] = 45; }
            else if (--holdTimer[i] <= 0) holds[i] = juce::jmax (-70.0f, holds[i] - 1.0f);
        }
        repaint();
    }

    Meter& meter;
    float levels[2] { -70, -70 }, holds[2] { -70, -70 };
    int holdTimer[2] { 0, 0 };
};

// Vertical fader with a meter next to it and dB scale + readout.
class FaderMeter : public juce::Component
{
public:
    FaderMeter (Param& p, Meter& m, juce::Colour accent)
        : fader (p, juce::Slider::LinearVertical, accent), meter (m)
    {
        addAndMakeVisible (fader);
        addAndMakeVisible (meter);
        fader.formatter = [] (float v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
        fader.setPopupDisplayEnabled (true, false, nullptr);
        fader.setVelocityBasedMode (false);
        fader.setMouseDragSensitivity (300);
        fader.onValueChange = [this, prev = fader.onValueChange] { prev(); repaint(); };
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto readout = r.removeFromBottom (16);
        g.setFont (theme::font (11.0f, true));
        g.setColour (theme::text);
        const float v = fader.param.get();
        g.drawText ((v > 0 ? "+" : "") + juce::String (v, 1), readout, juce::Justification::centred);

        // dB scale
        g.setFont (theme::font (9.0f));
        g.setColour (theme::dim);
        for (float db : { 12.0f, 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -60.0f })
        {
            const float y = scaleArea.getBottom() - 11.0f - (float) fader.valueToProportionOfLength (db) * (scaleArea.getHeight() - 22.0f);
            g.drawText (juce::String ((int) db), scaleArea.getX(), (int) y - 5, scaleArea.getWidth(), 10,
                        juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromBottom (18);
        meter.setBounds (r.removeFromRight (14).reduced (0, 11));
        r.removeFromRight (3);
        scaleArea = r.removeFromLeft (20);
        fader.setBounds (r);
    }

    ParamSlider fader;
    LevelMeter meter;

private:
    juce::Rectangle<int> scaleArea;
};

// XY pad: X = pan, Y = tone (warm <-> bright). Mirrors Voicemeeter's colour panel.
class XYPad : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    XYPad (Param& x, Param& y, juce::Colour c) : px (x), py (y), accent (c) { startTimerHz (15); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        juce::ColourGradient grad (accent.withAlpha (0.10f), r.getCentreX(), r.getY(),
                                   theme::bg, r.getCentreX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
        g.setColour (theme::border.withAlpha (0.6f));
        g.drawVerticalLine ((int) r.getCentreX(), r.getY() + 2, r.getBottom() - 2);
        g.drawHorizontalLine ((int) r.getCentreY(), r.getX() + 2, r.getRight() - 2);

        g.setFont (theme::font (8.5f, true));
        g.setColour (theme::dim);
        g.drawText ("BRIGHT", r.reduced (3), juce::Justification::centredTop);
        g.drawText ("WARM", r.reduced (3), juce::Justification::centredBottom);
        g.drawText ("L", r.reduced (4), juce::Justification::centredLeft);
        g.drawText ("R", r.reduced (4), juce::Justification::centredRight);

        const auto pt = toPoint();
        g.setColour (accent.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (16, 16).withCentre (pt));
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre (pt));
    }

    void mouseDown (const juce::MouseEvent& e) override { setFrom (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { setFrom (e.position); }
    void mouseDoubleClick (const juce::MouseEvent&) override { px.reset(); py.reset(); repaint(); }

private:
    juce::Point<float> toPoint() const
    {
        auto r = getLocalBounds().toFloat().reduced (8.0f);
        return { r.getX() + (px.get() + 1.0f) * 0.5f * r.getWidth(), r.getY() + (1.0f - (py.get() + 1.0f) * 0.5f) * r.getHeight() };
    }

    void setFrom (juce::Point<float> p)
    {
        auto r = getLocalBounds().toFloat().reduced (8.0f);
        auto x = juce::jlimit (-1.0f, 1.0f, (p.x - r.getX()) / r.getWidth() * 2.0f - 1.0f);
        auto y = juce::jlimit (-1.0f, 1.0f, (1.0f - (p.y - r.getY()) / r.getHeight()) * 2.0f - 1.0f);
        if (std::abs (x) < 0.06f) x = 0; // detents at centre
        if (std::abs (y) < 0.06f) y = 0;
        px.set (x);
        py.set (y);
        repaint();
    }

    void timerCallback() override
    {
        if (px.get() != lastX || py.get() != lastY) { lastX = px.get(); lastY = py.get(); repaint(); }
    }

    Param& px;
    Param& py;
    juce::Colour accent;
    float lastX = 9, lastY = 9;
};

// Small horizontal gain-reduction / gate indicator.
class DynamicsLed : public juce::Component, private juce::Timer
{
public:
    DynamicsLed (Meter& m, bool gateMode) : meter (m), gate (gateMode) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (r, 2.0f);
        const float amount = gate ? 1.0f - value : juce::jlimit (0.0f, 1.0f, -value / 20.0f);
        if (amount > 0.01f)
        {
            g.setColour (gate ? theme::virt : theme::mute.withAlpha (0.9f));
            g.fillRoundedRectangle (r.withWidth (r.getWidth() * amount), 2.0f);
        }
    }

private:
    void timerCallback() override
    {
        const float v = gate ? meter.gateGain.load() : meter.gainReduction.load();
        value = gate ? v : juce::jmin (v, value * 0.85f);
        repaint();
    }

    Meter& meter;
    bool gate;
    float value = 0;
};
