#include "SingStudio.h"
#include "Widgets.h"

namespace
{
juce::String fmtTime (double s)
{
    const int t = (int) std::max (0.0, s);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

juce::String stemTitle (const juce::String& n)
{
    if (n == "lead") return "Lead vocal";
    if (n == "backing") return "Backing vocals / ad-libs";
    if (n == "drums") return "Drums";
    if (n == "bass") return "Bass";
    if (n == "other") return "Instruments";
    if (n == "instrumental") return "Instrumental";
    return n;
}

juce::Colour stemColour (const juce::String& n)
{
    if (n == "lead") return juce::Colour (0xffff5c8a);
    if (n == "backing") return juce::Colour (0xffffa94d);
    if (n == "drums") return juce::Colour (0xff5ab4ff);
    if (n == "bass") return juce::Colour (0xffb38cff);
    return juce::Colour (0xff3fd49a);
}

juce::var readJson (const juce::File& f) { return juce::JSON::parse (f.loadFileAsString()); }
} // namespace

//==============================================================================
// Runs the Python stem worker and delivers its JSON lines on the message thread.
class SingStudio::Worker : private juce::Thread
{
public:
    Worker() : juce::Thread ("Stem worker") {}
    ~Worker() override { cancel(); }

    std::function<void (const juce::var&)> onMessage;
    std::function<void (int exitCode, const juce::String& lastLines)> onFinished;

    bool start (const juce::StringArray& args)
    {
        cancel();
        if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            return false;
        startThread();
        return true;
    }

    void cancel()
    {
        signalThreadShouldExit();
        if (process.isRunning())
            process.kill();
        stopThread (3000);
    }

    bool isBusy() const { return isThreadRunning(); }

private:
    void run() override
    {
        std::string line;
        juce::StringArray tail; // last non-JSON lines (Python errors) for the error message
        char c = 0;
        // ChildProcess reads block until the requested bytes arrive, so read byte-by-byte to get lines promptly.
        while (! threadShouldExit() && process.readProcessOutput (&c, 1) == 1)
        {
            if (c != '\n')
            {
                if (c != '\r') line += c;
                continue;
            }
            const auto text = juce::String::fromUTF8 (line.data(), (int) line.size());
            line.clear();
            auto v = juce::JSON::parse (text);
            if (v.isObject() && v.hasProperty ("type"))
                post (v);
            else if (text.isNotEmpty())
            {
                tail.add (text);
                if (tail.size() > 8) tail.remove (0);
            }
        }
        const int code = threadShouldExit() ? -1 : (int) process.getExitCode();
        juce::MessageManager::callAsync ([this, code, msg = tail.joinIntoString ("\n")]
                                         { if (onFinished) onFinished (code, msg); });
    }

    void post (const juce::var& v)
    {
        juce::MessageManager::callAsync ([this, v] { if (onMessage) onMessage (v); });
    }

    juce::ChildProcess process;
};

//==============================================================================
class SingStudio::ResultList : public juce::Component, private juce::ListBoxModel
{
public:
    std::function<void (const Item&)> onChoose;

    ResultList()
    {
        box.setModel (this);
        box.setRowHeight (46);
        box.setColour (juce::ListBox::backgroundColourId, theme::panel);
        box.setColour (juce::ListBox::outlineColourId, theme::border);
        box.setOutlineThickness (1);
        addAndMakeVisible (box);
    }

    void setItems (const std::vector<Item>& newItems, const juce::String& empty)
    {
        items = newItems;
        emptyText = empty;
        box.updateContent();
        box.repaint();
    }

    void resized() override { box.setBounds (getLocalBounds()); }

    void paintOverChildren (juce::Graphics& g) override
    {
        if (! items.empty()) return;
        g.setColour (theme::dim);
        g.setFont (theme::font (12.5f));
        g.drawFittedText (emptyText, getLocalBounds().reduced (20), juce::Justification::centred, 4);
    }

private:
    int getNumRows() override { return (int) items.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) items.size())) return;
        const auto& it = items[(size_t) row];
        if (selected) g.fillAll (theme::hw.withAlpha (0.18f));
        g.setColour (theme::border.withAlpha (0.5f));
        g.drawHorizontalLine (h - 1, 0.0f, (float) w);

        auto r = juce::Rectangle<int> (w, h).reduced (10, 5);
        auto badge = r.removeFromRight (it.processed ? 56 : 44);
        g.setFont (theme::font (10.5f, true));
        g.setColour (it.processed ? theme::busA : theme::dim);
        g.drawText (it.processed ? "READY" : fmtTime (it.duration), badge, juce::Justification::centredRight);

        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (it.title, r.removeFromTop (19), juce::Justification::centredLeft, true);
        g.setColour (theme::dim);
        g.setFont (theme::font (11.5f));
        g.drawText (it.channel, r, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (juce::isPositiveAndBelow (row, (int) items.size()) && onChoose)
            onChoose (items[(size_t) row]);
    }

    juce::ListBox box;
    std::vector<Item> items;
    juce::String emptyText;
};

