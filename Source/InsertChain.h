#pragma once
#include <JuceHeader.h>
#include <atomic>

// A chain of VST3 plugin inserts on a strip or bus. Plugins are created and
// destroyed on the message thread; the audio thread only processes them, and skips
// the chain for one block if it is being edited at that moment (never blocks).
class InsertChain
{
public:
    static constexpr int numSlots = 4;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::atomic<bool> bypass { false };
    };

    // Device (re)start. Called before audio callbacks run.
    void prepare (double sampleRate, int maxBlock);

    // Audio thread. Stereo in place.
    void process (float* l, float* r, int n) noexcept;

    // Message thread. Returns the previous plugin so the caller can close its editor before deleting it.
    std::unique_ptr<juce::AudioPluginInstance> swap (int slot, std::unique_ptr<juce::AudioPluginInstance> p);

    juce::AudioPluginInstance* get (int slot) const noexcept { return slots[(size_t) slot].plugin.get(); }
    Slot& slot (int i) noexcept { return slots[(size_t) i]; }
    bool isEmpty() const noexcept;
    int getLatencySamples() const;

    static void configureStereo (juce::AudioPluginInstance&);

    std::unique_ptr<juce::XmlElement> toXml() const;

    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }

private:
    std::array<Slot, numSlots> slots;
    juce::SpinLock lock;
    juce::AudioBuffer<float> scratch { 16, 8192 };
    juce::MidiBuffer midi;
    double sampleRate = 48000.0;
    int blockSize = 512;
};
