#pragma once
#include <JuceHeader.h>
#include "../Engine.h"

// Moises-style song separation + karaoke player:
// search YouTube -> split into lead vocal / backing vocals & ad-libs / drums / bass / instruments
// -> play inside the mixer with per-stem faders and synced lyrics.
class SingStudio : public juce::Component, private juce::Timer
{
public:
    SingStudio (Engine&, std::function<void()> onClose);
    ~SingStudio() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    // Opens a song from the library (used by the --sing command-line option).
    void openFromLibrary (const juce::String& songId, double seekSeconds, bool play);

    // Implementation details (public so the private helper classes in the .cpp can use them).
    struct Item
    {
        juce::String id, title, channel, thumbnail;
        double duration = 0;
        bool processed = false;
    };
    class Worker;
    class ResultList;
    class StemRow;
    class LyricsView;

private:
    void timerCallback() override;
    void runSearch();
    void choose (const Item&);
    void startProcessing (const Item&);
    void loadSong (const juce::File& dir);
    void refreshLibrary();
    void showLibrary (bool library);
    void showMusicAiSettings();
    void showAccountSettings();
    void showMoisesLogin();
    void checkMoisesAccount();
    static juce::File getMoisesTokenFile();
    void findLyrics();
    void installEngine();
    void setBusy (bool busy, const juce::String& message = {}, double pct = -1);
    void rebuildStemRows();
    void updateEngineBanner();

    static juce::File getPython();
    static juce::File getWorkerScript();

    Engine& engine;
    std::function<void()> onClose;

    juce::TextEditor searchBox;
    juce::TextButton searchBtn { "SEARCH" }, resultsTab { "RESULTS" }, libraryTab { "LIBRARY" },
        settingsBtn { "ACCOUNT..." }, closeBtn { "X" }, installBtn { "INSTALL AI ENGINE" }, cancelBtn { "CANCEL" },
        playBtn { "PLAY" }, backBtn { "-5s" }, fwdBtn { "+5s" }, offsetDown { "-" }, offsetUp { "+" },
        findLyricsBtn { "FIND LYRICS" };
    juce::ComboBox engineBox, stemsBox;
    juce::Slider positionSlider;
    std::unique_ptr<ResultList> list;
    std::unique_ptr<LyricsView> lyrics;
    juce::OwnedArray<StemRow> stemRows;
    std::unique_ptr<juce::Component> outputRow;
    std::unique_ptr<Worker> searchWorker, processWorker, accountWorker;

    std::vector<Item> results, library;
    bool showingLibrary = false;
    juce::File currentSong;
    juce::String nowTitle, nowSubtitle, statusText, bannerText;
    double busyPct = -1;
    bool busy = false;
    bool lyricsJob = false; // the running worker job only refreshes lyrics

    juce::Rectangle<int> topBar, listArea, playerArea, nowArea, transportArea, stemsArea, outputArea, lyricsArea,
        statusArea, bannerArea;
};
