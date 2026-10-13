#include "AppMixer.h"
#include "Theme.h"
#include "Widgets.h"
#include "../Engine.h"

namespace
{
constexpr int stripWidth = 132, groupWidth = 210;

juce::Colour colourFor (const juce::String& key, int index)
{
    // Distinct accent per strip from the mixer's palette (by position, so neighbours never match).
    const juce::Colour palette[] = { theme::hw, juce::Colour (0xffff5c8a), theme::busA, theme::virt,
                                     juce::Colour (0xffa78bfa), theme::busB };
    if (key == "system") return theme::dim;
    return palette[(size_t) index % std::size (palette)];
}

juce::Colour groupColour (int g)
{
    const juce::Colour c[] = { theme::hw, juce::Colour (0xffa78bfa), juce::Colour (0xffff5c8a), theme::busB };
    return c[(size_t) juce::jlimit (0, 3, g)];
}

void drawCard (juce::Graphics& g, juce::Rectangle<int> b, juce::Colour accent)
{
    auto r = b.toFloat().reduced (2.0f);
    g.setColour (theme::panel);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (theme::focusrite() ? theme::cardBorder : theme::border);
    g.drawRoundedRectangle (r, 5.0f, theme::focusrite() ? 1.5f : 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (r.removeFromTop (3.0f).reduced (6.0f, 0), 1.5f);
}

void drawIcon (juce::Graphics& g, const juce::Image& icon, const juce::String& name, juce::Rectangle<int> box, juce::Colour accent)
{
    if (icon.isValid())
    {
        g.drawImage (icon, box.toFloat(), juce::RectanglePlacement::centred);
        return;
    }
    g.setColour (accent);
    g.fillRoundedRectangle (box.toFloat(), 6.0f);
    g.setColour (theme::bg);
    g.setFont (theme::font (12.0f, true));
    g.drawText (name.substring (0, 2), box, juce::Justification::centred);
}

// Vertical dB fader (not tied to a Param) styled like the mixer's faders.
struct DbFader : juce::Slider
{
    DbFader (juce::Colour accent, float initialDb, std::function<void (float)> onChange)
    {
        setSliderStyle (juce::Slider::LinearVertical);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setNormalisableRange ({ -60.0, 12.0, 0.1, 1.0 });
        setSkewFactorFromMidPoint (-12.0);
        setColour (juce::Slider::thumbColourId, accent);
        setValue (initialDb, juce::dontSendNotification);
        setDoubleClickReturnValue (true, 0.0);
        setPopupDisplayEnabled (true, false, nullptr);
        onValueChange = [this, onChange] { onChange ((float) getValue()); };
    }
    juce::String getTextFromValue (double v) override
    {
        return v <= -59.9 ? juce::String ("-inf") : (v > 0 ? "+" : "") + juce::String (v, 1) + " dB";
    }
};
} // namespace

//==============================================================================
// VOLUME mode strip: Windows per-app volume.
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
        name = a.name; devices = a.devices; icon = a.icon; active = a.active;
        if (! fader.isMouseButtonDown() && std::abs (fader.getValue() - a.volume * 100.0) > 0.6)
            fader.setValue (a.volume * 100.0, juce::dontSendNotification);
        if (mute.getToggleState() != a.mute)
            mute.setToggleState (a.mute, juce::dontSendNotification);
        const float db = a.peak > 1.0e-5f ? 20.0f * std::log10 (a.peak) : -70.0f;
        level = db > level ? db : juce::jmax (-70.0f, level - 1.4f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds(), accent);
        drawIcon (g, icon, name, iconBox, accent);
        g.setColour (active ? theme::text : theme::dim);
        g.setFont (theme::font (12.5f, true));
        g.drawFittedText (name, nameBox, juce::Justification::centredLeft, 2, 0.8f);
        g.setColour (theme::dim);
        g.setFont (theme::font (10.0f));
        const int numDevices = devices.isEmpty() ? 0 : juce::StringArray::fromTokens (devices, ",", "").size();
        g.drawFittedText (numDevices == 0 ? juce::String() : numDevices > 2 ? "on " + juce::String (numDevices) + " devices" : "on " + devices,
                          deviceBox, juce::Justification::centred, 2, 0.8f);
        auto m = meterBox.toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (m, 2.0f);
        const float pos = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
        if (! mute.getToggleState() && pos > 0)
        {
            juce::ColourGradient grad (theme::meterLow, 0, m.getBottom(), theme::meterHigh, 0, m.getY(), false);
            grad.addColour (0.8, theme::meterLow);
            grad.addColour (0.95, theme::meterMid);
            g.setGradientFill (grad);
            g.fillRect (m.withTop (m.getBottom() - m.getHeight() * pos).reduced (1.0f, 0));
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
    float level = -70.0f;
    juce::Rectangle<int> iconBox, nameBox, deviceBox, meterBox, valueBox;
};

//==============================================================================
// GROUPS mode: one card per group with "you" and "stream" faders.
class AppMixer::GroupCard : public juce::Component
{
public:
    GroupCard (AppRouter& r, int index) : router (r), g (index), accent (groupColour (index)),
        you (accent, r.group (index).youDb.load(), [this] (float v) { router.group (g).youDb = v; repaint(); }),
        stream (accent, r.group (index).streamDb.load(), [this] (float v) { router.group (g).streamDb = v; repaint(); }),
        meter (r.group (index).meter)
    {
        mute.setClickingTogglesState (true);
        mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        mute.setToggleState (r.group (index).mute.load(), juce::dontSendNotification);
        mute.onClick = [this] { router.group (g).mute = mute.getToggleState(); };
        for (juce::Component* c : { (juce::Component*) &you, (juce::Component*) &stream, (juce::Component*) &meter, (juce::Component*) &mute })
            addAndMakeVisible (c);
        you.setTooltip ("Level in your headphones / speakers (A1)");
        stream.setTooltip ("Level on stream / Discord (B1)");
    }

