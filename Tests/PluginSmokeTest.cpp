// Loads real VST3 plugins through InsertChain and runs audio through them.
// Usage: PluginSmokeTest "C:\Program Files\Common Files\VST3\Some.vst3" [...]
#include <JuceHeader.h>
#include "../Source/InsertChain.h"
#include <cstdio>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui; // VST3 plugins expect a message thread
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager (formats);
    int failures = 0;

    for (int a = 1; a < argc; ++a)
    {
        const juce::String path = juce::String::fromUTF8 (argv[a]);
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : formats.getFormats())
            f->findAllTypesForFile (found, path);
        if (found.isEmpty())
        {
            std::printf ("[FAIL] %s: no plugin types found\n", path.toRawUTF8());
            ++failures;
            continue;
        }

        auto& desc = *found[0];
        juce::String err;
        auto inst = formats.createPluginInstance (desc, 48000.0, 64, err);
        if (inst == nullptr)
        {
            std::printf ("[FAIL] %s: %s\n", desc.name.toRawUTF8(), err.toRawUTF8());
            ++failures;
            continue;
        }

        InsertChain chain;
        chain.prepare (48000.0, 64);
        chain.swap (0, std::move (inst));
        auto* p = chain.get (0);

        // 2 s of a 440 Hz tone in 64-sample blocks, like the ASIO callback.
        std::vector<float> l (64), r (64);
        double peak = 0, phase = 0;
        bool finite = true;
        for (int block = 0; block < 1500; ++block)
        {
            for (int i = 0; i < 64; ++i, phase += 440.0 / 48000.0)
                l[(size_t) i] = r[(size_t) i] = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * phase);
            chain.process (l.data(), r.data(), 64);
            for (int i = 0; i < 64; ++i)
            {
                finite &= std::isfinite (l[(size_t) i]) && std::isfinite (r[(size_t) i]);
                peak = std::max (peak, (double) std::abs (l[(size_t) i]));
            }
        }

        // State round-trip through the same XML the app saves.
        auto xml = chain.toXml();
        const bool savedState = xml->getChildByName ("SLOT") != nullptr
                                && xml->getChildByName ("SLOT")->getStringAttribute ("state").isNotEmpty();

        const bool ok = finite && peak > 0.01 && savedState;
        std::printf ("[%s] %s (%s) in/out %d/%d, latency %d smp, output peak %.3f, state saved: %s\n",
                     ok ? "PASS" : "FAIL", desc.name.toRawUTF8(), desc.manufacturerName.toRawUTF8(),
                     p->getTotalNumInputChannels(), p->getTotalNumOutputChannels(), p->getLatencySamples(), peak,
                     savedState ? "yes" : "no");
        if (! ok) ++failures;
        chain.swap (0, nullptr);
    }
    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
