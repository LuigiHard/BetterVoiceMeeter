#include "ExternalBridge.h"

//==============================================================================
DriftFifo::DriftFifo()
{
    storage.clear();
    scratch.clear();
}

void DriftFifo::reset (double writerRate, double readerRate, int writerBlock, int readerBlock)
{
    fifo.reset();
    interpL.reset();
    interpR.reset();
    baseRatio = writerRate / readerRate;
    // Enough headroom to absorb both devices' block sizes plus scheduling jitter.
    target = juce::jlimit (256, capacity / 4, 2 * juce::jmax (writerBlock, readerBlock) + 256);
    smoothedFill = target;
    primed = false;
}

void DriftFifo::push (const float* l, const float* r, int n) noexcept
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    if (n1 > 0)
    {
        storage.copyFrom (0, s1, l, n1);
        storage.copyFrom (1, s1, r, n1);
    }
    if (n2 > 0)
    {
        storage.copyFrom (0, s2, l + n1, n2);
        storage.copyFrom (1, s2, r + n1, n2);
    }
    fifo.finishedWrite (n1 + n2);
}

void DriftFifo::pull (float* l, float* r, int n) noexcept
{
    auto silence = [&]
    {
        juce::FloatVectorOperations::clear (l, n);
        juce::FloatVectorOperations::clear (r, n);
    };

    int avail = fifo.getNumReady();

    if (! primed)
    {
        if (avail < target) { silence(); return; }
        primed = true;
        smoothedFill = avail;
    }

    // Way too much latency built up (e.g. the reader stalled): skip ahead.
    if (avail > target * 4)
    {
        fifo.finishedRead (avail - target);
        avail = target;
        interpL.reset();
        interpR.reset();
    }

    smoothedFill = smoothedFill * 0.99 + avail * 0.01;
    const double err = juce::jlimit (-1.0, 1.0, (smoothedFill - target) / (double) target);
    const double ratio = baseRatio * (1.0 + err * 0.005); // max ±0.5% speed correction

    const int needed = (int) std::ceil (ratio * n) + 4;
    if (needed > avail || needed > capacity)
    {
        primed = false; // underrun: re-prime to target
        silence();
        return;
    }

    int s1, n1, s2, n2;
    fifo.prepareToRead (needed, s1, n1, s2, n2);
    for (int ch = 0; ch < 2; ++ch)
    {
        scratch.copyFrom (ch, 0, storage, ch, s1, n1);
        if (n2 > 0)
            scratch.copyFrom (ch, n1, storage, ch, s2, n2);
    }

    const int used = interpL.process (ratio, scratch.getReadPointer (0), l, n);
    interpR.process (ratio, scratch.getReadPointer (1), r, n);
    fifo.finishedRead (juce::jmin (used, n1 + n2));
}

//==============================================================================
ExternalBridge::ExternalBridge (Direction d) : direction (d) {}

ExternalBridge::~ExternalBridge() { close(); }

juce::StringArray ExternalBridge::getDeviceNames (Direction d)
{
    std::unique_ptr<juce::AudioIODeviceType> t (
        juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    if (t == nullptr)
        return {};
    t->scanForDevices();
    return t->getDeviceNames (d == Direction::input);
}

juce::String ExternalBridge::open (const juce::String& deviceName, double engineRate, int engineBlock)
{
    close();
    if (deviceName.isEmpty())
        return {};

    if (type == nullptr)
        type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    if (type == nullptr)
        return "WASAPI unavailable";

    type->scanForDevices();
    device.reset (direction == Direction::output ? type->createDevice (deviceName, {})
                                                 : type->createDevice ({}, deviceName));
    if (device == nullptr)
        return "Could not create device: " + deviceName;

    auto rates = device->getAvailableSampleRates();
    double rate = rates.contains (engineRate) ? engineRate : (rates.isEmpty() ? 48000.0 : rates[0]);

    const int numCh = juce::jmin (2, direction == Direction::output ? device->getOutputChannelNames().size()
                                                                   : device->getInputChannelNames().size());
    juce::BigInteger chans;
    chans.setRange (0, juce::jmax (1, numCh), true);

    auto err = device->open (direction == Direction::input ? chans : juce::BigInteger(),
                             direction == Direction::output ? chans : juce::BigInteger(),
                             rate, device->getDefaultBufferSize());
    if (err.isNotEmpty())
    {
        device.reset();
        return err;
    }

    deviceRate = device->getCurrentSampleRate();
    const int devBlock = device->getCurrentBufferSizeSamples();

    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        if (direction == Direction::output)
            fifo.reset (engineRate, deviceRate, engineBlock, devBlock);
        else
            fifo.reset (deviceRate, engineRate, devBlock, engineBlock);
    }

    currentName = deviceName;
    running = true;
    device->start (this);
    return {};
}

void ExternalBridge::close()
{
    running = false;
    if (device != nullptr)
    {
        device->stop();
        device->close();
        device.reset();
    }
    currentName.clear();
}

void ExternalBridge::engineWrite (const float* l, const float* r, int n) noexcept
{
    if (! running.load (std::memory_order_relaxed))
        return;
    const juce::SpinLock::ScopedTryLockType tl (engineLock);
    if (tl.isLocked())
        fifo.push (l, r, n);
}

void ExternalBridge::engineRead (float* l, float* r, int n) noexcept
{
    if (running.load (std::memory_order_relaxed))
    {
        const juce::SpinLock::ScopedTryLockType tl (engineLock);
        if (tl.isLocked())
        {
            fifo.pull (l, r, n);
            return;
        }
    }
    juce::FloatVectorOperations::clear (l, n);
    juce::FloatVectorOperations::clear (r, n);
}

void ExternalBridge::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out,
                                                       int numOut, int numSamples,
                                                       const juce::AudioIODeviceCallbackContext&)
{
    if (direction == Direction::output)
    {
        if (numOut >= 2)
            fifo.pull (out[0], out[1], numSamples);
        else if (numOut == 1)
        {
            float tmp[4096];
            const int n = juce::jmin (numSamples, 4096);
            fifo.pull (out[0], tmp, n);
            juce::FloatVectorOperations::add (out[0], tmp, n);
            juce::FloatVectorOperations::multiply (out[0], 0.5f, n);
        }
        for (int ch = 2; ch < numOut; ++ch)
            juce::FloatVectorOperations::clear (out[ch], numSamples);
    }
    else
    {
        if (numIn >= 2)
            fifo.push (in[0], in[1], numSamples);
        else if (numIn == 1)
            fifo.push (in[0], in[0], numSamples);
        for (int ch = 0; ch < numOut; ++ch)
            juce::FloatVectorOperations::clear (out[ch], numSamples);
    }
}
