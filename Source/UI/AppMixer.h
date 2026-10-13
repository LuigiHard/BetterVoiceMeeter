#pragma once
#include <JuceHeader.h>
#include "../AppSessions.h"

class Engine;

// App Mixer panel with three modes:
//   VOLUME   (A) - Windows per-app volume, mute and meters; audio is not rerouted
//   GROUPS   (C) - apps routed into Game / Chat / Music / Browser groups, each with a "you" and a "stream" level
//   PER-APP  (B) - each routed app gets its own level and A1..B2 routing
class AppMixer : public juce::Component, private juce::Timer
{
public:
    AppMixer (Engine&, std::function<void()> onClose);
    ~AppMixer() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

    class Strip;       // volume mode
    class GroupCard;   // groups mode
    class AssignRow;   // groups mode
    class RouteStrip;  // per-app mode

private:
    void timerCallback() override;
    void setMode (int newMode);
    void rebuildContent();
    void layoutContent();
    juce::String contentSignature() const;

    Engine& engine;
    AppSessions sessions;
    std::function<void()> onClose;
    juce::TextButton closeBtn { "X" }, volumeBtn { "VOLUME" }, groupsBtn { "GROUPS" }, perAppBtn { "PER-APP" };
    juce::Label sinkLabel;
    juce::ComboBox sinkBox;
    juce::Viewport viewport;
    juce::Component holder;
    juce::OwnedArray<juce::Component> items;
    juce::OwnedArray<juce::Component> rows;
    juce::String signature;
    int mode = 0;
    juce::Rectangle<int> topBar, modeBar, helpArea;
};
