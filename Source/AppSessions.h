#pragma once
#include <JuceHeader.h>
#include <map>
#include <mutex>

// Windows per-application audio sessions (the same thing the Windows volume mixer shows):
// one entry per app across all playback devices, with its volume, mute and live level.
// All Core Audio objects live on a background thread; the UI reads snapshots and posts changes.
class AppSessions : private juce::Thread
{
public:
    struct App
    {
        juce::String key;      // exe path (lower case), or "system"
        juce::String name;     // e.g. "Google Chrome"
        juce::String devices;  // playback devices it plays to
        juce::Image icon;
        float volume = 1.0f;   // 0..1 (Windows' per-app volume)
        bool mute = false;
        bool active = false;   // currently playing
        float peak = 0.0f;     // 0..1
    };

    AppSessions();
    ~AppSessions() override;

    void start();  // begins polling (call while the panel is visible)
    void stop();

    std::vector<App> snapshot() const;
    void setVolume (const juce::String& key, float volume);
    void setMute (const juce::String& key, bool mute);

private:
    void run() override;
    void rescan();
    void poll();
    void applyCommands();

    struct Session;
    std::vector<std::unique_ptr<Session>> sessions;   // worker thread only
    std::map<juce::String, App> info;                 // worker thread only (names, icons cache)

    mutable std::mutex lock;                          // guards published + commands
    std::vector<App> published;
    std::vector<std::tuple<juce::String, int, float>> commands; // key, 0 = volume / 1 = mute, value
};
