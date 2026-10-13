#include "AppRouter.h"
#include "AppOutputRouting.h"
#include "DSP.h"

#include <windows.h>
#include <tlhelp32.h>

namespace
{
// Default group for well-known apps (by exe name).
int defaultGroupFor (const juce::String& key)
{
    const auto exe = juce::File (key).getFileNameWithoutExtension().toLowerCase();
    for (auto* s : { "discord", "teams", "ms-teams", "zoom", "slack", "skype", "whatsapp", "telegram", "teamspeak", "ts3client", "mumble" })
        if (exe.contains (s)) return 1; // Chat
    for (auto* s : { "spotify", "music", "itunes", "vlc", "foobar", "winamp", "aimp", "deezer", "tidal", "musicbee" })
        if (exe.contains (s)) return 2; // Music
    for (auto* s : { "chrome", "msedge", "firefox", "opera", "brave", "vivaldi", "arc" })
        if (exe.contains (s)) return 3; // Browser
    return 0; // Game (everything else)
}

} // namespace

const char* AppRouter::groupName (int g)
{
    static const char* names[] = { "Game", "Chat", "Music", "Browser" };
    return juce::isPositiveAndBelow (g, numGroups) ? names[g] : "Direct";
}

AppRouter::AppRouter() : juce::Thread ("App router")
{
    groups[1].streamDb = -60.0f; // chat: not on stream by default (avoids people hearing themselves)
    groups[3].streamDb = -60.0f; // browser: off stream by default
    loadRestoreFile();
    startThread();
}

AppRouter::~AppRouter() { shutdown(); }

juce::File AppRouter::restoreFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("BetterVoiceMeeter").getChildFile ("app-routing-restore.xml");
}

void AppRouter::saveRestoreFile() const
{
    juce::XmlElement xml ("RESTORE");
    for (auto& [key, id] : previousOutput)
    {
        auto* e = xml.createNewChildElement ("APP");
        e->setAttribute ("key", key);
        e->setAttribute ("output", id);
    }
    restoreFile().getParentDirectory().createDirectory();
    if (previousOutput.empty()) restoreFile().deleteFile();
    else xml.writeTo (restoreFile());
}

void AppRouter::loadRestoreFile()
{
    if (auto xml = juce::XmlDocument::parse (restoreFile()))
        for (auto* e : xml->getChildWithTagNameIterator ("APP"))
            previousOutput[e->getStringAttribute ("key")] = e->getStringAttribute ("output");
}

void AppRouter::setMode (Mode m)
{
    mode = (int) m;
    notify();
}

juce::StringArray AppRouter::candidateSinkDevices()
{
    // Any playback device works as a sink; it should be one nobody listens to.
    juce::StringArray out;
    for (auto& n : ExternalBridge::getDeviceNames (ExternalBridge::Direction::output))
        if (! n.containsIgnoreCase ("Focusrite"))
            out.add (n);
    return out;
}

void AppRouter::setSinkDevice (const juce::String& name)
{
    {
        std::lock_guard<std::mutex> g (lock);
        sinkName = name;
        sinkId.clear(); // resolved on the router thread
    }
    notify();
}

juce::String AppRouter::getSinkDevice() const
{
    std::lock_guard<std::mutex> g (lock);
    return sinkName;
}

std::vector<AppRouter::Status> AppRouter::snapshot() const
{
    std::lock_guard<std::mutex> g (lock);
    return published;
}

AppRouter::AppSetting AppRouter::getSetting (const juce::String& key) const
{
    std::lock_guard<std::mutex> g (lock);
    if (auto it = settings.find (key); it != settings.end()) return it->second;
    AppSetting s;
    s.group = defaultGroupFor (key);
    return s;
}

void AppRouter::setSetting (const juce::String& key, const AppSetting& s)
{
    {
        std::lock_guard<std::mutex> g (lock);
        settings[key] = s;
    }
    // Live parameters go straight to the slot; routing changes are handled by the router thread.
    const auto hash = key.hashCode64();
    for (auto& slot : slots)
        if (slot.active && slot.keyHash.load() == hash)
        {
            slot.gain = s.mute ? 0.0f : bvm::dbToGain (s.gainDb);
            slot.group = s.group;
            slot.routes = s.routes;
        }
    notify();
}

