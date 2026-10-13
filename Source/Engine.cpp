#include "Engine.h"

namespace
{
constexpr int maxBlock = 8192;
}

//==============================================================================
void Engine::StripState::resetDsp()
{
    for (int c = 0; c < 2; ++c)
        for (auto* b : { &hpf[c], &eqLow[c], &eqMid[c], &eqHigh[c], &tiltLow[c], &tiltHigh[c], &deessHp[c] })
            b->reset();
    gate.reset();
    comp.reset();
    deessEnv = 0;
    deessGain = 1;
    declick.reset();
    hum.reset();
    denoise.reset();
}

Engine::Engine()
{
    for (int i = 0; i < kNumStrips; ++i)
    {
        strips[(size_t) i] = std::make_unique<StripParams> (i);
        if (i >= kNumHwStrips)
        {
            strips[(size_t) i]->active->set (1.0f);
            strips[(size_t) i]->stereo->set (1.0f);
            strips[(size_t) i]->label = "Virtual " + juce::String (i - kNumHwStrips + 1);
        }
    }
    for (int i = 0; i < kNumBuses; ++i)  buses[(size_t) i] = std::make_unique<BusParams> (i);
    for (int i = 0; i < kNumFx; ++i)     fx[(size_t) i] = std::make_unique<FxParams> (i);
    for (auto& b : outBridges) b = std::make_unique<ExternalBridge> (ExternalBridge::Direction::output);
    for (auto& b : inBridges)  b = std::make_unique<ExternalBridge> (ExternalBridge::Direction::input);

    juce::addDefaultFormatsToManager (formatManager);
    if (auto pluginXml = juce::XmlDocument::parse (getDataDir().getChildFile ("plugins.xml")))
        knownPlugins.recreateFromXml (*pluginXml);
    knownPlugins.addChangeListener (this);

    stripBuf.setSize (2, maxBlock);
    busBuf.setSize (kNumBuses * 2, maxBlock);
    fxBuf.setSize (kNumFx * 2, maxBlock);

    std::unique_ptr<juce::XmlElement> saved;
    if (getSettingsFile().existsAsFile())
        saved = juce::XmlDocument::parse (getSettingsFile());

    const juce::XmlElement* deviceXml = saved != nullptr ? saved->getChildByName ("DEVICESETUP") : nullptr;
    auto logErr = [] (const juce::String& what, const juce::String& err)
    {
        if (err.isNotEmpty()) juce::Logger::writeToLog (what + ": " + err);
    };
    logErr ("initialise", deviceManager.initialise (64, 64, deviceXml, true));

    if (deviceXml == nullptr)
    {
        // First run: prefer the Focusrite ASIO driver.
        firstRun = true;
        deviceManager.setCurrentAudioDeviceType ("ASIO", true);
        if (auto* type = deviceManager.getCurrentDeviceTypeObject())
        {
            type->scanForDevices();
            juce::Logger::writeToLog ("ASIO drivers: " + type->getDeviceNames().joinIntoString (", "));
            for (auto& name : type->getDeviceNames())
            {
                if (name.containsIgnoreCase ("Focusrite") && ! name.containsIgnoreCase ("Thunderbolt"))
                {
                    auto setup = deviceManager.getAudioDeviceSetup();
                    setup.inputDeviceName = setup.outputDeviceName = name;
                    logErr ("open " + name, deviceManager.setAudioDeviceSetup (setup, true));

                    // Low latency defaults: 48 kHz (matches Discord/OBS/VB-Cable) and the
                    // smallest buffer >= 64 samples the driver offers.
                    if (auto* d = deviceManager.getCurrentAudioDevice())
                    {
                        setup = deviceManager.getAudioDeviceSetup();
                        if (d->getAvailableSampleRates().contains (48000.0))
                            setup.sampleRate = 48000.0;
                        for (int bs : d->getAvailableBufferSizes())
                            if (bs >= 64) { setup.bufferSize = bs; break; }
                        logErr ("low-latency setup", deviceManager.setAudioDeviceSetup (setup, true));
                    }
                    break;
                }
            }
        }
        // Default B1 to VB-Cable if installed, so Discord/OBS can pick up "CABLE Output".
        for (auto& n : ExternalBridge::getDeviceNames (ExternalBridge::Direction::output))
            if (n.containsIgnoreCase ("CABLE Input"))
            {
                outBridgeNames[0] = n;
                break;
            }
    }

    if (saved != nullptr)
        restoreStateXml (*saved, false);

    if (auto* d = deviceManager.getCurrentAudioDevice())
        juce::Logger::writeToLog ("Opened: " + d->getTypeName() + " / " + d->getName() + " @ "
                                  + juce::String (d->getCurrentSampleRate()) + " Hz, "
                                  + juce::String (d->getCurrentBufferSizeSamples()) + " samples, "
                                  + juce::String (d->getInputChannelNames().size()) + " in / "
                                  + juce::String (d->getOutputChannelNames().size()) + " out");
    else
        juce::Logger::writeToLog ("No audio device open");

    enableAllChannels();
    if (firstRun)
        autoAssignStrips();

    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (this);
    triggerAsyncUpdate();
}

