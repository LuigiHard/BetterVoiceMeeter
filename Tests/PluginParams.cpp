// Lists a VST3 plugin's parameters with sample value texts: PluginParams <path.vst3> [name filter]
#include <JuceHeader.h>
#include <cstdio>

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: PluginParams <path.vst3> [filter]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : formats.getFormats()) f->findAllTypesForFile (found, juce::String::fromUTF8 (argv[1]));
    if (found.isEmpty()) { std::printf ("no plugin found\n"); return 1; }
    juce::String err;
    auto p = formats.createPluginInstance (*found[0], 48000.0, 64, err);
    if (p == nullptr) { std::printf ("load failed: %s\n", err.toRawUTF8()); return 1; }
    const juce::String filter = argc > 2 ? juce::String::fromUTF8 (argv[2]) : juce::String();
    std::printf ("%s by %s, latency %d samples, %d params\n", found[0]->name.toRawUTF8(), found[0]->manufacturerName.toRawUTF8(),
                 p->getLatencySamples(), p->getParameters().size());
    for (auto* prm : p->getParameters())
    {
        const auto name = prm->getName (64);
        if (filter.isNotEmpty() && ! name.containsIgnoreCase (filter)) continue;
        juce::StringArray texts;
        const int steps = juce::jlimit (2, 24, prm->getNumSteps() > 0 && prm->getNumSteps() < 100 ? prm->getNumSteps() : 6);
        for (int i = 0; i < steps; ++i) texts.add (prm->getText ((float) i / (float) (steps - 1), 32));
        std::printf ("  [%d] %-28s steps=%-6d now='%s'  values: %s\n", prm->getParameterIndex(), name.toRawUTF8(), prm->getNumSteps(),
                     prm->getCurrentValueAsText().toRawUTF8(), texts.joinIntoString (" | ").toRawUTF8());
    }
    return 0;
}