//==============================================================================
// One stem: name, mute, solo, volume (0-150%), level.
class SingStudio::StemRow : public juce::Component, private juce::Timer
{
public:
    StemRow (SongPlayer::StemControl& c, const juce::String& stemName) : control (c), name (stemName), colour (stemColour (stemName))
    {
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 150.0, 1.0);
        volume.setValue (control.volume.load() * 100.0, juce::dontSendNotification);
        volume.setDoubleClickReturnValue (true, 100.0);
        volume.setColour (juce::Slider::thumbColourId, colour);
        volume.setColour (juce::Slider::trackColourId, colour.withAlpha (0.6f));
        volume.setColour (juce::Slider::backgroundColourId, theme::bg);
        volume.onValueChange = [this] { control.volume = (float) volume.getValue() / 100.0f; repaint(); };
        addAndMakeVisible (volume);

        for (auto* b : { &mute, &solo })
        {
            b->setClickingTogglesState (true);
            addAndMakeVisible (b);
        }
        mute.setColour (juce::TextButton::buttonOnColourId, theme::mute);
        solo.setColour (juce::TextButton::buttonOnColourId, theme::solo);
        mute.onClick = [this] { control.mute = mute.getToggleState(); };
        solo.onClick = [this] { control.solo = solo.getToggleState(); };
        startTimerHz (25);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (0, 3);
        g.setColour (theme::panelHi.withAlpha (0.6f));
        g.fillRoundedRectangle (r.toFloat(), 5.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.removeFromLeft (4).toFloat(), 2.0f);

        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (stemTitle (name), nameArea, juce::Justification::centredLeft, true);
        g.setColour (colour);
        g.drawText (juce::String ((int) volume.getValue()) + "%", valueArea, juce::Justification::centredRight);

        // level bar under the slider
        auto lv = meterArea.toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (lv, 1.5f);
        g.setColour (colour.withAlpha (0.85f));
        g.fillRoundedRectangle (lv.withWidth (lv.getWidth() * juce::jlimit (0.0f, 1.0f, level)), 1.5f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 6);
        nameArea = r.removeFromLeft (190);
        mute.setBounds (r.removeFromLeft (30).reduced (1, 2));
        solo.setBounds (r.removeFromLeft (30).reduced (1, 2));
        r.removeFromLeft (10);
        valueArea = r.removeFromRight (48);
        meterArea = r.removeFromBottom (3);
        volume.setBounds (r);
    }

private:
    void timerCallback() override
    {
        const float pk = control.peak.load();
        const float lv = juce::jlimit (0.0f, 1.0f, (bvm::gainToDb (pk) + 50.0f) / 50.0f);
        level = juce::jmax (lv, level - 0.04f);
        repaint (meterArea.expanded (2));
    }

    SongPlayer::StemControl& control;
    juce::String name;
    juce::Colour colour;
    juce::Slider volume;
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Rectangle<int> nameArea, valueArea, meterArea;
    float level = 0;
};

//==============================================================================
// Karaoke lyrics: previous line, current line filling as it's sung, next lines.
class SingStudio::LyricsView : public juce::Component, private juce::Timer
{
public:
    explicit LyricsView (SongPlayer& p) : player (p) { startTimerHz (30); }

    struct Word { double t, end; juce::String w; };
    struct Line { double t, end; juce::String text; std::vector<Word> words; };

    void load (const juce::File& f)
    {
        file = f;
        lines.clear();
        offset = 0;
        source.clear();
        const auto v = readJson (f);
        if (auto* obj = v.getDynamicObject())
        {
            offset = (double) obj->getProperty ("offset");
            source = obj->getProperty ("source").toString();
            if (auto* arr = obj->getProperty ("lines").getArray())
                for (auto& l : *arr)
                {
                    Line line { (double) l["t"], (double) l["end"], l["text"].toString(), {} };
                    if (auto* ws = l["words"].getArray())
                        for (auto& w : *ws)
                            line.words.push_back ({ (double) w["t"], (double) w["end"], w["w"].toString() });
                    lines.push_back (std::move (line));
                }
        }
        repaint();
    }

    void nudge (double delta)
    {
        offset = std::round ((offset + delta) * 100.0) / 100.0;
        if (auto v = readJson (file); auto* obj = v.getDynamicObject())
        {
            obj->setProperty ("offset", offset);
            file.replaceWithText (juce::JSON::toString (v));
        }
        repaint();
    }