Engine::~Engine()
{
    saveState();
    deviceManager.removeAudioCallback (this);
    deviceManager.removeChangeListener (this);
    knownPlugins.removeChangeListener (this);
    for (auto& b : outBridges) b->close();
    for (auto& b : inBridges)  b->close();
    deviceManager.closeAudioDevice();
}

//==============================================================================
juce::StringArray Engine::getInputChannelNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getInputChannelNames();
    return {};
}

juce::StringArray Engine::getOutputChannelNames() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
        return d->getOutputChannelNames();
    return {};
}

juce::String Engine::describeSource (int s) const
{
    auto& p = *strips[(size_t) s];
    if (s >= kNumHwStrips)
    {
        auto& n = inBridgeNames[(size_t) (s - kNumHwStrips)];
        return n.isEmpty() ? juce::String ("(no device)") : n;
    }
    const auto names = getInputChannelNames();
    const int l = (int) p.sourceL->get(), r = (int) p.sourceR->get();
    auto nameOf = [&] (int i) { return juce::isPositiveAndBelow (i, names.size()) ? names[i] : "In " + juce::String (i + 1); };
    if (l < 0) return "(none)";
    return p.stereo->on() && r >= 0 ? nameOf (l) + " + " + nameOf (r) : nameOf (l);
}

void Engine::autoAssignStrips()
{
    const auto names = getInputChannelNames();
    int strip = 0;
    for (int ch = 0; ch < names.size() && strip < kNumHwStrips; ++ch)
    {
        auto& p = *strips[(size_t) strip];
        p.resetAll();
        p.active->set (1.0f);
        p.sourceL->set ((float) ch);

        // Focusrite "Loopback" channels (Windows playback audio) come in pairs: make them one stereo strip.
        const bool pairable = names[ch].containsIgnoreCase ("Loopback") && ch + 1 < names.size()
                              && names[ch + 1].containsIgnoreCase ("Loopback");
        if (pairable)
        {
            p.stereo->set (1.0f);
            p.sourceR->set ((float) (ch + 1));
            p.label = "Loopback";
            // Windows playback already reaches the monitors directly; sending it to A1 again
            // would double it (or feed back), so by default it only goes to the stream bus.
            p.routes[0]->set (0.0f);
            p.routes[(size_t) kNumHwBuses]->set (1.0f);
            ++ch;
        }
        else
        {
            p.label = names[ch].replace ("Analogue", "In").replace ("Analog", "In");
            p.routes[(size_t) kNumHwBuses]->set (1.0f); // mics/instruments: monitors (A1) + stream (B1)
        }
        ++strip;
    }
    for (; strip < kNumHwStrips; ++strip)
    {
        strips[(size_t) strip]->resetAll();
        strips[(size_t) strip]->label = "Strip " + juce::String (strip + 1);
    }
    if (onLayoutChanged) onLayoutChanged();
}

juce::String Engine::getLatencyText() const
{
    if (auto* d = deviceManager.getCurrentAudioDevice())
    {
        const double sr = d->getCurrentSampleRate();
        const int bs = d->getCurrentBufferSizeSamples();
        const int total = bs + d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
        return juce::String (sr / 1000.0, 1) + " kHz | " + juce::String (bs) + " smp | RTL "
               + juce::String (1000.0 * total / sr, 1) + " ms";
    }
    return "No device";
}

void Engine::setOutBridgeDevice (int i, const juce::String& name)
{
    outBridgeNames[(size_t) i] = name;
    auto err = outBridges[(size_t) i]->open (name, sampleRate.load(), blockSize.load());
    if (err.isNotEmpty())
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Virtual output", err);
}

void Engine::setInBridgeDevice (int i, const juce::String& name)
{
    inBridgeNames[(size_t) i] = name;
    auto err = inBridges[(size_t) i]->open (name, sampleRate.load(), blockSize.load());
    if (err.isNotEmpty())
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Virtual input", err);
    if (onLayoutChanged) onLayoutChanged();
}

void Engine::reopenBridges()
{
    bridgeRate = sampleRate.load();
    bridgeBlock = blockSize.load();
    for (int i = 0; i < kNumVirtBuses; ++i)
        outBridges[(size_t) i]->open (outBridgeNames[(size_t) i], bridgeRate, bridgeBlock);
    for (int i = 0; i < kNumVirtStrips; ++i)
        inBridges[(size_t) i]->open (inBridgeNames[(size_t) i], bridgeRate, bridgeBlock);
}

