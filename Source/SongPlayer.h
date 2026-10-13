#pragma once
#include <JuceHeader.h>
#include <atomic>

// Plays a separated song (lead vocal, backing vocals, drums, bass, ...) inside the mixer.
// Stems are held in memory as 16-bit at their source rate, mixed with per-stem
// volume / mute / solo, then resampled to the device rate.
class SongPlayer
{
public:
    static constexpr int maxStems = 6;

    struct StemControl
    {
        std::atomic<float> volume { 1.0f }; // 0..1.5 (100% = original)
        std::atomic<bool> mute { false }, solo { false };
        std::atomic<float> peak { 0.0f };    // meter, written by the audio thread
    };

    SongPlayer();

    // Message thread. Loads <dir>/<stem>.wav for each name. Returns an error or empty.
    juce::String load (const juce::File& songDir, const juce::StringArray& stemNames);
    void unload();

    void prepare (double deviceRate);

    // Audio thread: renders the mix into l/r (overwrites).
    void render (float* l, float* r, int n) noexcept;

    // Transport (any thread).
    void setPlaying (bool shouldPlay) noexcept;
    bool isPlaying() const noexcept { return playing.load(); }
    void seek (double seconds) noexcept;
    double getPosition() const noexcept;  // seconds
    double getLength() const noexcept { return lengthSeconds.load(); }
    bool isLoaded() const noexcept { return lengthSeconds.load() > 0.0; }

    juce::StringArray getStemNames() const { return stemNames; } // message thread
    StemControl& stem (int i) noexcept { return controls[(size_t) i]; }

private:
    struct SongData
    {
        double sampleRate = 44100.0;
        int64_t length = 0;
        std::vector<std::vector<int16_t>> stems; // interleaved stereo
    };

    std::unique_ptr<SongData> data;
    juce::SpinLock lock;
    juce::StringArray stemNames;
    std::array<StemControl, maxStems> controls;
    std::array<float, maxStems> currentGain {};

    std::atomic<bool> playing { false };
    std::atomic<int64_t> position { 0 };     // source samples
    std::atomic<int64_t> seekRequest { -1 };
    std::atomic<double> lengthSeconds { 0.0 }, sourceRate { 44100.0 };
    double deviceRate = 48000.0;

    juce::AudioBuffer<float> scratch { 2, 32768 };
    juce::LagrangeInterpolator interpL, interpR;
};