    double getOffset() const { return offset; }
    juce::String getSource() const { return source; }
    bool hasLyrics() const { return ! lines.empty(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::bg.withAlpha (0.6f));
        g.fillRoundedRectangle (r, 8.0f);

        if (lines.empty())
        {
            g.setColour (theme::dim);
            g.setFont (theme::font (15.0f));
            g.drawFittedText (file.existsAsFile() ? "No synced lyrics found for this song."
                                                  : "Lyrics appear here when a song is loaded.",
                              getLocalBounds().reduced (30), juce::Justification::centred, 3);
            return;
        }

        // Lyrics lead slightly so you can read ahead, like Moises / Apple Music.
        const double pos = player.getPosition() + offset + 0.15;
        int cur = -1;
        for (int i = 0; i < (int) lines.size(); ++i)
            if (lines[(size_t) i].t <= pos) cur = i; else break;

        const float h = r.getHeight();
        const float bigSize = juce::jlimit (20.0f, 38.0f, h / 7.0f);
        const float smallSize = bigSize * 0.62f;
        const float centreY = r.getY() + h * 0.42f;

        auto drawLine = [&] (int idx, float y, float size, juce::Colour c)
        {
            if (! juce::isPositiveAndBelow (idx, (int) lines.size())) return;
            g.setColour (c);
            g.setFont (theme::font (size, true));
            g.drawFittedText (lines[(size_t) idx].text, juce::Rectangle<float> (r.getX() + 20, y - size, r.getWidth() - 40, size * 2).toNearestInt(),
                              juce::Justification::centred, 1, 0.7f);
        };

        drawLine (cur - 1, centreY - bigSize * 1.9f, smallSize, theme::dim.withAlpha (0.55f));
        drawLine (cur + 1, centreY + bigSize * 1.8f, smallSize, theme::dim);
        drawLine (cur + 2, centreY + bigSize * 1.8f + smallSize * 1.6f, smallSize, theme::dim.withAlpha (0.6f));

        if (cur < 0)
        {
            g.setColour (theme::dim);
            g.setFont (theme::font (bigSize * 0.8f, true));
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x99\xaa"), r.withHeight (bigSize * 2).withCentre ({ r.getCentreX(), centreY }),
                        juce::Justification::centred);
            return;
        }

        const auto& line = lines[(size_t) cur];
        // Instrumental gap: the line is long finished and the next one is far away.
        const double lineLen = std::min (line.end - line.t, 0.6 + line.text.length() * 0.09);
        if (pos > line.t + lineLen + 3.0 && (cur + 1 >= (int) lines.size() || lines[(size_t) cur + 1].t - pos > 3.0))
        {
            g.setColour (theme::dim);
            g.setFont (theme::font (bigSize * 0.8f, true));
            g.drawText (juce::CharPointer_UTF8 ("\xe2\x99\xaa"), r.withHeight (bigSize * 2).withCentre ({ r.getCentreX(), centreY }),
                        juce::Justification::centred);
            return;
        }

        double fill;
        if (! line.words.empty())
        {
            int done = 0;
            for (auto& w : line.words) if (w.end <= pos) ++done;
            fill = (double) done / (double) line.words.size();
        }
        else
            fill = juce::jlimit (0.0, 1.0, (pos - line.t) / std::max (0.3, lineLen));

        auto font = theme::font (bigSize, true);
        const float maxW = r.getWidth() - 40;
        float textW = juce::GlyphArrangement::getStringWidth (font, line.text);
        if (textW > maxW)
        {
            font = theme::font (bigSize * maxW / textW, true);
            textW = maxW;
        }
        const auto textRect = juce::Rectangle<float> (textW + 4, font.getHeight() * 1.4f).withCentre ({ r.getCentreX(), centreY });
        g.setFont (font);
        g.setColour (theme::text.withAlpha (0.35f));
        g.drawText (line.text, textRect, juce::Justification::centred, false);
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (textRect.withWidth (textRect.getWidth() * (float) fill).toNearestInt());
            g.setColour (juce::Colours::white);
            g.drawText (line.text, textRect, juce::Justification::centred, false);
        }
    }

private:
    void timerCallback() override { if (player.isPlaying() || ! lines.empty()) repaint(); }

    SongPlayer& player;
    juce::File file;
    std::vector<Line> lines;
    double offset = 0;
    juce::String source;
};

//==============================================================================
namespace
{
// Song volume, mute and bus routing (where the karaoke track goes in the mixer).
class SongOutputRow : public juce::Component
{
public:
    explicit SongOutputRow (Engine& e)
        : gain (*e.song.gain, juce::Slider::LinearHorizontal, theme::fx), mute (*e.song.mute, "MUTE", theme::mute),
          meter (e.songMeter)
    {
        gain.formatter = [] (float v) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
        gain.setPopupDisplayEnabled (true, true, nullptr);
        addAndMakeVisible (gain);
        addAndMakeVisible (mute);
        addAndMakeVisible (meter);
        for (int b = 0; b < kNumBuses; ++b)
        {
            auto* t = routes.add (new ParamToggle (*e.song.routes[(size_t) b], busName (b), b < kNumHwBuses ? theme::busA : theme::busB));
            t->setTooltip ("Send the song to bus " + busName (b) + (b == kNumHwBuses ? " (e.g. your stream / Discord)" : ""));
            addAndMakeVisible (t);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::dim);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("SONG OUT", getLocalBounds().removeFromLeft (80), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromLeft (80);
        for (int b = kNumBuses; --b >= 0;)
            routes[b]->setBounds (r.removeFromRight (40).reduced (2, 4));
        r.removeFromRight (8);
        mute.setBounds (r.removeFromRight (56).reduced (2, 4));
        r.removeFromRight (8);
        meter.setBounds (r.removeFromRight (14).reduced (0, 4));
        r.removeFromRight (6);
        gain.setBounds (r);
    }

private:
    ParamSlider gain;
    ParamToggle mute;
    LevelMeter meter;
    juce::OwnedArray<ParamToggle> routes;
};
} // namespace

//==============================================================================
juce::File SingStudio::getPython()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
        .getChildFile ("BetterVoiceMeeter/stems-env/Scripts/python.exe");
}