template <typename Bridges, typename Names>
static bool needsReopen (const Bridges& bridges, const Names& names)
{
    for (size_t i = 0; i < bridges.size(); ++i)
        if (names[i].isNotEmpty() && ! bridges[i]->isRunning())
            return true;
    return false;
}

void Engine::handleAsyncUpdate()
{
    if (bridgeRate != sampleRate.load() || bridgeBlock != blockSize.load()
        || needsReopen (outBridges, outBridgeNames) || needsReopen (inBridges, inBridgeNames))
        reopenBridges();

    // Only rebuild the UI when the interface's channel layout actually changed.
    auto layoutKey = getInputChannelNames().joinIntoString ("|") + "#" + getOutputChannelNames().joinIntoString ("|");
    if (auto* d = deviceManager.getCurrentAudioDevice())
        layoutKey += "#" + d->getName();
    if (layoutKey != lastLayoutKey)
    {
        lastLayoutKey = layoutKey;
        if (onLayoutChanged) onLayoutChanged();
    }
}

// Make sure every input/output channel of the interface is active, so strips can use any of them.
void Engine::enableAllChannels()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (d == nullptr || enforcingChannels)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();
    const int nIn = d->getInputChannelNames().size(), nOut = d->getOutputChannelNames().size();
    if (setup.inputChannels.countNumberOfSetBits() == nIn && setup.outputChannels.countNumberOfSetBits() == nOut)
        return;

    enforcingChannels = true;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (0, nIn, true);
    setup.outputChannels.clear();
    setup.outputChannels.setRange (0, nOut, true);
    deviceManager.setAudioDeviceSetup (setup, true);
    enforcingChannels = false;
}

void Engine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &knownPlugins)
    {
        if (auto xml = knownPlugins.createXml())
        {
            getDataDir().createDirectory();
            xml->writeTo (getDataDir().getChildFile ("plugins.xml"));
        }
        return;
    }
    enableAllChannels();
    triggerAsyncUpdate();
}

//==============================================================================
void Engine::audioDeviceAboutToStart (juce::AudioIODevice* d)
{
    const double sr = d->getCurrentSampleRate();
    sampleRate = sr;
    blockSize = d->getCurrentBufferSizeSamples();

    auto buildMap = [] (const juce::BigInteger& active, int total)
    {
        std::vector<int> m ((size_t) juce::jmax (total, 1), -1);
        int idx = 0;
        for (int ch = 0; ch < total; ++ch)
            if (active[ch]) m[(size_t) ch] = idx++;
        return m;
    };
    inputMap = buildMap (d->getActiveInputChannels(), d->getInputChannelNames().size());
    outputMap = buildMap (d->getActiveOutputChannels(), d->getOutputChannelNames().size());

    for (auto& st : stripState)
    {
        st.resetDsp();
        for (auto* sv : { &st.trim, &st.fader, &st.panL, &st.panR, &st.sendRev, &st.sendDly })
            sv->reset (sr, 0.02);
        st.cHpf = -1; st.cLowF = -1; st.cMidF = -1; st.cHighF = -1; st.cTone = -99;
    }
    for (auto& b : busState)
    {
        b.gain.reset (sr, 0.02);
        b.limiter.reset();
        b.cLow = b.cMid = b.cHigh = -99;
        for (int c = 0; c < 2; ++c) { b.low[c].reset(); b.mid[c].reset(); b.high[c].reset(); }
    }
    for (auto& g : fxGain) g.reset (sr, 0.02);
    songGain.reset (sr, 0.02);
    songPlayer.prepare (sr);

    for (auto& c : stripInserts) c.prepare (sr, d->getCurrentBufferSizeSamples());
    for (auto& c : busInserts)   c.prepare (sr, d->getCurrentBufferSizeSamples());

    reverb.setSampleRate (sr);
    reverb.reset();
    delay.prepare (sr);

    triggerAsyncUpdate();
}

void Engine::audioDeviceStopped() {}

