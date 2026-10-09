#include "InsertChain.h"

void InsertChain::configureStereo (juce::AudioPluginInstance& p)
{
    p.disableNonMainBuses(); // drop sidechains etc.
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    if (! p.setBusesLayout (stereo))
    {
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        p.setBusesLayout (mono); // otherwise keep the plugin's default layout
    }
}

void InsertChain::prepare (double sr, int maxBlock)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    sampleRate = sr;
    blockSize = juce::jlimit (32, scratch.getNumSamples(), maxBlock);
    for (auto& s : slots)
        if (s.plugin != nullptr)
        {
            s.plugin->releaseResources();
            s.plugin->setRateAndBufferSizeDetails (sampleRate, blockSize);
            s.plugin->prepareToPlay (sampleRate, blockSize);
        }
}

void InsertChain::process (float* l, float* r, int n) noexcept
{
    if (n > blockSize)
        return;
    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked())
        return;

    for (auto& s : slots)
    {
        auto* p = s.plugin.get();
        if (p == nullptr || s.bypass.load (std::memory_order_relaxed) || p->isSuspended())
            continue;

        const int ins = p->getTotalNumInputChannels(), outs = p->getTotalNumOutputChannels();
        const int numCh = juce::jlimit (2, scratch.getNumChannels(), juce::jmax (ins, outs));
        juce::AudioBuffer<float> buf (scratch.getArrayOfWritePointers(), numCh, n);
        buf.copyFrom (0, 0, l, n);
        buf.copyFrom (1, 0, ins == 1 ? l : r, n);
        for (int c = 2; c < numCh; ++c)
            buf.clear (c, 0, n);
        if (ins == 1) // mono plugin: feed it the mix
        {
            buf.addFrom (0, 0, r, n);
            buf.applyGain (0, 0, n, 0.5f);
        }

        midi.clear();
        p->processBlock (buf, midi);

        juce::FloatVectorOperations::copy (l, buf.getReadPointer (0), n);
        juce::FloatVectorOperations::copy (r, buf.getReadPointer (outs >= 2 ? 1 : 0), n);
    }
}

std::unique_ptr<juce::AudioPluginInstance> InsertChain::swap (int i, std::unique_ptr<juce::AudioPluginInstance> p)
{
    if (p != nullptr)
    {
        configureStereo (*p);
        p->setRateAndBufferSizeDetails (sampleRate, blockSize);
        p->prepareToPlay (sampleRate, blockSize);
    }
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::swap (slots[(size_t) i].plugin, p);
        slots[(size_t) i].bypass = false;
    }
    if (p != nullptr)
        p->releaseResources();
    return p;
}

bool InsertChain::isEmpty() const noexcept
{
    return std::none_of (slots.begin(), slots.end(), [] (const Slot& s) { return s.plugin != nullptr; });
}

int InsertChain::getLatencySamples() const
{
    int total = 0;
    for (auto& s : slots)
        if (s.plugin != nullptr && ! s.bypass.load())
            total += s.plugin->getLatencySamples();
    return total;
}

std::unique_ptr<juce::XmlElement> InsertChain::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("INSERTS");
    for (int i = 0; i < numSlots; ++i)
    {
        auto* p = slots[(size_t) i].plugin.get();
        if (p == nullptr) continue;
        auto* e = xml->createNewChildElement ("SLOT");
        e->setAttribute ("index", i);
        e->setAttribute ("bypass", slots[(size_t) i].bypass.load());
        e->addChildElement (p->getPluginDescription().createXml().release());
        juce::MemoryBlock state;
        p->getStateInformation (state);
        e->setAttribute ("state", state.toBase64Encoding());
    }
    return xml;
}
