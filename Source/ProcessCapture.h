#pragma once
#include <JuceHeader.h>
#include <functional>

// Captures the audio of one app (a process and its child processes) with Windows'
// process-loopback capture (Windows 10 2004+ / 11). Delivers 48 kHz stereo float.
class ProcessCapture : private juce::Thread
{
public:
    static constexpr double sampleRate = 48000.0;

    ProcessCapture();
    ~ProcessCapture() override;

    // Called on the capture thread.
    std::function<void (const float* l, const float* r, int n)> onAudio;

    juce::String start (unsigned long processId); // error message or empty
    void stop();
    bool isCapturing() const noexcept { return isThreadRunning(); }
    unsigned long getProcessId() const noexcept { return pid; }

private:
    void run() override;

    unsigned long pid = 0;
    juce::WaitableEvent started;
    juce::String startError;
};