    void setMembers (const juce::StringArray& names) { members = names; repaint(); }

    void paint (juce::Graphics& gr) override
    {
        drawCard (gr, getLocalBounds(), accent);
        gr.setColour (theme::text);
        gr.setFont (theme::font (15.0f, true));
        gr.drawText (AppRouter::groupName (g), titleBox, juce::Justification::centredLeft);
        gr.setColour (theme::dim);
        gr.setFont (theme::font (11.0f));
        gr.drawFittedText (members.isEmpty() ? juce::String ("No apps") : members.joinIntoString (", "), membersBox,
                           juce::Justification::topLeft, 3, 0.85f);
        gr.setFont (theme::font (10.5f, true));
        gr.drawText ("YOU (A1)", youLabel, juce::Justification::centred);
        gr.drawText ("STREAM (B1)", streamLabel, juce::Justification::centred);
        gr.setColour (theme::text);
        gr.setFont (theme::font (11.5f, true));
        gr.drawText (you.getTextFromValue (you.getValue()), youValue, juce::Justification::centred);
        gr.drawText (stream.getTextFromValue (stream.getValue()), streamValue, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        r.removeFromTop (4);
        titleBox = r.removeFromTop (22);
        membersBox = r.removeFromTop (44);
        mute.setBounds (r.removeFromBottom (26).reduced (2, 0));
        r.removeFromBottom (4);
        auto values = r.removeFromBottom (18);
        auto labels = r.removeFromTop (16);
        const int colW = (r.getWidth() - 16) / 2;
        youLabel = labels.removeFromLeft (colW);
        streamLabel = labels.withTrimmedLeft (16);
        youValue = values.removeFromLeft (colW);
        streamValue = values.withTrimmedLeft (16);
        you.setBounds (r.removeFromLeft (colW));
        meter.setBounds (r.removeFromLeft (16).reduced (2, 12));
        stream.setBounds (r);
    }

private:
    AppRouter& router;
    int g;
    juce::Colour accent;
    DbFader you, stream;
    LevelMeter meter;
    juce::TextButton mute { "MUTE" };
    juce::StringArray members;
    juce::Rectangle<int> titleBox, membersBox, youLabel, streamLabel, youValue, streamValue;
};

// GROUPS mode: app -> group assignment row.
class AppMixer::AssignRow : public juce::Component
{
public:
    AssignRow (AppRouter& r, const AppRouter::Status& s) : router (r), key (s.key)
    {
        for (int g = 0; g < AppRouter::numGroups; ++g)
            box.addItem (AppRouter::groupName (g), g + 1);
        box.addItem ("Direct (not routed)", 100);
        box.onChange = [this]
        {
            auto st = router.getSetting (key);
            st.group = box.getSelectedId() == 100 ? AppRouter::direct : box.getSelectedId() - 1;
            router.setSetting (key, st);
        };
        addAndMakeVisible (box);
        update (s);
    }