void AppRouter::saveTo (juce::XmlElement& xml) const
{
    std::lock_guard<std::mutex> g (lock);
    xml.setAttribute ("mode", mode.load());
    xml.setAttribute ("sink", sinkName);
    for (int i = 0; i < numGroups; ++i)
    {
        auto* e = xml.createNewChildElement ("GROUP");
        e->setAttribute ("you", groups[(size_t) i].youDb.load());
        e->setAttribute ("stream", groups[(size_t) i].streamDb.load());
        e->setAttribute ("mute", groups[(size_t) i].mute.load());
    }
    for (auto& [key, s] : settings)
    {
        auto* e = xml.createNewChildElement ("APP");
        e->setAttribute ("key", key);
        e->setAttribute ("group", s.group);
        e->setAttribute ("routed", s.routed);
        e->setAttribute ("gain", s.gainDb);
        e->setAttribute ("mute", s.mute);
        e->setAttribute ("routes", s.routes);
    }
}

void AppRouter::loadFrom (const juce::XmlElement& xml)
{
    {
        std::lock_guard<std::mutex> g (lock);
        sinkName = xml.getStringAttribute ("sink", sinkName);
        int i = 0;
        for (auto* e : xml.getChildWithTagNameIterator ("GROUP"))
        {
            if (i >= numGroups) break;
            groups[(size_t) i].youDb = (float) e->getDoubleAttribute ("you", 0.0);
            groups[(size_t) i].streamDb = (float) e->getDoubleAttribute ("stream", -12.0);
            groups[(size_t) i].mute = e->getBoolAttribute ("mute");
            ++i;
        }
        for (auto* e : xml.getChildWithTagNameIterator ("APP"))
        {
            AppSetting s;
            s.group = e->getIntAttribute ("group", 0);
            s.routed = e->getBoolAttribute ("routed", true);
            s.gainDb = (float) e->getDoubleAttribute ("gain", 0.0);
            s.mute = e->getBoolAttribute ("mute");
            s.routes = e->getIntAttribute ("routes", 1);
            settings[e->getStringAttribute ("key")] = s;
        }
    }
    setMode ((Mode) juce::jlimit (0, 2, xml.getIntAttribute ("mode", 0)));
}

//==============================================================================
void AppRouter::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    sessions.start();
    while (! threadShouldExit())
    {
        step();
        wait (400);
    }
    // Stopping (app exit): put every routed app back on its own output.
    const auto apps = sessions.snapshot();
    for (auto& slot : slots)
        if (slot.key.isNotEmpty())
            releaseSlot (slot, std::any_of (apps.begin(), apps.end(), [&] (auto& a) { return a.key == slot.key; }));
    sessions.stop();
    CoUninitialize();
}

bool AppRouter::wanted (const AppSessions::App& a) const
{
    if (a.key == "system") return false;
    const auto m = (Mode) mode.load();
    if (m == Mode::off) return false;
    const auto s = getSetting (a.key);
    return m == Mode::groups ? s.group != direct : s.routed;
}

unsigned long AppRouter::rootProcess (const AppSessions::App& a) const
{
    // Capture the top-most process of the app (process-tree capture then includes e.g. Chrome's audio service).
    if (a.pids.empty()) return 0;
    std::map<DWORD, std::pair<DWORD, juce::String>> parentOf;
    HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe { sizeof (pe) };
        for (BOOL ok = Process32FirstW (snap, &pe); ok; ok = Process32NextW (snap, &pe))
            parentOf[pe.th32ProcessID] = { pe.th32ParentProcessID, juce::String (pe.szExeFile).toLowerCase() };
        CloseHandle (snap);
    }
    const auto exe = juce::File (a.key).getFileName().toLowerCase();
    DWORD pid = (DWORD) a.pids.front();
    for (int guard = 0; guard < 16; ++guard)
    {
        auto it = parentOf.find (pid);
        if (it == parentOf.end()) break;
        auto parent = parentOf.find (it->second.first);
        if (parent == parentOf.end() || parent->second.second != exe) break;
        pid = it->second.first;
    }
    return pid;
}