void Engine::updateStripCoefficients (int s, StripState& st)
{
    auto& p = *strips[(size_t) s];
    const double sr = sampleRate.load();
    auto changed = [] (float& cache, float v) { if (cache != v) { cache = v; return true; } return false; };

    if (changed (st.cHpf, p.hpfFreq->get()))
        for (auto& f : st.hpf) f.setHighPass (sr, st.cHpf);
    const bool low = changed (st.cLowF, p.eqLowFreq->get()) | changed (st.cLowG, p.eqLowGain->get());
    if (low) for (auto& f : st.eqLow) f.setLowShelf (sr, st.cLowF, st.cLowG);
    const bool mid = changed (st.cMidF, p.eqMidFreq->get()) | changed (st.cMidG, p.eqMidGain->get())
                   | changed (st.cMidQ, p.eqMidQ->get());
    if (mid) for (auto& f : st.eqMid) f.setPeak (sr, st.cMidF, st.cMidQ, st.cMidG);
    const bool high = changed (st.cHighF, p.eqHighFreq->get()) | changed (st.cHighG, p.eqHighGain->get());
    if (high) for (auto& f : st.eqHigh) f.setHighShelf (sr, st.cHighF, st.cHighG);
    if (changed (st.cTone, p.tone->get()))
    {
        // "Color" axis of the XY pad: a tilt EQ, warm (down) <-> bright (up).
        const float t = st.cTone * 6.0f;
        for (auto& f : st.tiltLow)  f.setLowShelf (sr, 300.0, -t, 0.7);
        for (auto& f : st.tiltHigh) f.setHighShelf (sr, 3000.0, t, 0.7);
    }
    if (st.deessHp[0].b0 == 1.0f && st.deessHp[0].a1 == 0.0f)
        for (auto& f : st.deessHp) f.setHighPass (sr, 5500.0, 0.7071);
}

