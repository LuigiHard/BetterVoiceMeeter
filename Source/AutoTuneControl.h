#pragma once
#include <JuceHeader.h>

// Drives an Antares Auto-Tune instance hosted as a VST3 insert (the user's installed copy).
namespace AutoTuneControl
{
inline bool isAutoTune (const juce::AudioPluginInstance* p)
{
    if (p == nullptr) return false;
    const auto n = p->getName();
    return (n.containsIgnoreCase ("Auto-Tune") || n.containsIgnoreCase ("AutoTune")) && ! n.containsIgnoreCase ("Auto-Key");
}

// Sets a choice parameter by its displayed text (e.g. Key = "F#", Scale = "Minor").
inline bool setParamByText (juce::AudioPluginInstance& p, const juce::String& paramName, const juce::String& text)
{
    for (auto* prm : p.getParameters())
    {
        if (! prm->getName (64).equalsIgnoreCase (paramName)) continue;
        const int steps = prm->getNumSteps() > 1 && prm->getNumSteps() < 1000 ? prm->getNumSteps() : 128;
        for (int i = 0; i < steps; ++i)
        {
            const float v = (float) i / (float) (steps - 1);
            if (prm->getText (v, 64).trim().equalsIgnoreCase (text))
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (v);
                prm->endChangeGesture();
                return true;
            }
        }
    }
    return false;
}

inline juce::String paramText (juce::AudioPluginInstance& p, const juce::String& paramName)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (64).equalsIgnoreCase (paramName)) return prm->getCurrentValueAsText();
    return {};
}

// Key + scale for the song, plus Auto-Tune Pro's low-latency mode. Returns true if key and scale were set.
inline bool applyKey (juce::AudioPluginInstance& p, const juce::String& key, const juce::String& scale)
{
    const bool k = setParamByText (p, "Key", key);
    const bool s = setParamByText (p, "Scale", scale);
    setParamByText (p, "Latency Removal", "on");
    setParamByText (p, "Correction Mode", "Auto mode");
    return k && s;
}
} // namespace AutoTuneControl
