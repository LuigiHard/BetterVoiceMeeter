#include "AppMixer.h"
#include "Theme.h"

namespace
{
constexpr int stripWidth = 132;

juce::Colour colourFor (const juce::String& key, int index)
{
    // Distinct accent per strip from the mixer's palette (by position, so neighbours never match).
    const juce::Colour palette[] = { theme::hw, juce::Colour (0xffff5c8a), theme::busA, theme::virt,
                                            juce::Colour (0xffa78bfa), theme::busB };
    if (key == "system") return theme::dim;
    return palette[(size_t) index % std::size (palette)];
}
} // namespace

class AppMixer::Strip : public juce::Component
{
public:
    Strip (AppSessions& s, const AppSessions::App& a, int index) : sessions (s), key (a.key), accent (colourFor (a.key, index))
    {
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange (0.0, 100.0, 1.0);
        fader.setColour (juce::Slider::thumbColourId, accent);
        fader.setDoubleClickReturnValue (true, 100.0);
        fader.onValueChange = [this] { sessions.setVolume (key, (float) fader.getValue() / 100.0f); repaint(); };
        addAndMakeVisible (fader);

        mute.setClickingTogglesState (true);
        mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        mute.onClick = [this] { sessions.setMute (key, mute.getToggleState()); repaint(); };
        addAndMakeVisible (mute);
        update (a);
    }

    const juce::String key;

