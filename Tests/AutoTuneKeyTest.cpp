// Loads the installed Auto-Tune, applies song keys through AutoTuneControl and reads them back.
#include <JuceHeader.h>
#include "../Source/AutoTuneControl.h"
#include <cstdio>

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: AutoTuneKeyTest <Auto-Tune.vst3>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : formats.getFormats()) f->findAllTypesForFile (found, juce::String::fromUTF8 (argv[1]));
    juce::String err;
    auto p = found.isEmpty() ? nullptr : formats.createPluginInstance (*found[0], 48000.0, 64, err);
    if (p == nullptr) { std::printf ("load failed\n"); return 1; }
    std::printf ("%s detected as Auto-Tune: %d\n", p->getName().toRawUTF8(), (int) AutoTuneControl::isAutoTune (p.get()));
    int failures = 0;
    for (auto [key, scale] : { std::pair<const char*, const char*> { "F", "minor" }, { "E", "minor" }, { "F#", "major" }, { "Bb", "minor" } })
    {
        const bool ok = AutoTuneControl::applyKey (*p, key, scale);
        const auto k = AutoTuneControl::paramText (*p, "Key"), s = AutoTuneControl::paramText (*p, "Scale");
        const auto lat = AutoTuneControl::paramText (*p, "Latency Removal");
        const bool match = k == key && s.equalsIgnoreCase (scale);
        std::printf ("  set %-3s %-5s -> plugin shows Key '%s' Scale '%s'%s  [%s]\n", key, scale, k.toRawUTF8(), s.toRawUTF8(),
                     lat.isNotEmpty() ? (", Latency Removal '" + lat + "'").toRawUTF8() : "", ok && match ? "OK" : "FAIL");
        failures += ok && match ? 0 : 1;
    }
    std::printf ("latency reported: %d samples\n%s\n", p->getLatencySamples(), failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