juce::File SingStudio::getWorkerScript()
{
    // Installed next to the exe; when running from a build tree, find Tools/ in the repo.
    auto exeDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    auto f = exeDir.getChildFile ("stemworker/stemworker.py");
    for (auto d = exeDir; ! f.existsAsFile() && d.getParentDirectory() != d; d = d.getParentDirectory())
        f = d.getChildFile ("Tools/stemworker/stemworker.py");
    return f;
}

SingStudio::SingStudio (Engine& e, std::function<void()> closeFn) : engine (e), onClose (std::move (closeFn))
{
    setWantsKeyboardFocus (true);

    searchBox.setTextToShowWhenEmpty ("Search a song on YouTube (artist - title)", theme::dim);
    searchBox.setFont (theme::font (14.0f));
    searchBox.setColour (juce::TextEditor::backgroundColourId, theme::bg);
    searchBox.setColour (juce::TextEditor::outlineColourId, theme::border);
    searchBox.setColour (juce::TextEditor::focusedOutlineColourId, theme::hw);
    searchBox.setColour (juce::TextEditor::textColourId, theme::text);
    searchBox.setIndents (10, 7);
    searchBox.onReturnKey = [this] { runSearch(); };
    addAndMakeVisible (searchBox);
    searchBtn.onClick = [this] { runSearch(); };

    engineBox.addItem ("Local GPU (free)", 1);
    engineBox.addItem ("Music.AI cloud", 2);
    engineBox.setSelectedId (engine.sing.engine == "musicai" ? 2 : 1, juce::dontSendNotification);
    engineBox.onChange = [this] { engine.sing.engine = engineBox.getSelectedId() == 2 ? "musicai" : "local"; updateEngineBanner(); };
    engineBox.setTooltip ("Which AI separates the song");
    stemsBox.addItem ("5 stems", 5);
    stemsBox.addItem ("3 stems (faster)", 3);
    stemsBox.setSelectedId (engine.sing.stems == 3 ? 3 : 5, juce::dontSendNotification);
    stemsBox.onChange = [this] { engine.sing.stems = stemsBox.getSelectedId(); };
    stemsBox.setTooltip ("5: lead, backing, drums, bass, instruments.  3: lead, backing, instrumental (local engine)");

    settingsBtn.onClick = [this] { showMusicAiSettings(); };
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    installBtn.onClick = [this] { installEngine(); };
    cancelBtn.onClick = [this] { processWorker->cancel(); setBusy (false, "Cancelled"); };
    resultsTab.onClick = [this] { showLibrary (false); };
    libraryTab.onClick = [this] { showLibrary (true); };
    for (auto* t : { &resultsTab, &libraryTab })
    {
        t->setClickingTogglesState (true);
        t->setRadioGroupId (777);
        t->setColour (juce::TextButton::buttonOnColourId, theme::hw);
    }

    playBtn.onClick = [this] { engine.songPlayer.setPlaying (! engine.songPlayer.isPlaying()); };
    backBtn.onClick = [this] { engine.songPlayer.seek (engine.songPlayer.getPosition() - 5.0); };
    fwdBtn.onClick = [this] { engine.songPlayer.seek (engine.songPlayer.getPosition() + 5.0); };
    offsetDown.onClick = [this] { lyrics->nudge (-0.25); };
    offsetUp.onClick = [this] { lyrics->nudge (0.25); };
    findLyricsBtn.onClick = [this] { findLyrics(); };
    findLyricsBtn.setTooltip ("Search the lyrics again (edit artist / title) and sync them to this recording with AI if needed");
    offsetDown.setTooltip ("Lyrics are late? Show them earlier");
    offsetUp.setTooltip ("Lyrics are early? Show them later");

    positionSlider.setSliderStyle (juce::Slider::LinearBar);
    positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    positionSlider.setColour (juce::Slider::trackColourId, theme::hw.withAlpha (0.5f));
    positionSlider.setColour (juce::Slider::backgroundColourId, theme::bg);
    positionSlider.setRange (0.0, 1.0);
    positionSlider.onDragEnd = [this] { engine.songPlayer.seek (positionSlider.getValue()); };
    positionSlider.onValueChange = [this] { if (positionSlider.isMouseButtonDown()) engine.songPlayer.seek (positionSlider.getValue()); };

    for (juce::Component* c : { (juce::Component*) &searchBtn, (juce::Component*) &engineBox, (juce::Component*) &stemsBox,
                                (juce::Component*) &settingsBtn, (juce::Component*) &closeBtn, (juce::Component*) &resultsTab,
                                (juce::Component*) &libraryTab, (juce::Component*) &playBtn, (juce::Component*) &backBtn,
                                (juce::Component*) &fwdBtn, (juce::Component*) &positionSlider, (juce::Component*) &offsetDown,
                                (juce::Component*) &offsetUp, (juce::Component*) &findLyricsBtn })
        addAndMakeVisible (c);
    addChildComponent (installBtn);
    addChildComponent (cancelBtn);

    list = std::make_unique<ResultList>();
    list->onChoose = [this] (const Item& it) { choose (it); };
    addAndMakeVisible (*list);

    lyrics = std::make_unique<LyricsView> (engine.songPlayer);
    addAndMakeVisible (*lyrics);
    outputRow = std::make_unique<SongOutputRow> (engine);
    addAndMakeVisible (*outputRow);

    searchWorker = std::make_unique<Worker>();
    processWorker = std::make_unique<Worker>();

    searchWorker->onMessage = [this] (const juce::var& v)
    {
        if (v["type"] == "results")
        {
            results.clear();
            if (auto* arr = v["items"].getArray())
                for (auto& it : *arr)
                {
                    Item item { it["id"].toString(), it["title"].toString(), it["channel"].toString(), it["thumbnail"].toString(),
                                (double) it["duration"], false };
                    item.processed = Engine::getSongsDir().getChildFile (item.id).getChildFile ("meta.json").existsAsFile();
                    results.push_back (item);
                }
            showLibrary (false);
            statusText = juce::String ((int) results.size()) + " results. Click one to separate it.";
        }
        else if (v["type"] == "error")
            statusText = "Search failed: " + v["message"].toString();
        repaint();
    };
    searchWorker->onFinished = [this] (int code, const juce::String& tail)
    {
        if (code > 0 && results.empty() && statusText.startsWith ("Searching"))
            statusText = "Search failed. " + tail.fromLastOccurrenceOf ("\n", false, false);
        repaint();
    };

    processWorker->onMessage = [this] (const juce::var& v)
    {
        if (v["type"] == "progress")
            setBusy (true, v["message"].toString(), (double) v["pct"]);
        else if (v["type"] == "done" && lyricsJob)
        {
            lyricsJob = false;
            const juce::File dir (v["dir"].toString());
            if (dir == currentSong)
                lyrics->load (dir.getChildFile ("lyrics.json"));
            setBusy (false, "Lyrics: " + v["meta"]["lyrics"].toString());
            refreshLibrary();
            showLibrary (showingLibrary);
        }
        else if (v["type"] == "done")
        {
            setBusy (false, "Ready");
            loadSong (juce::File (v["dir"].toString()));
            refreshLibrary();
            for (auto& r : results) if (r.id == currentSong.getFileName()) r.processed = true;
            showLibrary (showingLibrary);
        }
        else if (v["type"] == "error")
        {
            lyricsJob = false;
            setBusy (false, "Error: " + v["message"].toString());
        }
    };
    processWorker->onFinished = [this] (int code, const juce::String& tail)
    {
        lyricsJob = false;
        if (busy) // ended without "done"/"error" (crash)
            setBusy (false, code == -1 ? "Cancelled" : "Processing stopped. " + tail.fromLastOccurrenceOf ("\n", false, false));
    };

    refreshLibrary();
    showLibrary (true);
    updateEngineBanner();
    if (engine.songPlayer.isLoaded())
        rebuildStemRows();
    startTimerHz (15);
}

