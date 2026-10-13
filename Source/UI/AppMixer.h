#pragma once
#include <JuceHeader.h>
#include "../AppSessions.h"

// Per-app volume panel: one strip per app playing audio on this PC (Windows per-app volume).
class AppMixer : public juce::Component, private juce::Timer
{
public:
    explicit AppMixer (std::function<void()> onClose);
    ~AppMixer() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

    class Strip;

private:
    void timerCallback() override;

    AppSessions sessions;
    std::function<void()> onClose;
    juce::TextButton closeBtn { "X" };
    juce::Viewport viewport;
    juce::Component holder;
    juce::OwnedArray<Strip> strips;
    juce::Rectangle<int> topBar;
};