void Engine::processStrip (int s, StripState& st, float* l, float* r, int n, bool anySolo)
{
    auto& p = *strips[(size_t) s];
    const double sr = sampleRate.load();
    updateStripCoefficients (s, st);

    const bool muted = p.mute->on() || (anySolo && ! p.solo->on());
    const float pan = p.pan->get();
    const bool stereo = p.stereo->on();

    st.trim.setTargetValue (bvm::dbToGain (p.trim->get()) * (p.polarity->on() ? -1.0f : 1.0f));
    st.fader.setTargetValue (muted ? 0.0f : bvm::dbToGain (p.fader->get()));
    st.panL.setTargetValue (juce::jmin (1.0f, 1.0f - pan));
    st.panR.setTargetValue (juce::jmin (1.0f, 1.0f + pan));
    st.sendRev.setTargetValue (bvm::dbToGain (p.sendReverb->get()));
    st.sendDly.setTargetValue (bvm::dbToGain (p.sendDelay->get()));

    const bool hpfOn = p.hpfFreq->get() > 21.0f;
    const bool gateOn = p.gateThresh->get() > -79.5f;
    const bool eqOn = p.eqOn->on();
    const bool toneOn = std::abs (p.tone->get()) > 0.01f;
    const bool deessOn = p.deessThresh->get() < -0.5f;
    const float compThresh = p.compThresh->get();
    const bool compOn = compThresh < -0.1f;
    const float drive = p.drive->get();

    const float gateThresh = bvm::dbToGain (p.gateThresh->get());
    const float gateFloor = bvm::dbToGain (p.gateRange->get());
    const float gateAtt = bvm::timeCoeff (p.gateAttack->get(), sr), gateRel = bvm::timeCoeff (p.gateRelease->get(), sr);
    const float gateHold = p.gateHold->get() * 0.001f * (float) sr, gateEnvRel = bvm::timeCoeff (10.0f, sr);

    const float ratio = p.compRatio->get(), knee = p.compKnee->get();
    const float compAtt = bvm::timeCoeff (p.compAttack->get(), sr), compRel = bvm::timeCoeff (p.compRelease->get(), sr);
    const float autoMakeup = p.compAuto->on() ? -compThresh * (1.0f - 1.0f / ratio) * 0.5f : 0.0f;
    const float makeup = bvm::dbToGain (p.compMakeup->get() + autoMakeup);

    const float deessThresh = bvm::dbToGain (p.deessThresh->get());
    const float deessAtt = bvm::timeCoeff (1.0f, sr), deessRel = bvm::timeCoeff (60.0f, sr);

    const float driveK = 1.0f + drive * 12.0f, driveNorm = 1.0f / std::sqrt (driveK);

    float inPeakL = 0, inPeakR = 0, outPeakL = 0, outPeakR = 0, minGr = 0, minGate = 1;

    for (int i = 0; i < n; ++i)
    {
        const float tg = st.trim.getNextValue();
        l[i] *= tg;
        r[i] *= tg;
    }

    // Noise repair (bad cables etc.): de-crackle -> hum notch -> spectral denoise.
    if (const float dc = p.declick->get(); dc > 0.001f)
        st.declick.process (l, r, n, dc);
    if (const int hum = (int) p.hum->get(); hum > 0)
        st.hum.process (l, r, n, hum == 1 ? 50.0f : 60.0f, sr);
    if (denoiseReset[(size_t) s].exchange (false))
        st.denoise.clearProfile();
    const float dn = p.dnAmount->get();
    const bool learn = p.dnLearn->on();
    if (dn > 0.001f || learn)
        st.denoise.process (l, r, n, dn, learn);
    denoiseStatus[(size_t) s].store (st.denoise.status.load());

    for (int i = 0; i < n; ++i)
    {
        float xl = l[i], xr = r[i];

        if (hpfOn) { xl = st.hpf[0].process (xl); xr = st.hpf[1].process (xr); }

        inPeakL = juce::jmax (inPeakL, std::abs (xl));
        inPeakR = juce::jmax (inPeakR, std::abs (xr));

        if (gateOn)
        {
            const float g = st.gate.process (juce::jmax (std::abs (xl), std::abs (xr)), gateThresh, gateFloor,
                                             gateAtt, gateRel, gateHold, gateEnvRel);
            xl *= g; xr *= g;
            minGate = juce::jmin (minGate, g);
        }

        if (eqOn)
        {
            xl = st.eqHigh[0].process (st.eqMid[0].process (st.eqLow[0].process (xl)));
            xr = st.eqHigh[1].process (st.eqMid[1].process (st.eqLow[1].process (xr)));
        }

        if (toneOn)
        {
            xl = st.tiltHigh[0].process (st.tiltLow[0].process (xl));
            xr = st.tiltHigh[1].process (st.tiltLow[1].process (xr));
        }

        if (deessOn)
        {
            // Split off the sibilance band and duck only that band when it exceeds the threshold.
            const float hl = st.deessHp[0].process (xl), hr = st.deessHp[1].process (xr);
            const float lvl = juce::jmax (std::abs (hl), std::abs (hr));
            st.deessEnv = lvl > st.deessEnv ? lvl + (st.deessEnv - lvl) * deessAtt : lvl + (st.deessEnv - lvl) * deessRel;
            const float target = st.deessEnv > deessThresh ? juce::jmax (0.1f, deessThresh / st.deessEnv) : 1.0f;
            st.deessGain = target + (st.deessGain - target) * (target < st.deessGain ? deessAtt : deessRel);
            xl -= hl * (1.0f - st.deessGain);
            xr -= hr * (1.0f - st.deessGain);
        }

        if (compOn)
        {
            const float g = st.comp.process (juce::jmax (std::abs (xl), std::abs (xr)), compThresh, ratio, knee,
                                             compAtt, compRel) * makeup;
            xl *= g; xr *= g;
            minGr = juce::jmin (minGr, st.comp.grDb);
        }

        if (drive > 0.001f)
        {
            xl = (1.0f - drive) * xl + drive * std::tanh (driveK * xl) * driveNorm;
            xr = (1.0f - drive) * xr + drive * std::tanh (driveK * xr) * driveNorm;
        }

        l[i] = xl;
        r[i] = xr;
    }

    stripInserts[(size_t) s].process (l, r, n); // VST3 inserts, pre-fader

    for (int i = 0; i < n; ++i)
    {
        const float fg = st.fader.getNextValue();
        const float xl = l[i] * fg * st.panL.getNextValue();
        const float xr = r[i] * fg * st.panR.getNextValue();
        l[i] = xl;
        r[i] = xr;
        outPeakL = juce::jmax (outPeakL, std::abs (xl));
        outPeakR = juce::jmax (outPeakR, std::abs (xr));
    }

    stripInputMeters[(size_t) s].push (inPeakL, stereo ? inPeakR : inPeakL);
    auto& m = stripMeters[(size_t) s];
    m.push (outPeakL, outPeakR);
    m.gainReduction.store (minGr);
    m.gateGain.store (gateOn ? minGate : 1.0f);
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                               int n, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch)
        juce::FloatVectorOperations::clear (out[ch], n);
    if (n > maxBlock)
        return;

    busBuf.clear (0, n);
    fxBuf.clear (0, n);

    bool anySolo = false;
    for (int s = 0; s < kNumStrips; ++s)
        anySolo |= strips[(size_t) s]->active->on() && strips[(size_t) s]->solo->on();

    auto inputPtr = [&] (int physical) -> const float*
    {
        if (! juce::isPositiveAndBelow (physical, (int) inputMap.size())) return nullptr;
        const int idx = inputMap[(size_t) physical];
        return juce::isPositiveAndBelow (idx, numIn) ? in[idx] : nullptr;
    };

    float* sl = stripBuf.getWritePointer (0);
    float* sr = stripBuf.getWritePointer (1);

    for (int s = 0; s < kNumStrips; ++s)
    {
        auto& p = *strips[(size_t) s];
        if (! p.active->on())
            continue;

        if (s < kNumHwStrips)
        {
            const float* srcL = inputPtr ((int) p.sourceL->get());
            if (srcL == nullptr) continue;
            const float* srcR = p.stereo->on() ? inputPtr ((int) p.sourceR->get()) : nullptr;
            juce::FloatVectorOperations::copy (sl, srcL, n);
            juce::FloatVectorOperations::copy (sr, srcR != nullptr ? srcR : srcL, n);
        }
        else
        {
            auto& bridge = *inBridges[(size_t) (s - kNumHwStrips)];
            if (! bridge.isRunning()) continue;
            bridge.engineRead (sl, sr, n);
        }

        auto& st = stripState[(size_t) s];
        processStrip (s, st, sl, sr, n, anySolo);

        for (int b = 0; b < kNumBuses; ++b)
        {
            if (p.routes[(size_t) b]->on())
            {
                busBuf.addFrom (b * 2, 0, sl, n);
                busBuf.addFrom (b * 2 + 1, 0, sr, n);
            }
        }

        // Post-fader FX sends.
        auto* rl = fxBuf.getWritePointer (0); auto* rr = fxBuf.getWritePointer (1);
        auto* dl = fxBuf.getWritePointer (2); auto* dr = fxBuf.getWritePointer (3);
        if (st.sendRev.isSmoothing() || st.sendRev.getTargetValue() > 0.0f)
            for (int i = 0; i < n; ++i) { const float g = st.sendRev.getNextValue(); rl[i] += sl[i] * g; rr[i] += sr[i] * g; }
        if (st.sendDly.isSmoothing() || st.sendDly.getTargetValue() > 0.0f)
            for (int i = 0; i < n; ++i) { const float g = st.sendDly.getNextValue(); dl[i] += sl[i] * g; dr[i] += sr[i] * g; }
    }

    // Sing Studio song player (strip buffers are free again at this point).
    if (songPlayer.isLoaded())
    {
        songPlayer.render (sl, sr, n);
        songGain.setTargetValue (song.mute->on() ? 0.0f : bvm::dbToGain (song.gain->get()));
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i)
        {
            const float g = songGain.getNextValue();
            sl[i] *= g;
            sr[i] *= g;
            pl = juce::jmax (pl, std::abs (sl[i]));
            pr = juce::jmax (pr, std::abs (sr[i]));
        }
        songMeter.push (pl, pr);
        for (int b = 0; b < kNumBuses; ++b)
            if (song.routes[(size_t) b]->on())
            {
                busBuf.addFrom (b * 2, 0, sl, n);
                busBuf.addFrom (b * 2 + 1, 0, sr, n);
            }
    }

    // FX returns.
    {
        auto& rp = *fx[0];
        juce::Reverb::Parameters params;
        params.roomSize = rp.size->get();
        params.damping = rp.damping->get();
        params.width = rp.width->get();
        params.wetLevel = 1.0f;
        params.dryLevel = 0.0f;
        reverb.setParameters (params);
        reverb.processStereo (fxBuf.getWritePointer (0), fxBuf.getWritePointer (1), n);

        auto& dp = *fx[1];
        delay.process (fxBuf.getWritePointer (2), fxBuf.getWritePointer (3), n, dp.timeMs->get(), dp.feedback->get(),
                       dp.tone->get());

        for (int f = 0; f < kNumFx; ++f)
        {
            auto& p = *fx[(size_t) f];
            fxGain[(size_t) f].setTargetValue (p.mute->on() ? 0.0f : bvm::dbToGain (p.returnGain->get()));
            auto* fl = fxBuf.getWritePointer (f * 2);
            auto* fr = fxBuf.getWritePointer (f * 2 + 1);
            float pl = 0, pr = 0;
            for (int i = 0; i < n; ++i)
            {
                const float g = fxGain[(size_t) f].getNextValue();
                fl[i] *= g; fr[i] *= g;
                pl = juce::jmax (pl, std::abs (fl[i]));
                pr = juce::jmax (pr, std::abs (fr[i]));
            }
            fxMeters[(size_t) f].push (pl, pr);
            for (int b = 0; b < kNumBuses; ++b)
                if (p.routes[(size_t) b]->on())
                {
                    busBuf.addFrom (b * 2, 0, fl, n);
                    busBuf.addFrom (b * 2 + 1, 0, fr, n);
                }
        }
    }

    // Buses.
    const double rate = sampleRate.load();
    const float limRel = bvm::timeCoeff (80.0f, rate);
    for (int b = 0; b < kNumBuses; ++b)
    {
        auto& p = *buses[(size_t) b];
        auto& st = busState[(size_t) b];
        float* bl = busBuf.getWritePointer (b * 2);
        float* br = busBuf.getWritePointer (b * 2 + 1);

        const bool eqOn = p.eqOn->on();
        if (eqOn)
        {
            if (st.cLow != p.eqLowG->get())  { st.cLow = p.eqLowG->get();  for (auto& f : st.low)  f.setLowShelf (rate, 100.0, st.cLow); }
            if (st.cMid != p.eqMidG->get())  { st.cMid = p.eqMidG->get();  for (auto& f : st.mid)  f.setPeak (rate, 1000.0, 0.7, st.cMid); }
            if (st.cHigh != p.eqHighG->get()) { st.cHigh = p.eqHighG->get(); for (auto& f : st.high) f.setHighShelf (rate, 8000.0, st.cHigh); }
        }

        st.gain.setTargetValue (p.mute->on() ? 0.0f : bvm::dbToGain (p.fader->get()));
        const bool mono = p.mono->on(), lim = p.limiter->on();
        const float ceil = bvm::dbToGain (p.limitCeil->get());
        float pl = 0, pr = 0;

        for (int i = 0; i < n; ++i)
        {
            float xl = bl[i], xr = br[i];
            if (eqOn)
            {
                xl = st.high[0].process (st.mid[0].process (st.low[0].process (xl)));
                xr = st.high[1].process (st.mid[1].process (st.low[1].process (xr)));
            }
            if (mono) { const float m = 0.5f * (xl + xr); xl = xr = m; }
            bl[i] = xl; br[i] = xr;
        }

        busInserts[(size_t) b].process (bl, br, n);

        for (int i = 0; i < n; ++i)
        {
            float xl = bl[i], xr = br[i];
            const float g = st.gain.getNextValue();
            xl *= g; xr *= g;
            if (lim)
            {
                const float lg = st.limiter.process (juce::jmax (std::abs (xl), std::abs (xr)), ceil, limRel);
                xl *= lg; xr *= lg;
            }
            bl[i] = xl; br[i] = xr;
            pl = juce::jmax (pl, std::abs (xl));
            pr = juce::jmax (pr, std::abs (xr));
        }
        busMeters[(size_t) b].push (pl, pr);

        if (b < kNumHwBuses)
        {
            const int pair = (int) p.outPair->get();
            if (pair < 0) continue;
            auto outIdx = [&] (int physical)
            {
                return juce::isPositiveAndBelow (physical, (int) outputMap.size()) ? outputMap[(size_t) physical] : -1;
            };
            const int oL = outIdx (pair * 2), oR = outIdx (pair * 2 + 1);
            if (juce::isPositiveAndBelow (oL, numOut)) juce::FloatVectorOperations::add (out[oL], bl, n);
            if (juce::isPositiveAndBelow (oR, numOut)) juce::FloatVectorOperations::add (out[oR], br, n);
        }
        else
        {
            outBridges[(size_t) (b - kNumHwBuses)]->engineWrite (bl, br, n);
        }
    }
}