SingStudio::~SingStudio()
{
    searchWorker->cancel();
    processWorker->cancel();
}

void SingStudio::visibilityChanged()
{
    if (isVisible())
    {
        updateEngineBanner();
        searchBox.grabKeyboardFocus();
    }
}

void SingStudio::updateEngineBanner()
{
    bannerText.clear();
    const bool hasPython = getPython().existsAsFile();
    if (! getWorkerScript().existsAsFile())
        bannerText = "Stem worker script not found next to the app (stemworker/stemworker.py).";
    else if (! hasPython)
        bannerText = "The AI engine isn't installed yet: Python + PyTorch (CUDA 12.6 for your GTX 10-series) + separation models, about 3 GB.";
    else if (engine.sing.engine == "musicai" && (engine.sing.musicaiKey.isEmpty() || engine.sing.musicaiWorkflow.isEmpty()))
        bannerText = "Music.AI selected: click MUSIC.AI... to paste your API key and workflow ID.";
    installBtn.setVisible (! hasPython && getWorkerScript().existsAsFile());
    resized();
    repaint();
}

void SingStudio::refreshLibrary()
{
    library.clear();
    for (auto& meta : Engine::getSongsDir().findChildFiles (juce::File::findFiles, true, "meta.json"))
    {
        auto v = readJson (meta);
        Item it { v["id"].toString(), v["title"].toString(), "", v["thumbnail"].toString(), (double) v["duration"], true };
        it.channel = v["artist"].toString() + "  -  " + juce::String (v["stems"].getArray() != nullptr ? v["stems"].getArray()->size() : 0)
                   + " stems, lyrics: " + v["lyrics"].toString();
        library.push_back (it);
    }
    std::sort (library.begin(), library.end(), [] (auto& a, auto& b) { return a.title.compareIgnoreCase (b.title) < 0; });
}

