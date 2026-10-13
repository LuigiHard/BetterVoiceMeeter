#pragma once
#include <JuceHeader.h>

// App themes. "Classic" is the original Voicemeeter-inspired look; "Focusrite" follows
// Focusrite Control 2: near-black, grey cards, red/blue channel underlines, silver faders,
// solid green meters. The colours are variables so the theme can change at runtime
// (the window is rebuilt after switching).
namespace theme
{
enum class Style { classic, focusrite };
inline Style style = Style::classic;

inline juce::Colour bg, panel, panelHi, border, text, dim,
    hw,        // hardware inputs
    virt,      // virtual inputs / playback
    fx,        // FX returns
    busA,      // A (hardware) buses
    busB,      // B (virtual) buses
    mute, solo, meterLow, meterMid, meterHigh,
    cardBorder; // strip / card outline

inline bool focusrite() noexcept { return style == Style::focusrite; }

inline void apply (Style s)
{
    style = s;
    if (s == Style::focusrite)
    {
        bg = juce::Colour (0xff121212);  panel = juce::Colour (0xff1f1f1f);  panelHi = juce::Colour (0xff2c2c2c);
        border = juce::Colour (0xff3a3a3a);  text = juce::Colour (0xffeaeaea);  dim = juce::Colour (0xff8f8f8f);
        hw = juce::Colour (0xffe1262f);    // Focusrite red: input channels
        virt = juce::Colour (0xff3b82f6);  // blue: playback / virtual
        fx = juce::Colour (0xff9f7aea);
        busA = juce::Colour (0xff1fa463);  // green toggles (+48V / Inst)
        busB = juce::Colour (0xffc9a227);  // gold toggle (Air)
        mute = juce::Colour (0xffe1262f);  solo = juce::Colour (0xffe8c547);
        meterLow = juce::Colour (0xff1fc46a);  meterMid = juce::Colour (0xffe8c547);  meterHigh = juce::Colour (0xffe1262f);
        cardBorder = juce::Colour (0xffd4232c);  // Focusrite red outline
    }
    else
    {
        bg = juce::Colour (0xff1b1e24);  panel = juce::Colour (0xff262a32);  panelHi = juce::Colour (0xff30353f);
        border = juce::Colour (0xff3b414c);  text = juce::Colour (0xffd6dbe3);  dim = juce::Colour (0xff858e9d);
        hw = juce::Colour (0xff5ab4ff);  virt = juce::Colour (0xffffb04a);  fx = juce::Colour (0xffc58cff);
        busA = juce::Colour (0xff3fd49a);  busB = juce::Colour (0xffffc94a);
        mute = juce::Colour (0xffff5a5a);  solo = juce::Colour (0xffffe14d);
        meterLow = juce::Colour (0xff2ed47a);  meterMid = juce::Colour (0xffe8d84a);  meterHigh = juce::Colour (0xffff4d4d);
        cardBorder = border;
    }
}

inline Style styleFromName (const juce::String& n) { return n == "focusrite" ? Style::focusrite : Style::classic; }
inline juce::String nameOf (Style s) { return s == Style::focusrite ? "focusrite" : "classic"; }

// Channel titles: coloured in Classic; white with a coloured underline in Focusrite.
inline juce::Colour titleColour (juce::Colour accent) { return focusrite() ? text : accent; }

// Initialise the palette before any component reads it.
inline const bool initialised = (apply (Style::classic), true);

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
        const float radius = theme::focusrite() ? 4.0f : 3.0f;
        g.setColour (c);
        g.fillRoundedRectangle (r, radius);
        g.setColour (b.getToggleState() ? c.brighter (theme::focusrite() ? 0.25f : 0.4f) : theme::border);
        g.drawRoundedRectangle (r, radius, 1.0f);
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
        const float cx = x + w * 0.5f;
        if (theme::focusrite())
        {
            // Focusrite Control style: thin light rail, unity tick, silver metal cap.
            g.setColour (juce::Colour (0xffd0d0d0).withAlpha (0.85f));
            g.fillRoundedRectangle (cx - 1.0f, (float) y + 4, 2.0f, (float) h - 8, 1.0f);
            const float zy = (float) (y + h - s.valueToProportionOfLength (0.0) * h);
            g.setColour (theme::dim);
            g.fillRect (cx - 12.0f, zy, 7.0f, 1.0f);
            g.fillRect (cx + 5.0f, zy, 7.0f, 1.0f);
            const auto cap = juce::Rectangle<float> (juce::jmin (22.0f, (float) w - 2.0f), 28.0f).withCentre ({ cx, pos });
            juce::ColourGradient metal (juce::Colour (0xfff4f4f4), cap.getX(), cap.getY(), juce::Colour (0xff8e8e8e), cap.getX(), cap.getBottom(), false);
            metal.addColour (0.48, juce::Colour (0xffd6d6d6));
            metal.addColour (0.52, juce::Colour (0xffb0b0b0));
            g.setGradientFill (metal);
            g.fillRoundedRectangle (cap, 3.0f);
            g.setColour (juce::Colour (0xff4a4a4a));
            g.drawRoundedRectangle (cap, 3.0f, 1.0f);
            g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 6.0f, 1.5f));
            return;
        }
        // Voicemeeter-style fader: thin slot, wide flat cap.
        const auto accent = s.findColour (juce::Slider::thumbColourId);
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
        return s.getSliderStyle() == juce::Slider::LinearVertical ? (theme::focusrite() ? 14 : 11) : LookAndFeel_V4::getSliderThumbRadius (s);
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
