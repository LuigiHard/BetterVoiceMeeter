#pragma once
#include <JuceHeader.h>
#include <map>
#include <mutex>
#include "Params.h"
#include "ExternalBridge.h"
#include "AppSessions.h"
#include "ProcessCapture.h"

// App Mixer modes B and C: routes apps into the mixer.
//
// A routed app's Windows output is pointed at a silent "sink" device and its audio is captured
// (process loopback) into the mixer, so it's heard once, through BetterVoiceMeeter:
//   Groups (C):  app -> Game / Chat / Music / Browser group -> "you" level (A1) + "stream" level (B1)
//   Per-app (B): app -> its own level and A1..B2 routing
// The captured copy is only mixed once Windows reports the app actually playing on the sink,
// so an app that hasn't moved yet is never heard twice. Previous output settings are restored
// when routing stops (and remembered on disk for apps that aren't running at that moment).
class AppRouter : private juce::Thread
{
public:
    enum class Mode { off = 0, groups = 1, perApp = 2 };
    static constexpr int numGroups = 4, maxSlots = 16, direct = -1;
    static const char* groupName (int g);

    struct GroupParams
    {
        std::atomic<float> youDb { 0.0f }, streamDb { -12.0f };
        std::atomic<bool> mute { false };
        Meter meter;
        juce::SmoothedValue<float> youGain, streamGain; // audio thread
    };

    struct AppSetting
    {
        int group = 0;        // groups mode; direct = not routed
        bool routed = true;   // per-app mode
        float gainDb = 0.0f;
        bool mute = false;
        int routes = 1;       // per-app mode: bit per bus (A1 = bit 0)
    };

    struct Status   // UI snapshot
    {
        juce::String key, name;
        juce::Image icon;
        AppSetting setting;
        bool routed = false, moved = false, playing = false;
        float peak = 0.0f;
        juce::String note;
    };

    AppRouter();
    ~AppRouter() override;

    // Message thread
    void setMode (Mode);
    Mode getMode() const noexcept { return (Mode) mode.load(); }
    void setSinkDevice (const juce::String& nameContains);
    juce::String getSinkDevice() const;
    static juce::StringArray candidateSinkDevices();
    std::vector<Status> snapshot() const;
    AppSetting getSetting (const juce::String& key) const;
    void setSetting (const juce::String& key, const AppSetting&);
    GroupParams& group (int g) noexcept { return groups[(size_t) g]; }
    void saveTo (juce::XmlElement&) const;
    void loadFrom (const juce::XmlElement&);
    void shutdown(); // restore every app's output (call before exit)

    // Audio thread: adds routed app audio into the bus buffers (stereo pairs per bus).
    void prepare (double sampleRate, int blockSize);
    void process (juce::AudioBuffer<float>& busBuf, int numBuses, int n) noexcept;

private:
    struct Slot
    {
        DriftFifo fifo;
        std::atomic<bool> active { false }, moved { false };
        std::atomic<int> group { 0 }, routes { 1 };
        std::atomic<float> gain { 1.0f };
        std::atomic<bool> mute { false }, resync { false };
        std::atomic<bool> draining { false };   // routing stopped, but the app's current sound is still on the sink
        std::atomic<juce::int64> keyHash { 0 }; // identifies the app for lock-free UI updates
        Meter meter;
        juce::SmoothedValue<float> smoothGain;
        // router thread only:
        juce::String key;
        std::unique_ptr<ProcessCapture> capture;
        unsigned long capturePid = 0;
        std::vector<unsigned long> routedPids;
    };

    void run() override;
    void step();
    bool wanted (const AppSessions::App&) const;
    void routeApp (Slot&, const AppSessions::App&);
    void releaseSlot (Slot&, bool appStillRunning);
    void restoreOutput (Slot&);
    void restorePending (const std::vector<AppSessions::App>&);
    unsigned long rootProcess (const AppSessions::App&) const;
    void saveRestoreFile() const;
    void loadRestoreFile();
    static juce::File restoreFile();

    AppSessions sessions;
    std::array<Slot, maxSlots> slots;
    std::array<GroupParams, numGroups> groups;
    std::atomic<int> mode { (int) Mode::off };
    std::atomic<double> engineRate { 48000.0 };
    std::atomic<int> engineBlock { 256 };

    mutable std::mutex lock;                         // guards the members below
    juce::String sinkName, sinkId;
    std::map<juce::String, AppSetting> settings;     // by app key (exe path)
    std::map<juce::String, juce::String> previousOutput; // app key -> endpoint id before routing ("" = default)
    std::vector<Status> published;

    juce::AudioBuffer<float> appBuf { 2, 8192 }, groupBuf { numGroups * 2, 8192 };
};
