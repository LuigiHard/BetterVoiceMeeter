#pragma once
#include <JuceHeader.h>

// Dark, Voicemeeter-inspired palette.
namespace theme
{
const juce::Colour bg        { 0xff1b1e24 };
const juce::Colour panel     { 0xff262a32 };
const juce::Colour panelHi   { 0xff30353f };
const juce::Colour border    { 0xff3b414c };
const juce::Colour text      { 0xffd6dbe3 };
const juce::Colour dim       { 0xff858e9d };
const juce::Colour hw        { 0xff5ab4ff };   // hardware inputs
const juce::Colour virt      { 0xffffb04a };   // virtual inputs
const juce::Colour fx        { 0xffc58cff };   // FX returns
const juce::Colour busA      { 0xff3fd49a };   // A (hardware) buses
const juce::Colour busB      { 0xffffc94a };   // B (virtual) buses
const juce::Colour mute      { 0xffff5a5a };
const juce::Colour solo      { 0xffffe14d };
const juce::Colour meterLow  { 0xff2ed47a };
const juce::Colour meterMid  { 0xffe8d84a };
const juce::Colour meterHigh { 0xffff4d4d };

inline juce::Font font (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}
} // namespace theme

class BvmLookAndFeel : public juce::LookAndFeel_V4
{
public:
    BvmLookAndFeel()
    {
        setColourScheme ({ theme::bg, theme::panel, theme::panelHi, theme::border, theme::text,
                           theme::hw, juce::Colours::black, theme::panelHi, theme::text });
        setColour (juce::TextButton::buttonColourId, theme::panelHi);
        setColour (juce::TextButton::textColourOffId, theme::dim);
        setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        setColour (juce::Label::textColourId, theme::text);
        setColour (juce::Slider::textBoxTextColourId, theme::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::backgroundColourId, theme::panelHi);
        setColour (juce::ComboBox::outlineColourId, theme::border);
        setColour (juce::PopupMenu::backgroundColourId, theme::panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::hw.withAlpha (0.35f));
        setColour (juce::ResizableWindow::backgroundColourId, theme::bg);
        setColour (juce::TooltipWindow::backgroundColourId, theme::panelHi);
        setColour (juce::TooltipWindow::textColourId, theme::text);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : base;
        if (down) c = c.darker (0.2f);
        else if (over) c = c.brighter (0.12f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (b.getToggleState() ? c.brighter (0.4f) : theme::border);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton& b, int h) override
    {
        // Shrink short labels to fit narrow buttons instead of truncating them.
        const float byWidth = (float) (b.getWidth() - 6) / juce::jmax (1, b.getButtonText().length()) * 1.25f;
        return theme::font (juce::jmax (8.5f, juce::jmin (13.0f, h * 0.55f, byWidth)), true);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                           juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = start + pos * (end - start);
        const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);

        g.setColour (theme::bg);
        g.fillEllipse (juce::Rectangle<float> (radius * 2, radius * 2).withCentre (centre));

        juce::Path track, value;
        track.addCentredArc (centre.x, centre.y, radius - 2, radius - 2, 0, start, end, true);
        g.setColour (theme::border);
        g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (pos > 0.001f)
        {
            value.addCentredArc (centre.x, centre.y, radius - 2, radius - 2, 0, start, angle, true);
            g.setColour (accent);
            g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (theme::panelHi);
        g.fillEllipse (juce::Rectangle<float> ((radius - 5) * 2, (radius - 5) * 2).withCentre (centre));
        juce::Path pointer;
        pointer.addRoundedRectangle (-1.5f, -(radius - 5), 3.0f, radius * 0.5f, 1.5f);
        g.setColour (theme::text);
        g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            if (style == juce::Slider::LinearBar) // V4 draws only the filled part; give the bar a visible track
            {
                g.setColour (s.findColour (juce::Slider::backgroundColourId));
                g.fillRoundedRectangle ((float) x, (float) y, (float) w, (float) h, 3.0f);
            }
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }
        // Voicemeeter-style fader: thin slot, wide flat cap.
        const auto accent = s.findColour (juce::Slider::thumbColourId);
        const float cx = x + w * 0.5f;
        g.setColour (theme::bg);
        g.fillRoundedRectangle (cx - 2.5f, (float) y, 5.0f, (float) h, 2.5f);

        // 0 dB marker
        const double zeroPos = s.valueToProportionOfLength (0.0);
        const float zy = (float) (y + h - zeroPos * h);
        g.setColour (theme::dim.withAlpha (0.6f));
        g.drawHorizontalLine ((int) zy, (float) x + 2, (float) (x + w - 2));

        const float capH = 22.0f, capW = (float) w - 4.0f;
        auto cap = juce::Rectangle<float> (capW, capH).withCentre ({ cx, pos });
        g.setColour (theme::panelHi.brighter (0.15f));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (accent);
        g.fillRect (cap.withSizeKeepingCentre (capW - 6, 2.0f));
        g.setColour (theme::border.brighter (0.3f));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
    }

    int getSliderThumbRadius (juce::Slider& s) override
    {
        return s.getSliderStyle() == juce::Slider::LinearVertical ? 11 : LookAndFeel_V4::getSliderThumbRadius (s);
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (theme::font (11.0f));
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        return l;
    }
};