//==============================================================================
juce::File Engine::getDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("BetterVoiceMeeter");
}

juce::File Engine::getSettingsFile()
{
    return getDataDir().getChildFile ("settings.xml");
}

juce::String Engine::loadPlugin (InsertChain& chain, int slot, const juce::PluginDescription& desc)
{
    juce::String err;
    auto instance = formatManager.createPluginInstance (desc, sampleRate.load(), juce::jmax (blockSize.load(), 32), err);
    if (instance == nullptr)
        return err.isEmpty() ? "Could not load " + desc.name : err;
    auto old = chain.swap (slot, std::move (instance));
    if (old != nullptr && onPluginRemoved) onPluginRemoved (old.get());
    return {};
}

void Engine::removePlugin (InsertChain& chain, int slot)
{
    auto old = chain.swap (slot, nullptr);
    if (old != nullptr && onPluginRemoved) onPluginRemoved (old.get());
}

void Engine::restoreInserts (InsertChain& chain, const juce::XmlElement* xml)
{
    for (int i = 0; i < InsertChain::numSlots; ++i)
        if (chain.get (i) != nullptr)
            removePlugin (chain, i);
    if (xml == nullptr)
        return;

    for (auto* e : xml->getChildWithTagNameIterator ("SLOT"))
    {
        const int idx = e->getIntAttribute ("index", -1);
        juce::PluginDescription desc;
        auto* d = e->getChildByName ("PLUGIN");
        if (! juce::isPositiveAndBelow (idx, InsertChain::numSlots) || d == nullptr || ! desc.loadFromXml (*d))
            continue;
        const auto err = loadPlugin (chain, idx, desc);
        if (err.isNotEmpty())
        {
            juce::Logger::writeToLog ("Plugin restore failed (" + desc.name + "): " + err);
            continue;
        }
        juce::MemoryBlock state;
        if (state.fromBase64Encoding (e->getStringAttribute ("state")))
            chain.get (idx)->setStateInformation (state.getData(), (int) state.getSize());
        chain.slot (idx).bypass = e->getBoolAttribute ("bypass");
    }
}

