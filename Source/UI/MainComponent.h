#pragma once
#include <JuceHeader.h>
#include "../Engine.h"
#include "Theme.h"

class StripComponent;
class BusComponent;
class FxReturnComponent;
class DetailPanel;
class PluginWindow;

class MainComponent : public juce::Component, private juce::Timer
{
public:
    explicit MainComponent (Engine&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    void rebuild();
    void openDetail (int stripIndex);
    void closeDetail();

    void openPluginEditor (juce::AudioPluginInstance*);
    void closePluginEditor (juce::AudioProcessor*);
    void openPluginManager();

private:
    void timerCallback() override;
    void openSettings();
    void loadPreset();
    void savePreset();

    Engine& engine;
    BvmLookAndFeel lnf;

    juce::TextButton setupButton { "AUDIO SETUP" }, autoButton { "AUTO-ASSIGN" },
        loadButton { "LOAD" }, saveButton { "SAVE" };
    juce::String statusText;

    juce::Viewport stripViewport;
    juce::Component stripHolder;
    juce::OwnedArray<StripComponent> stripComps;
    juce::OwnedArray<FxReturnComponent> fxComps;
    juce::OwnedArray<BusComponent> busComps;
    std::unique_ptr<DetailPanel> detail;
    int detailStrip = -1;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::OwnedArray<PluginWindow> pluginWindows;

    juce::Rectangle<int> headerArea, masterArea, fxArea;
    juce::TooltipWindow tooltips { this };
};