void AppRouter::routeApp (Slot& slot, const AppSessions::App& a)
{
    juce::String sink;
    {
        std::lock_guard<std::mutex> g (lock);
        sink = sinkId;
        if (previousOutput.find (a.key) == previousOutput.end() && ! a.pids.empty())
        {
            previousOutput[a.key] = AppOutputRouting::getAppOutput (a.pids.front());
            saveRestoreFile();
        }
    }
    for (auto pid : a.pids)
        if (std::find (slot.routedPids.begin(), slot.routedPids.end(), pid) == slot.routedPids.end()
            && AppOutputRouting::setAppOutput (pid, sink))
            slot.routedPids.push_back (pid);

    const auto root = rootProcess (a);
    if (slot.capture == nullptr || slot.capturePid != root || ! slot.capture->isCapturing() || slot.resync.exchange (false))
    {
        slot.active = false;
        juce::Thread::sleep (30); // let the audio thread leave this slot
        if (slot.capture) slot.capture->stop();
        slot.fifo.reset (ProcessCapture::sampleRate, engineRate.load(), 480, engineBlock.load());
        slot.capture = std::make_unique<ProcessCapture>();
        slot.capture->onAudio = [&slot] (const float* l, const float* r, int n) { slot.fifo.push (l, r, n); };
        slot.capturePid = root;
        if (slot.capture->start (root).isNotEmpty())
            slot.capture.reset();
    }
    const auto s = getSetting (a.key);
    slot.key = a.key;
    slot.keyHash = a.key.hashCode64();
    slot.group = s.group;
    slot.routes = s.routes;
    slot.gain = s.mute ? 0.0f : bvm::dbToGain (s.gainDb);
    slot.active = slot.capture != nullptr;
}

void AppRouter::restoreOutput (Slot& slot)
{
    std::lock_guard<std::mutex> g (lock);
    const auto prev = previousOutput.count (slot.key) ? previousOutput[slot.key] : juce::String();
    bool restored = false;
    for (auto pid : slot.routedPids)
        restored |= AppOutputRouting::setAppOutput (pid, prev);
    if (restored)
    {
        previousOutput.erase (slot.key);
        saveRestoreFile();
    }
    slot.routedPids.clear();
}

void AppRouter::releaseSlot (Slot& slot, bool appStillRunning)
{
    slot.active = false;
    slot.moved = false;
    slot.draining = false;
    juce::Thread::sleep (30);
    if (slot.capture) slot.capture->stop();
    slot.capture.reset();
    if (appStillRunning && ! slot.routedPids.empty())
    {
        std::lock_guard<std::mutex> g (lock);
        const auto prev = previousOutput.count (slot.key) ? previousOutput[slot.key] : juce::String();
        bool restored = false;
        for (auto pid : slot.routedPids)
            restored |= AppOutputRouting::setAppOutput (pid, prev);
        if (restored)
        {
            previousOutput.erase (slot.key);
            saveRestoreFile();
        }
    }
    slot.routedPids.clear();
    slot.key.clear();
    slot.keyHash = 0;
    slot.capturePid = 0;
}

void AppRouter::restorePending (const std::vector<AppSessions::App>& apps)
{
    // Apps routed earlier (e.g. before a crash, or not running when routing stopped): put them back.
    std::lock_guard<std::mutex> g (lock);
    bool changed = false;
    for (auto& a : apps)
    {
        auto it = previousOutput.find (a.key);
        if (it == previousOutput.end() || a.pids.empty()) continue;
        bool inSlot = false;
        for (auto& s : slots) inSlot |= s.active && s.key == a.key;
        if (inSlot) continue;
        bool ok = false;
        for (auto pid : a.pids) ok |= AppOutputRouting::setAppOutput (pid, it->second);
        if (ok) { previousOutput.erase (it); changed = true; }
    }
    if (changed) saveRestoreFile();
}

