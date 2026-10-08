#pragma once
#include <JuceHeader.h>
#include <atomic>

// Stereo lock-free FIFO between two audio clocks (e.g. the Focusrite ASIO device and a
// WASAPI device such as VB-Cable). The reading side resamples adaptively so the fill
// level stays near a target, compensating for sample-rate mismatch and clock drift.
class DriftFifo
{
public:
    DriftFifo();

    void reset (double writerRate, double readerRate, int writerBlock, int readerBlock);

    // Producer side. Drops samples if the FIFO is full.
    void push (const float* l, const float* r, int n) noexcept;

    // Consumer side. Fills silence (and re-primes) on underrun.
    void pull (float* l, float* r, int n) noexcept;

    int getFill() const noexcept { return fifo.getNumReady(); }

private:
    static constexpr int capacity = 32768;
    juce::AbstractFifo fifo { capacity };
    juce::AudioBuffer<float> storage { 2, capacity };
    juce::AudioBuffer<float> scratch { 2, capacity };
    juce::LagrangeInterpolator interpL, interpR;

    double baseRatio = 1.0, smoothedFill = 0.0;
    int target = 2048;
    bool primed = false;
};

// A secondary (non-ASIO) device opened through WASAPI shared mode. Used for:
//  - output bridges: virtual buses B1/B2 feed apps like Discord/OBS through VB-Cable
//  - input bridges: virtual input strips capture any Windows recording device
class ExternalBridge : private juce::AudioIODeviceCallback
{
public:
    enum class Direction { output, input };

    explicit ExternalBridge (Direction d);
    ~ExternalBridge() override;

    static juce::StringArray getDeviceNames (Direction d);

    // Message thread. Empty name closes the device.
    juce::String open (const juce::String& deviceName, double engineRate, int engineBlock);
    void close();

    juce::String getDeviceName() const { return currentName; }
    bool isRunning() const noexcept { return running.load(); }
    double getDeviceRate() const noexcept { return deviceRate; }

    // Called from the main (ASIO) audio thread.
    void engineWrite (const float* l, const float* r, int n) noexcept;  // output bridges
    void engineRead (float* l, float* r, int n) noexcept;               // input bridges

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    Direction direction;
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    juce::String currentName;
    double deviceRate = 48000;
    DriftFifo fifo;
    std::atomic<bool> running { false };
    juce::SpinLock engineLock; // guards engine-side access while (re)configuring
};