void SingStudio::showLibrary (bool lib)
{
    showingLibrary = lib;
    resultsTab.setToggleState (! lib, juce::dontSendNotification);
    libraryTab.setToggleState (lib, juce::dontSendNotification);
    list->setItems (lib ? library : results,
                    lib ? "Songs you separate are kept here.\nSearch for a song to start."
                        : "Type a song in the search box and press Enter.");
}

void SingStudio::runSearch()
{
    const auto q = searchBox.getText().trim();
    if (q.isEmpty()) return;
    if (! getPython().existsAsFile()) { updateEngineBanner(); return; }
    statusText = "Searching YouTube for \"" + q + "\"...";
    results.clear();
    showLibrary (false);
    searchWorker->start ({ getPython().getFullPathName(), "-u", getWorkerScript().getFullPathName(), "search", q, "--limit", "12" });
    repaint();
}

void SingStudio::choose (const Item& it)
{
    const auto dir = Engine::getSongsDir().getChildFile (it.id);
    if (it.processed && dir.getChildFile ("meta.json").existsAsFile())
    {
        loadSong (dir);
        return;
    }
    if (busy)
    {
        statusText = "Already processing a song. Wait or press CANCEL.";
        repaint();
        return;
    }
    startProcessing (it);
}

void SingStudio::startProcessing (const Item& it)
{
    if (! getPython().existsAsFile()) { updateEngineBanner(); return; }
    juce::StringArray args { getPython().getFullPathName(), "-u", getWorkerScript().getFullPathName(), "process", it.id,
                             "--songs", Engine::getSongsDir().getFullPathName(), "--engine", engine.sing.engine,
                             "--stems", juce::String (engine.sing.stems) };
    if (engine.sing.engine == "musicai")
        args.addArray ({ "--musicai-key", engine.sing.musicaiKey, "--musicai-workflow", engine.sing.musicaiWorkflow });
    Engine::getSongsDir().createDirectory();
    nowTitle = it.title;
    nowSubtitle = "Separating...";
    setBusy (true, "Starting...", 0.0);
    if (! processWorker->start (args))
        setBusy (false, "Could not start the stem worker (Python)");
}

void SingStudio::loadSong (const juce::File& dir)
{
    const auto meta = readJson (dir.getChildFile ("meta.json"));
    juce::StringArray stems;
    if (auto* arr = meta["stems"].getArray())
        for (auto& s : *arr) stems.add (s.toString());

    stemRows.clear(); // rows reference the player's stem controls
    if (auto err = engine.songPlayer.load (dir, stems); err.isNotEmpty())
    {
        statusText = err;
        repaint();
        return;
    }
    currentSong = dir;
    nowTitle = meta["track"].toString().isNotEmpty() ? meta["track"].toString() : meta["title"].toString();
    nowSubtitle = meta["artist"].toString() + "   |   separated with "
                + (meta["engine"].toString() == "musicai" ? juce::String ("Music.AI") : juce::String ("local GPU"));
    lyrics->load (dir.getChildFile ("lyrics.json"));
    positionSlider.setRange (0.0, juce::jmax (1.0, engine.songPlayer.getLength()));
    rebuildStemRows();
    engine.songPlayer.setPlaying (true);
    statusText = "Playing. Lower the lead vocal to sing it yourself.";
    repaint();
}

void SingStudio::rebuildStemRows()
{
    stemRows.clear();
    const auto names = engine.songPlayer.getStemNames();
    for (int i = 0; i < names.size(); ++i)
        addAndMakeVisible (stemRows.add (new StemRow (engine.songPlayer.stem (i), names[i])));
    resized();
}

void SingStudio::setBusy (bool b, const juce::String& message, double pct)
{
    busy = b;
    busyPct = pct;
    statusText = message;
    cancelBtn.setVisible (b);
    repaint();
}

void SingStudio::showMusicAiSettings()
{
    auto* aw = new juce::AlertWindow ("Music.AI engine",
                                      "Create a workflow at music.ai (dashboard > Workflows) with the modules you want, e.g.\n"
                                      "Vocals Stems (lead + backing), Musical Stems (drums, bass, other) and Lyrics.\n"
                                      "Then paste your API key and the workflow ID (slug) here.\n"
                                      "Cost is billed by Music.AI per minute of audio (roughly $0.70-1.70 per song).",
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("key", engine.sing.musicaiKey, "API key:", true);
    aw->addTextEditor ("workflow", engine.sing.musicaiWorkflow, "Workflow ID:");
    aw->addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int result)
    {
        std::unique_ptr<juce::AlertWindow> owner (aw);
        if (result == 1)
        {
            engine.sing.musicaiKey = aw->getTextEditorContents ("key").trim();
            engine.sing.musicaiWorkflow = aw->getTextEditorContents ("workflow").trim();
            engine.saveState();
            updateEngineBanner();
        }
    }), false);
}