    void update (const AppSessions::App& a)
    {
        name = a.name;
        devices = a.devices;
        icon = a.icon;
        active = a.active;
        if (! fader.isMouseButtonDown() && std::abs (fader.getValue() - a.volume * 100.0) > 0.6)
            fader.setValue (a.volume * 100.0, juce::dontSendNotification);
        if (mute.getToggleState() != a.mute)
            mute.setToggleState (a.mute, juce::dontSendNotification);
        // Meter ballistics: instant rise, smooth fall.
        const float db = a.peak > 1.0e-5f ? 20.0f * std::log10 (a.peak) : -70.0f;
        level = db > level ? db : juce::jmax (-70.0f, level - 1.4f);
        if (db >= hold) { hold = db; holdTimer = 40; }
        else if (--holdTimer <= 0) hold = juce::jmax (-70.0f, hold - 0.8f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (theme::border);
        g.drawRoundedRectangle (r, 5.0f, 1.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (r.removeFromTop (3.0f).reduced (6.0f, 0), 1.5f);

        // Icon + name
        auto iconArea = iconBox.toFloat();
        if (icon.isValid())
            g.drawImage (icon, iconArea, juce::RectanglePlacement::centred);
        else
        {
            g.setColour (accent);
            g.fillRoundedRectangle (iconArea, 6.0f);
            g.setColour (theme::bg);
            g.setFont (theme::font (13.0f, true));
            g.drawText (name.substring (0, 2), iconBox, juce::Justification::centred);
        }
        g.setColour (active ? theme::text : theme::dim);
        g.setFont (theme::font (12.5f, true));
        g.drawFittedText (name, nameBox, juce::Justification::centredLeft, 2, 0.8f);
        g.setColour (theme::dim);
        g.setFont (theme::font (10.0f));
        const int numDevices = devices.isEmpty() ? 0 : juce::StringArray::fromTokens (devices, ",", "").size();
        g.drawFittedText (numDevices == 0 ? juce::String() : numDevices > 2 ? "on " + juce::String (numDevices) + " devices" : "on " + devices,
                          deviceBox, juce::Justification::centred, 2, 0.8f);

        // Meter
        auto m = meterBox.toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (m, 2.0f);
        auto pos = [] (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
        if (! mute.getToggleState() && pos (level) > 0)
        {
            juce::ColourGradient grad (theme::meterLow, 0, m.getBottom(), theme::meterHigh, 0, m.getY(), false);
            grad.addColour (pos (-12.0f), theme::meterLow);
            grad.addColour (pos (-3.0f), theme::meterMid);
            g.setGradientFill (grad);
            g.fillRect (m.withTop (m.getBottom() - m.getHeight() * pos (level)).reduced (1.0f, 0));
        }
        if (hold > -59.0f)
        {
            g.setColour (theme::text);
            g.fillRect (m.getX() + 1, m.getBottom() - m.getHeight() * pos (hold), m.getWidth() - 2, 1.5f);
        }

        g.setColour (theme::text);
        g.setFont (theme::font (12.0f, true));
        g.drawText (juce::String ((int) fader.getValue()) + "%", valueBox, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (9, 8);
        r.removeFromTop (4);
        auto head = r.removeFromTop (34);
        iconBox = head.removeFromLeft (28).withSizeKeepingCentre (26, 26);
        head.removeFromLeft (6);
        nameBox = head;
        deviceBox = r.removeFromTop (26);
        mute.setBounds (r.removeFromBottom (26).reduced (2, 0));
        r.removeFromBottom (4);
        valueBox = r.removeFromBottom (18);
        r.removeFromTop (4);
        meterBox = r.removeFromRight (12).reduced (0, 11);
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    AppSessions& sessions;
    juce::Colour accent;
    juce::Slider fader;
    juce::TextButton mute { "MUTE" };
    juce::String name, devices;
    juce::Image icon;
    bool active = false;
    float level = -70.0f, hold = -70.0f;
    int holdTimer = 0;
    juce::Rectangle<int> iconBox, nameBox, deviceBox, meterBox, valueBox;
};

AppMixer::AppMixer (std::function<void()> closeFn) : onClose (std::move (closeFn))
{
    setWantsKeyboardFocus (true);
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeBtn);
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
}

AppMixer::~AppMixer()
{
    stopTimer();
    sessions.stop();
}

void AppMixer::visibilityChanged()
{
    if (isVisible())
    {
        sessions.start();
        startTimerHz (25);
        grabKeyboardFocus();
    }
    else
    {
        stopTimer();
        sessions.stop();
    }
}

bool AppMixer::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    return false;
}

void AppMixer::timerCallback()
{
    const auto apps = sessions.snapshot();
    bool sameSet = (int) apps.size() == strips.size();
    for (int i = 0; sameSet && i < strips.size(); ++i)
        sameSet = strips[i]->key == apps[(size_t) i].key;

    if (! sameSet)
    {
        strips.clear();
        for (auto& a : apps)
            holder.addAndMakeVisible (strips.add (new Strip (sessions, a, strips.size())));
        resized();
        repaint();
    }
    else
        for (int i = 0; i < strips.size(); ++i)
            strips[i]->update (apps[(size_t) i]);
}

void AppMixer::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::panel);
    g.fillRect (topBar);
    g.setColour (theme::border);
    g.drawHorizontalLine (topBar.getBottom(), 0.0f, (float) getWidth());

    auto t = topBar.reduced (14, 0);
    g.setFont (theme::font (17.0f, true));
    g.setColour (theme::text);
    g.drawText ("APP", t, juce::Justification::centredLeft);
    g.setColour (theme::hw);
    g.drawText ("MIXER", t.withTrimmedLeft (40), juce::Justification::centredLeft);
    g.setColour (theme::dim);
    g.setFont (theme::font (12.5f));
    g.drawText ("Volume per app on this PC. Same controls as the Windows volume mixer; changes show up there too.",
                t.withTrimmedLeft (130).withTrimmedRight (50), juce::Justification::centredLeft, true);

    if (strips.isEmpty())
    {
        g.setColour (theme::dim);
        g.setFont (theme::font (14.0f));
        g.drawText ("No apps are using audio right now. Play something and it appears here.",
                    getLocalBounds().withTrimmedTop (topBar.getBottom()), juce::Justification::centred);
    }
}

void AppMixer::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop (48);
    closeBtn.setBounds (topBar.reduced (10, 9).removeFromRight (34));
    r.reduce (10, 10);
    viewport.setBounds (r);
    const int h = r.getHeight() - (strips.size() * stripWidth > r.getWidth() ? viewport.getScrollBarThickness() : 0);
    holder.setSize (juce::jmax (r.getWidth(), strips.size() * stripWidth), h);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripWidth, 0, stripWidth, juce::jmin (h, 560));
}
