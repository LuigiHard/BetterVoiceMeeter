#pragma once
#include <JuceHeader.h>
#include "Params.h"
#include "DSP.h"
#include "ExternalBridge.h"

// The mixing engine: runs inside the ASIO callback of the Focusrite (or whichever
// device is selected), so hardware in -> processing -> hardware out costs exactly one
// driver buffer of latency, with no extra buffering.
class Engine : private juce::AudioIODeviceCallback,
               private juce::ChangeListener,
               private juce::AsyncUpdater
{
public:
    Engine();
    ~Engine() override;

    juce::AudioDeviceManager deviceManager;

    std::array<std::unique_ptr<StripParams>, kNumStrips> strips;
    std::array<std::unique_ptr<BusParams>, kNumBuses> buses;
    std::array<std::unique_ptr<FxParams>, kNumFx> fx;

    std::array<Meter, kNumStrips> stripMeters, stripInputMeters;
    std::array<Meter, kNumBuses> busMeters;
    std::array<Meter, kNumFx> fxMeters;

    // Virtual I/O (WASAPI). Index 0/1 = B1/B2 outputs, V1/V2 inputs.
    std::array<std::unique_ptr<ExternalBridge>, kNumVirtBuses> outBridges;
    std::array<std::unique_ptr<ExternalBridge>, kNumVirtStrips> inBridges;
    std::array<juce::String, kNumVirtBuses> outBridgeNames;
    std::array<juce::String, kNumVirtStrips> inBridgeNames;

    void setOutBridgeDevice (int index, const juce::String& name);
    void setInBridgeDevice (int index, const juce::String& name);

    juce::StringArray getInputChannelNames() const;
    juce::StringArray getOutputChannelNames() const;
    juce::String describeSource (int stripIndex) const;
    void autoAssignStrips();

    double getSampleRate() const noexcept { return sampleRate.load(); }
    int getBlockSize() const noexcept { return blockSize.load(); }
    double getCpuUsage() const { return deviceManager.getCpuUsage(); }
    juce::String getLatencyText() const;

    void saveState();
    void loadPreset (const juce::File&);
    void savePreset (const juce::File&);

    std::function<void()> onLayoutChanged; // strips activated/renamed, device changed

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void handleAsyncUpdate() override;

    void enableAllChannels();
    void reopenBridges();
    std::unique_ptr<juce::XmlElement> createStateXml (bool includeDevice) const;
    void restoreStateXml (const juce::XmlElement&, bool includeDevice);
    static juce::File getSettingsFile();

    struct StripState
    {
        bvm::Biquad hpf[2], eqLow[2], eqMid[2], eqHigh[2], tiltLow[2], tiltHigh[2], deessHp[2];
        bvm::Gate gate;
        bvm::Compressor comp;
        float deessEnv = 0, deessGain = 1;
        juce::SmoothedValue<float> trim, fader, panL, panR, sendRev, sendDly;
        // Cached values used to detect when filter coefficients need recomputing.
        float cHpf = -1, cLowF = -1, cLowG = -99, cMidF = -1, cMidG = -99, cMidQ = -1, cHighF = -1, cHighG = -99, cTone = -99;
        void resetDsp();
    };

    struct BusState
    {
        bvm::Biquad low[2], mid[2], high[2];
        bvm::Limiter limiter;
        juce::SmoothedValue<float> gain;
        float cLow = -99, cMid = -99, cHigh = -99;
    };

    void updateStripCoefficients (int s, StripState&);
    void processStrip (int s, StripState&, float* l, float* r, int n, bool anySolo);

    std::array<StripState, kNumStrips> stripState;
    std::array<BusState, kNumBuses> busState;
    juce::Reverb reverb;
    bvm::StereoDelay delay;
    std::array<juce::SmoothedValue<float>, kNumFx> fxGain;

    juce::AudioBuffer<float> stripBuf, busBuf, fxBuf;
    std::vector<int> inputMap, outputMap; // physical channel -> callback channel index (-1 if inactive)

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> blockSize { 256 };
    double bridgeRate = 0;
    int bridgeBlock = 0;
    bool enforcingChannels = false;
    bool firstRun = false;
};