void AppRouter::step()
{
    {
        std::lock_guard<std::mutex> g (lock);
        if (sinkId.isEmpty())
        {
            if (sinkName.isEmpty())
                for (auto* pref : { "Realtek Digital Output", "Voice Changer Virtual", "NVIDIA High Definition" })
                    for (auto& n : candidateSinkDevices())
                        if (sinkName.isEmpty() && n.containsIgnoreCase (pref)) sinkName = n;
            if (sinkName.isNotEmpty())
                sinkId = AppOutputRouting::findPlaybackDeviceId (sinkName);
        }
    }
    const auto apps = sessions.snapshot();
    const bool canRoute = AppOutputRouting::isAvailable() && getSinkDevice().isNotEmpty();

    juce::String sinkNow;
    { std::lock_guard<std::mutex> g (lock); sinkNow = sinkName; }
    auto playsOnSink = [&] (const AppSessions::App& a)
    {
        for (auto& d : a.activeDevices)
            if (sinkNow.isNotEmpty() && d.containsIgnoreCase (sinkNow)) return true;
        return false;
    };

    // Release slots whose app is gone or no longer wanted. If the app's current sound is still on the
    // sink, put its output setting back but keep playing the capture (to A1) until it has moved back.
    for (auto& slot : slots)
    {
        if (slot.key.isEmpty()) continue;
        auto it = std::find_if (apps.begin(), apps.end(), [&] (auto& a) { return a.key == slot.key; });
        if (it == apps.end()) { releaseSlot (slot, false); continue; }
        if (canRoute && wanted (*it)) { slot.draining = false; continue; }
        if (! slot.routedPids.empty()) restoreOutput (slot);
        if (playsOnSink (*it) && slot.capture != nullptr) { slot.draining = true; slot.moved = true; }
        else releaseSlot (slot, true);
    }

    restorePending (apps);

    // Route wanted apps.
    juce::String sinkFull;
    { std::lock_guard<std::mutex> g (lock); sinkFull = sinkName; }
    for (auto& a : apps)
    {
        if (! canRoute || ! wanted (a)) continue;
        Slot* slot = nullptr;
        for (auto& s : slots) if (s.key == a.key) slot = &s;
        if (slot == nullptr)
            for (auto& s : slots) if (slot == nullptr && s.key.isEmpty()) slot = &s;
        if (slot == nullptr) continue; // all slots busy
        routeApp (*slot, a);
        // Mix the captured copy only once the app really plays on the sink (never heard twice).
        bool onSink = ! a.activeDevices.isEmpty();
        for (auto& d : a.activeDevices) onSink &= sinkFull.isNotEmpty() && d.containsIgnoreCase (sinkFull);
        if (! a.activeDevices.isEmpty()) slot->moved = onSink;
    }

    // Publish status for the UI.
    std::vector<Status> out;
    for (auto& a : apps)
    {
        if (a.key == "system") continue;
        Status st;
        st.key = a.key; st.name = a.name; st.icon = a.icon; st.playing = a.active;
        st.setting = getSetting (a.key);
        bool draining = false;
        for (auto& s : slots)
            if (s.key == a.key && s.active && s.draining) draining = true;
        if (draining)
            st.note = "Moving back: pause/play " + a.name + " to finish";
        for (auto& s : slots)
            if (s.key == a.key && s.active && ! s.draining)
            {
                st.routed = true;
                st.moved = s.moved;
                st.peak = juce::jmax (s.meter.peakL.load(), s.meter.peakR.load());
            }
        if (! canRoute && mode.load() != (int) Mode::off)
            st.note = AppOutputRouting::isAvailable() ? "Choose a silent device" : "Needs Windows 10 21H2 or newer";
        else if (st.routed && ! st.moved && a.active)
            st.note = "Pause/play or restart " + a.name + " to move it";
        else if (st.routed && ! st.moved)
            st.note = "Routed (starts with its next sound)";
        out.push_back (st);
    }
    std::lock_guard<std::mutex> g (lock);
    published = std::move (out);
}