void SingStudio::findLyrics()
{
    if (! currentSong.isDirectory())
    {
        statusText = "Load a song first.";
        repaint();
        return;
    }
    if (busy)
    {
        statusText = "Busy - wait for the current job or press CANCEL.";
        repaint();
        return;
    }
    const auto meta = readJson (currentSong.getChildFile ("meta.json"));
    auto* aw = new juce::AlertWindow ("Find lyrics",
                                      "Lyrics are searched on LRCLIB. If none are synced to this recording, the AI times\n"
                                      "each word against the isolated vocal. If nothing is found online, the vocal is transcribed.",
                                      juce::MessageBoxIconType::NoIcon, this);
    aw->addTextEditor ("artist", meta["artist"].toString(), "Artist:");
    aw->addTextEditor ("title", meta["track"].toString(), "Song title:");
    aw->addComboBox ("mode", { "Use online timing when it matches this recording",
                               "Always re-sync to this recording with AI" }, "Timing:");
    aw->addButton ("SEARCH", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int result)
    {
        std::unique_ptr<juce::AlertWindow> owner (aw);
        if (result != 1) return;
        juce::StringArray args { getPython().getFullPathName(), "-u", getWorkerScript().getFullPathName(), "lyrics",
                                 currentSong.getFullPathName(), "--artist", aw->getTextEditorContents ("artist").trim(),
                                 "--title", aw->getTextEditorContents ("title").trim() };
        if (aw->getComboBoxComponent ("mode")->getSelectedItemIndex() == 1)
            args.add ("--resync");
        lyricsJob = true;
        setBusy (true, "Searching lyrics...", 0.9);
        if (! processWorker->start (args))
        {
            lyricsJob = false;
            setBusy (false, "Could not start the stem worker (Python)");
        }
    }), false);
}

void SingStudio::installEngine()
{
    const auto setup = getWorkerScript().getSiblingFile ("setup.ps1");
    if (! setup.existsAsFile()) { statusText = "setup.ps1 not found"; repaint(); return; }
    installBtn.setEnabled (false);
    setBusy (true, "Installing the AI engine (downloads ~3 GB, takes a few minutes)...", -1);
    processWorker->onFinished = [this, prev = processWorker->onFinished] (int code, const juce::String& tail)
    {
        installBtn.setEnabled (true);
        setBusy (false, code == 0 ? "AI engine installed." : "Install failed: " + tail.fromLastOccurrenceOf ("\n", false, false));
        processWorker->onFinished = prev;
        updateEngineBanner();
    };
    processWorker->start ({ "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", setup.getFullPathName() });
}

bool SingStudio::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onClose) onClose(); return true; }
    if (k == juce::KeyPress::spaceKey && ! searchBox.hasKeyboardFocus (true))
    {
        engine.songPlayer.setPlaying (! engine.songPlayer.isPlaying());
        return true;
    }
    return false;
}

void SingStudio::timerCallback()
{
    auto& p = engine.songPlayer;
    if (! positionSlider.isMouseButtonDown())
        positionSlider.setValue (p.getPosition(), juce::dontSendNotification);
    playBtn.setButtonText (p.isPlaying() ? "PAUSE" : "PLAY");
    for (juce::Component* b : { (juce::Component*) &playBtn, (juce::Component*) &backBtn, (juce::Component*) &fwdBtn, (juce::Component*) &positionSlider })
        b->setEnabled (p.isLoaded());
    repaint (transportArea);
    repaint (lyricsArea.withHeight (26).translated (0, -26));
}