std::unique_ptr<juce::XmlElement> Engine::createStateXml (bool includeDevice) const
{
    auto xml = std::make_unique<juce::XmlElement> ("BetterVoiceMeeter");
    if (includeDevice)
        if (auto dev = deviceManager.createStateXml())
            xml->addChildElement (dev.release());

    for (int i = 0; i < kNumStrips; ++i)
    {
        auto* e = xml->createNewChildElement ("STRIP");
        strips[(size_t) i]->saveTo (*e);
        e->setAttribute ("label", strips[(size_t) i]->label);
        e->addChildElement (stripInserts[(size_t) i].toXml().release());
        const auto profile = stripState[(size_t) i].denoise.getProfile();
        if (! profile.empty())
        {
            juce::StringArray vals;
            for (float v : profile) vals.add (juce::String (v, 8));
            e->createNewChildElement ("DENOISE")->setAttribute ("profile", vals.joinIntoString (","));
        }
    }
    for (int b = 0; b < kNumBuses; ++b)
    {
        auto* e = xml->createNewChildElement ("BUS");
        buses[(size_t) b]->saveTo (*e);
        e->addChildElement (busInserts[(size_t) b].toXml().release());
    }
    for (auto& f : fx)    f->saveTo (*xml->createNewChildElement ("FX"));

    {
        auto* e = xml->createNewChildElement ("SING");
        song.saveTo (*e);
        e->setAttribute ("engine", sing.engine);
        e->setAttribute ("stems", sing.stems);
        e->setAttribute ("musicaiWorkflow", sing.musicaiWorkflow);
        if (includeDevice) // the API key stays in the local settings file, never in shared presets
            e->setAttribute ("musicaiKey", sing.musicaiKey);
    }

    auto* v = xml->createNewChildElement ("VIRTUAL");
    for (int i = 0; i < kNumVirtBuses; ++i)  v->setAttribute ("out" + juce::String (i), outBridgeNames[(size_t) i]);
    for (int i = 0; i < kNumVirtStrips; ++i) v->setAttribute ("in" + juce::String (i), inBridgeNames[(size_t) i]);
    return xml;
}