void AppRouter::shutdown()
{
    stopThread (5000); // run() restores routed apps on its way out
}

//==============================================================================
void AppRouter::prepare (double rate, int block)
{
    engineRate = rate;
    engineBlock = block;
    for (auto& g : groups)
    {
        g.youGain.reset (rate, 0.03);
        g.streamGain.reset (rate, 0.03);
    }
    for (auto& s : slots)
        s.smoothGain.reset (rate, 0.03);
    // Slots re-sync their FIFO rates on the next routing pass.
    for (auto& s : slots)
        s.resync = true;
}

void AppRouter::process (juce::AudioBuffer<float>& busBuf, int numBuses, int n) noexcept
{
    if (n > appBuf.getNumSamples())
        return;
    const bool off = mode.load() == (int) Mode::off;
    const bool groupsMode = mode.load() == (int) Mode::groups;
    groupBuf.clear (0, n);
    auto* al = appBuf.getWritePointer (0);
    auto* ar = appBuf.getWritePointer (1);

    for (auto& s : slots)
    {
        if (! s.active.load())
            continue;
        s.fifo.pull (al, ar, n); // always drain, so the FIFO stays in sync
        if (! s.moved.load())
            continue;            // app still plays on its own device: don't double it
        s.smoothGain.setTargetValue (s.gain.load());
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i)
        {
            const float g = s.smoothGain.getNextValue();
            al[i] *= g; ar[i] *= g;
            pl = juce::jmax (pl, std::abs (al[i]));
            pr = juce::jmax (pr, std::abs (ar[i]));
        }
        s.meter.push (pl, pr);

        if (s.draining.load() || off)
        {
            // Routing was switched off but this app's current sound is still on the sink: keep it audible.
            busBuf.addFrom (0, 0, al, n);
            busBuf.addFrom (1, 0, ar, n);
        }
        else if (groupsMode)
        {
            const int g = juce::jlimit (0, numGroups - 1, s.group.load());
            groupBuf.addFrom (g * 2, 0, al, n);
            groupBuf.addFrom (g * 2 + 1, 0, ar, n);
        }
        else
        {
            const int routes = s.routes.load();
            for (int b = 0; b < numBuses; ++b)
                if (routes & (1 << b))
                {
                    busBuf.addFrom (b * 2, 0, al, n);
                    busBuf.addFrom (b * 2 + 1, 0, ar, n);
                }
        }
    }

    if (! groupsMode || off)
        return;
    const int youBus = 0, streamBus = juce::jmin (3, numBuses - 1); // A1 and B1
    for (int gi = 0; gi < numGroups; ++gi)
    {
        auto& g = groups[(size_t) gi];
        const bool muted = g.mute.load();
        g.youGain.setTargetValue (muted ? 0.0f : bvm::dbToGain (g.youDb.load()));
        g.streamGain.setTargetValue (muted ? 0.0f : bvm::dbToGain (g.streamDb.load()));
        const auto* gl = groupBuf.getReadPointer (gi * 2);
        const auto* gr = groupBuf.getReadPointer (gi * 2 + 1);
        auto* yl = busBuf.getWritePointer (youBus * 2);
        auto* yr = busBuf.getWritePointer (youBus * 2 + 1);
        auto* sl = busBuf.getWritePointer (streamBus * 2);
        auto* sr = busBuf.getWritePointer (streamBus * 2 + 1);
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i)
        {
            const float y = g.youGain.getNextValue(), st = g.streamGain.getNextValue();
            yl[i] += gl[i] * y; yr[i] += gr[i] * y;
            sl[i] += gl[i] * st; sr[i] += gr[i] * st;
            pl = juce::jmax (pl, std::abs (gl[i]));
            pr = juce::jmax (pr, std::abs (gr[i]));
        }
        g.meter.push (pl, pr);
    }
}