void SingStudio::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    g.setColour (theme::panel);
    g.fillRect (topBar);
    g.setColour (theme::border);
    g.drawHorizontalLine (topBar.getBottom(), 0.0f, (float) getWidth());

    g.setColour (theme::text);
    g.setFont (theme::font (17.0f, true));
    g.drawText ("SING", topBar.withWidth (130).reduced (14, 0), juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xffff5c8a));
    g.drawText ("STUDIO", topBar.withWidth (130).reduced (14, 0).withTrimmedLeft (52), juce::Justification::centredLeft);

    if (! bannerArea.isEmpty())
    {
        g.setColour (theme::virt.withAlpha (0.15f));
        g.fillRoundedRectangle (bannerArea.toFloat(), 6.0f);
        g.setColour (theme::virt);
        g.setFont (theme::font (12.5f, true));
        g.drawFittedText (bannerText, bannerArea.reduced (12, 4).withTrimmedRight (installBtn.isVisible() ? 190 : 0),
                          juce::Justification::centredLeft, 2);
    }

    // Now playing
    g.setColour (theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText (nowTitle.isEmpty() ? juce::String ("No song loaded") : nowTitle, nowArea.removeFromTop (30), juce::Justification::centredLeft, true);
    g.setColour (theme::dim);
    g.setFont (theme::font (12.5f));
    g.drawText (nowSubtitle, nowArea, juce::Justification::topLeft, true);

    // Time
    auto t = transportArea;
    g.setColour (theme::text);
    g.setFont (theme::font (12.5f, true));
    g.drawText (fmtTime (engine.songPlayer.getPosition()) + " / " + fmtTime (engine.songPlayer.getLength()),
                t.removeFromRight (90), juce::Justification::centredRight);

    // Lyrics header
    if (! lyricsArea.isEmpty())
    {
        auto hdr = lyricsArea.withHeight (24).translated (0, -26);
        g.setColour (theme::dim);
        g.setFont (theme::font (11.0f, true));
        auto src = lyrics->getSource();
        g.drawText ("LYRICS" + (src.isNotEmpty() ? "  (" + src + ")" : juce::String()), hdr, juce::Justification::centredLeft);
        g.drawText ("timing " + juce::String (lyrics->getOffset(), 2) + " s", hdr.withTrimmedRight (64).withTrimmedLeft (hdr.getWidth() - 214),
                    juce::Justification::centredRight);
    }

    // Status / progress
    auto s = statusArea;
    if (busy)
    {
        auto bar = s.removeFromBottom (6).toFloat();
        g.setColour (theme::bg.brighter (0.1f));
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (juce::Colour (0xffff5c8a));
        if (busyPct >= 0)
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) busyPct), 3.0f);
        else // indeterminate: moving block
        {
            const float x = (float) std::fmod (juce::Time::getMillisecondCounter() / 1500.0, 1.0);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * 0.25f).withX (bar.getX() + x * bar.getWidth() * 0.75f), 3.0f);
        }
    }
    g.setColour (busy ? theme::text : theme::dim);
    g.setFont (theme::font (12.5f));
    g.drawText (statusText, s.withTrimmedRight (busy ? 90 : 0), juce::Justification::centredLeft, true);
}

void SingStudio::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop (48);
    {
        auto t = topBar.reduced (10, 9);
        t.removeFromLeft (130);
        closeBtn.setBounds (t.removeFromRight (34));
        t.removeFromRight (8);
        settingsBtn.setBounds (t.removeFromRight (100));
        t.removeFromRight (6);
        stemsBox.setBounds (t.removeFromRight (140));
        t.removeFromRight (6);
        engineBox.setBounds (t.removeFromRight (160));
        t.removeFromRight (14);
        searchBtn.setBounds (t.removeFromRight (90));
        t.removeFromRight (6);
        searchBox.setBounds (t);
    }

    r.reduce (12, 10);
    bannerArea = {};
    if (bannerText.isNotEmpty())
    {
        bannerArea = r.removeFromTop (44);
        installBtn.setBounds (bannerArea.reduced (8).removeFromRight (180));
        r.removeFromTop (10);
    }

    statusArea = r.removeFromBottom (30);
    cancelBtn.setBounds (statusArea.removeFromRight (84).withTrimmedBottom (8).reduced (0, 2).translated (0, 0));
    statusArea = statusArea.withTrimmedRight (-84);
    r.removeFromBottom (6);

    listArea = r.removeFromLeft (juce::jmin (380, r.getWidth() / 3));
    {
        auto l = listArea;
        auto tabs = l.removeFromTop (30);
        resultsTab.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2).reduced (2, 2));
        libraryTab.setBounds (tabs.reduced (2, 2));
        l.removeFromTop (6);
        list->setBounds (l);
    }
    r.removeFromLeft (16);

    playerArea = r;
    nowArea = r.removeFromTop (50);
    transportArea = r.removeFromTop (34);
    {
        auto t = transportArea;
        t.removeFromRight (96);
        backBtn.setBounds (t.removeFromLeft (52).reduced (2, 3));
        playBtn.setBounds (t.removeFromLeft (80).reduced (2, 3));
        fwdBtn.setBounds (t.removeFromLeft (52).reduced (2, 3));
        t.removeFromLeft (10);
        positionSlider.setBounds (t.reduced (0, 9));
    }
    r.removeFromTop (8);

    const int rowH = 40;
    stemsArea = r.removeFromTop (rowH * juce::jmax (1, stemRows.size()));
    {
        auto s = stemsArea;
        for (auto* row : stemRows)
            row->setBounds (s.removeFromTop (rowH));
    }
    r.removeFromTop (6);
    outputArea = r.removeFromTop (34);
    outputRow->setBounds (outputArea);
    r.removeFromTop (34);

    lyricsArea = r;
    lyrics->setBounds (lyricsArea);
    auto hdr = lyricsArea.withHeight (24).translated (0, -26);
    offsetUp.setBounds (hdr.removeFromRight (28).reduced (1));
    offsetDown.setBounds (hdr.removeFromRight (28).reduced (1));
    hdr.removeFromRight (150);
    findLyricsBtn.setBounds (hdr.removeFromRight (110).reduced (1));
}