void Engine::restoreStateXml (const juce::XmlElement& xml, bool includeDevice)
{
    if (includeDevice)
        if (auto* dev = xml.getChildByName ("DEVICESETUP"))
            deviceManager.initialise (64, 64, dev, true);

    int i = 0;
    for (auto* e : xml.getChildWithTagNameIterator ("STRIP"))
    {
        if (i >= kNumStrips) break;
        strips[(size_t) i]->loadFrom (*e);
        strips[(size_t) i]->label = e->getStringAttribute ("label", strips[(size_t) i]->label);
        strips[(size_t) i]->dnLearn->set (0.0f);
        restoreInserts (stripInserts[(size_t) i], e->getChildByName ("INSERTS"));
        if (auto* dn = e->getChildByName ("DENOISE"))
        {
            std::vector<float> prof;
            for (auto& v : juce::StringArray::fromTokens (dn->getStringAttribute ("profile"), ",", ""))
                prof.push_back (v.getFloatValue());
            stripState[(size_t) i].denoise.setProfile (prof);
        }
        else
            stripState[(size_t) i].denoise.clearProfile();
        ++i;
    }
    i = 0;
    for (auto* e : xml.getChildWithTagNameIterator ("BUS"))
        if (i < kNumBuses)
        {
            restoreInserts (busInserts[(size_t) i], e->getChildByName ("INSERTS"));
            buses[(size_t) i++]->loadFrom (*e);
        }
    i = 0;
    for (auto* e : xml.getChildWithTagNameIterator ("FX"))
        if (i < kNumFx) fx[(size_t) i++]->loadFrom (*e);

    if (auto* e = xml.getChildByName ("SING"))
    {
        song.loadFrom (*e);
        sing.engine = e->getStringAttribute ("engine", sing.engine);
        sing.stems = e->getIntAttribute ("stems", sing.stems);
        sing.musicaiWorkflow = e->getStringAttribute ("musicaiWorkflow", sing.musicaiWorkflow);
        if (e->hasAttribute ("musicaiKey"))
            sing.musicaiKey = e->getStringAttribute ("musicaiKey");
    }

    if (auto* v = xml.getChildByName ("VIRTUAL"))
    {
        for (int k = 0; k < kNumVirtBuses; ++k)  outBridgeNames[(size_t) k] = v->getStringAttribute ("out" + juce::String (k));
        for (int k = 0; k < kNumVirtStrips; ++k) inBridgeNames[(size_t) k] = v->getStringAttribute ("in" + juce::String (k));
    }
}

void Engine::saveState()
{
    auto f = getSettingsFile();
    f.getParentDirectory().createDirectory();
    createStateXml (true)->writeTo (f);
}

void Engine::savePreset (const juce::File& f)
{
    createStateXml (false)->writeTo (f);
}

void Engine::loadPreset (const juce::File& f)
{
    if (auto xml = juce::XmlDocument::parse (f))
    {
        restoreStateXml (*xml, false);
        reopenBridges();
        if (onLayoutChanged) onLayoutChanged();
    }
}