    const juce::String key;

    void update (const AppRouter::Status& s)
    {
        status = s;
        const int id = s.setting.group == AppRouter::direct ? 100 : s.setting.group + 1;
        if (box.getSelectedId() != id) box.setSelectedId (id, juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (2);
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        drawIcon (g, status.icon, status.name, iconBox, groupColour (juce::jmax (0, status.setting.group)));
        g.setColour (status.playing ? theme::text : theme::dim);
        g.setFont (theme::font (12.5f, true));
        g.drawText (status.name, nameBox, juce::Justification::centredLeft, true);
        const auto note = status.note.isNotEmpty() ? status.note
                        : status.routed && status.moved ? juce::String ("Routed through BetterVoiceMeeter")
                        : status.setting.group == AppRouter::direct ? juce::String ("Plays directly (not routed)") : juce::String();
        g.setColour (status.routed && ! status.moved && status.playing ? theme::virt : theme::dim);
        g.setFont (theme::font (11.0f));
        g.drawText (note, noteBox, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        iconBox = r.removeFromLeft (24).withSizeKeepingCentre (22, 22);
        r.removeFromLeft (8);
        box.setBounds (r.removeFromRight (170).withSizeKeepingCentre (170, 26));
        r.removeFromRight (8);
        nameBox = r.removeFromTop (r.getHeight() / 2);
        noteBox = r;
    }

private:
    AppRouter& router;
    AppRouter::Status status;
    juce::ComboBox box;
    juce::Rectangle<int> iconBox, nameBox, noteBox;
};

//==============================================================================
// PER-APP mode strip: its own level, mute and A1..B2 routing.
class AppMixer::RouteStrip : public juce::Component
{
public:
    RouteStrip (AppRouter& r, const AppRouter::Status& s, int index)
        : router (r), key (s.key), accent (colourFor (s.key, index)),
          fader (accent, s.setting.gainDb, [this] (float v) { auto st = router.getSetting (key); st.gainDb = v; router.setSetting (key, st); repaint(); })
    {
        addAndMakeVisible (fader);
        mute.setClickingTogglesState (true);
        mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        mute.onClick = [this] { auto st = router.getSetting (key); st.mute = mute.getToggleState(); router.setSetting (key, st); };
        addAndMakeVisible (mute);
        routed.setClickingTogglesState (true);
        routed.setColour (juce::TextButton::buttonOnColourId, theme::hw);
        routed.setTooltip ("Route this app through BetterVoiceMeeter");
        routed.onClick = [this] { auto st = router.getSetting (key); st.routed = routed.getToggleState(); router.setSetting (key, st); };
        addAndMakeVisible (routed);
        for (int b = 0; b < kNumBuses; ++b)
        {
            auto* t = buses.add (new juce::TextButton (busName (b)));
            t->setClickingTogglesState (true);
            t->setColour (juce::TextButton::buttonOnColourId, b < kNumHwBuses ? theme::busA : theme::busB);
            t->onClick = [this, b, t]
            {
                auto st = router.getSetting (key);
                st.routes = t->getToggleState() ? (st.routes | (1 << b)) : (st.routes & ~(1 << b));
                router.setSetting (key, st);
            };
            addAndMakeVisible (t);
        }
        update (s);
    }

    const juce::String key;

    void update (const AppRouter::Status& s)
    {
        status = s;
        mute.setToggleState (s.setting.mute, juce::dontSendNotification);
        routed.setToggleState (s.setting.routed, juce::dontSendNotification);
        routed.setButtonText (s.setting.routed ? "ROUTED" : "DIRECT");
        for (int b = 0; b < buses.size(); ++b)
            buses[b]->setToggleState ((s.setting.routes >> b) & 1, juce::dontSendNotification);
        const float db = s.peak > 1.0e-5f ? 20.0f * std::log10 (s.peak) : -70.0f;
        level = db > level ? db : juce::jmax (-70.0f, level - 3.0f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        drawCard (g, getLocalBounds(), accent);
        drawIcon (g, status.icon, status.name, iconBox, accent);
        g.setColour (status.playing ? theme::text : theme::dim);
        g.setFont (theme::font (12.5f, true));
        g.drawFittedText (status.name, nameBox, juce::Justification::centredLeft, 2, 0.8f);
        g.setColour (status.routed && ! status.moved && status.playing ? theme::virt : theme::dim);
        g.setFont (theme::font (10.0f));
        const auto note = status.note.isNotEmpty() ? status.note : status.routed && status.moved ? juce::String ("Routed") : juce::String();
        g.drawFittedText (note, noteBox, juce::Justification::centred, 3, 0.8f);
        auto m = meterBox.toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (m, 2.0f);
        const float pos = juce::jlimit (0.0f, 1.0f, (level + 60.0f) / 60.0f);
        if (pos > 0)
        {
            g.setColour (theme::meterLow);
            g.fillRect (m.withTop (m.getBottom() - m.getHeight() * pos).reduced (1.0f, 0));
        }
        g.setColour (theme::text);
        g.setFont (theme::font (11.5f, true));
        g.drawText (fader.getTextFromValue (fader.getValue()), valueBox, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (9, 8);
        r.removeFromTop (4);
        auto head = r.removeFromTop (34);
        iconBox = head.removeFromLeft (28).withSizeKeepingCentre (26, 26);
        head.removeFromLeft (6);
        nameBox = head;
        noteBox = r.removeFromTop (38);
        routed.setBounds (r.removeFromTop (24).reduced (2, 1));
        r.removeFromTop (4);
        mute.setBounds (r.removeFromBottom (26).reduced (2, 0));
        r.removeFromBottom (2);
        auto row2 = r.removeFromBottom (24), row1 = r.removeFromBottom (24);
        const int w1 = row1.getWidth() / kNumHwBuses, w2 = row2.getWidth() / kNumVirtBuses;
        for (int b = 0; b < buses.size(); ++b)
            buses[b]->setBounds (b < kNumHwBuses ? row1.removeFromLeft (w1).reduced (1) : row2.removeFromLeft (w2).reduced (1));
        r.removeFromBottom (4);
        valueBox = r.removeFromBottom (18);
        meterBox = r.removeFromRight (12).reduced (0, 11);
        r.removeFromRight (4);
        fader.setBounds (r);
    }

private:
    AppRouter& router;
    juce::Colour accent;
    DbFader fader;
    juce::TextButton mute { "MUTE" }, routed { "ROUTED" };
    juce::OwnedArray<juce::TextButton> buses;
    AppRouter::Status status;
    float level = -70.0f;
    juce::Rectangle<int> iconBox, nameBox, noteBox, meterBox, valueBox;
};

//==============================================================================
AppMixer::AppMixer (Engine& e, std::function<void()> closeFn) : engine (e), onClose (std::move (closeFn))
{
    setWantsKeyboardFocus (true);
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (closeBtn);

    const juce::String tips[] = {
        "Windows per-app volume. Audio isn't rerouted: no delay.",
        "Route apps into Game / Chat / Music / Browser groups with separate levels for you and your stream.",
        "Route each app through its own strip with A1..B2 routing." };
    juce::TextButton* modeButtons[] = { &volumeBtn, &groupsBtn, &perAppBtn };
    for (int i = 0; i < 3; ++i)
    {
        auto* b = modeButtons[i];
        b->setClickingTogglesState (true);
        b->setRadioGroupId (9001);
        b->setColour (juce::TextButton::buttonOnColourId, theme::hw);
        b->setTooltip (tips[i]);
        b->onClick = [this, i] { setMode (i); };
        addAndMakeVisible (b);
    }

    sinkLabel.setText ("Silent device for routed apps:", juce::dontSendNotification);
    sinkLabel.setFont (theme::font (12.0f));
    sinkLabel.setColour (juce::Label::textColourId, theme::dim);
    addAndMakeVisible (sinkLabel);
    sinkBox.addItemList (AppRouter::candidateSinkDevices(), 1);
    sinkBox.setTooltip ("Routed apps are pointed at this device and captured from there. Pick one you never listen to "
                        "(e.g. an unused digital output). BetterVoiceMeeter plays them for you instead.");
    sinkBox.onChange = [this] { engine.appRouter.setSinkDevice (sinkBox.getText()); };
    addAndMakeVisible (sinkBox);

    viewport.setViewedComponent (&holder, false);
    addAndMakeVisible (viewport);

    mode = (int) engine.appRouter.getMode();
    modeButtons[mode]->setToggleState (true, juce::dontSendNotification);
    sinkBox.setText (engine.appRouter.getSinkDevice(), juce::dontSendNotification);
}

AppMixer::~AppMixer()
{
    stopTimer();
    sessions.stop();
}

void AppMixer::setMode (int m)
{
    mode = m;
    engine.appRouter.setMode ((AppRouter::Mode) m);
    signature.clear();
    rebuildContent();
    resized();
    repaint();
}

void AppMixer::visibilityChanged()
{
    if (isVisible())
    {
        sessions.start();
        startTimerHz (20);
        if (sinkBox.getText().isEmpty())
            sinkBox.setText (engine.appRouter.getSinkDevice(), juce::dontSendNotification);
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

juce::String AppMixer::contentSignature() const
{
    juce::String sig (mode);
    if (mode == 0)
        for (auto& a : sessions.snapshot()) sig << "|" << a.key;
    else
        for (auto& s : engine.appRouter.snapshot()) sig << "|" << s.key;
    return sig;
}

void AppMixer::rebuildContent()
{
    items.clear();
    rows.clear();
    if (mode == 0)
    {
        int i = 0;
        for (auto& a : sessions.snapshot())
            holder.addAndMakeVisible (items.add (new Strip (sessions, a, i++)));
    }
    else if (mode == 1)
    {
        for (int g = 0; g < AppRouter::numGroups; ++g)
            holder.addAndMakeVisible (items.add (new GroupCard (engine.appRouter, g)));
        for (auto& s : engine.appRouter.snapshot())
            holder.addAndMakeVisible (rows.add (new AssignRow (engine.appRouter, s)));
    }
    else
    {
        int i = 0;
        for (auto& s : engine.appRouter.snapshot())
            holder.addAndMakeVisible (items.add (new RouteStrip (engine.appRouter, s, i++)));
    }
    layoutContent();
}

void AppMixer::timerCallback()
{
    if (const int routerMode = (int) engine.appRouter.getMode(); routerMode != mode) // changed elsewhere
    {
        juce::TextButton* buttons[] = { &volumeBtn, &groupsBtn, &perAppBtn };
        buttons[routerMode]->setToggleState (true, juce::dontSendNotification);
        mode = routerMode;
        signature.clear();
        resized();
        repaint();
    }
    const auto sig = contentSignature();
    if (sig != signature)
    {
        signature = sig;
        rebuildContent();
        repaint();
        return;
    }
    if (mode == 0)
    {
        const auto apps = sessions.snapshot();
        for (int i = 0; i < items.size() && i < (int) apps.size(); ++i)
            if (auto* s = dynamic_cast<Strip*> (items[i])) s->update (apps[(size_t) i]);
        return;
    }
    const auto status = engine.appRouter.snapshot();
    if (mode == 1)
    {
        for (int g = 0; g < AppRouter::numGroups && g < items.size(); ++g)
        {
            juce::StringArray members;
            for (auto& s : status)
                if (s.setting.group == g) members.add (s.name);
            if (auto* card = dynamic_cast<GroupCard*> (items[g])) card->setMembers (members);
        }
        for (int i = 0; i < rows.size() && i < (int) status.size(); ++i)
            if (auto* row = dynamic_cast<AssignRow*> (rows[i])) row->update (status[(size_t) i]);
    }
    else
        for (int i = 0; i < items.size() && i < (int) status.size(); ++i)
            if (auto* st = dynamic_cast<RouteStrip*> (items[i])) st->update (status[(size_t) i]);
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

    const juce::String help[] = {
        "Volume per app on this PC. Same controls as the Windows volume mixer; audio isn't rerouted.",
        "Apps play through BetterVoiceMeeter in groups: set what you hear and what your stream hears. "
        "Adds ~20 ms to app audio; your mic stays direct.",
        "Each routed app gets its own level and bus routing. Adds ~20 ms to app audio; your mic stays direct." };
    g.setColour (theme::dim);
    g.setFont (theme::font (12.0f));
    g.drawFittedText (help[mode], helpArea, juce::Justification::centredLeft, 2);

    if (items.isEmpty() && rows.isEmpty())
    {
        g.setFont (theme::font (14.0f));
        g.drawText ("No apps are using audio right now. Play something and it appears here.",
                    viewport.getBounds(), juce::Justification::centred);
    }
}

void AppMixer::layoutContent()
{
    const auto area = viewport.getLocalBounds();
    const int scroll = viewport.getScrollBarThickness();
    if (mode == 1)
    {
        const int cardsH = juce::jmin (420, juce::jmax (300, area.getHeight() - 30 - rows.size() * 44));
        const int w = juce::jmax (area.getWidth() - scroll, AppRouter::numGroups * groupWidth);
        holder.setSize (w, cardsH + 16 + rows.size() * 44);
        for (int i = 0; i < items.size(); ++i)
            items[i]->setBounds (i * groupWidth, 0, groupWidth, cardsH);
        for (int i = 0; i < rows.size(); ++i)
            rows[i]->setBounds (0, cardsH + 16 + i * 44, juce::jmin (w, 760), 44);
    }
    else
    {
        const int h = area.getHeight() - (items.size() * stripWidth > area.getWidth() ? scroll : 0);
        holder.setSize (juce::jmax (area.getWidth(), items.size() * stripWidth), h);
        for (int i = 0; i < items.size(); ++i)
            items[i]->setBounds (i * stripWidth, 0, stripWidth, juce::jmin (h, 600));
    }
}

void AppMixer::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop (48);
    {
        auto t = topBar.reduced (10, 9);
        closeBtn.setBounds (t.removeFromRight (34));
        t.removeFromLeft (110);
        for (auto* b : { &volumeBtn, &groupsBtn, &perAppBtn })
        {
            b->setBounds (t.removeFromLeft (96));
            t.removeFromLeft (4);
        }
        t.removeFromLeft (12);
        helpArea = t;
    }
    r.reduce (10, 8);
    modeBar = mode == 0 ? juce::Rectangle<int>() : r.removeFromTop (32);
    sinkLabel.setVisible (mode != 0);
    sinkBox.setVisible (mode != 0);
    if (mode != 0)
    {
        auto m = modeBar;
        sinkLabel.setBounds (m.removeFromLeft (200));
        sinkBox.setBounds (m.removeFromLeft (360).reduced (0, 3));
        r.removeFromTop (6);
    }
    viewport.setBounds (r);
    layoutContent();
}
