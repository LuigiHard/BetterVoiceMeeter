// End-to-end check of App Mixer Groups mode with a test tone app only (all other apps stay Direct).
// Usage: AppRouterTest <tone script path>   (launches python <script> twice)
#include <JuceHeader.h>
#include "../Source/AppRouter.h"
#include "../Source/AppOutputRouting.h"
#include <windows.h>
#include <cstdio>

static juce::String exeKeyContaining (AppRouter& r, const juce::String& part)
{
    for (auto& s : r.snapshot())
        if (s.key.containsIgnoreCase (part)) return s.key;
    return {};
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: AppRouterTest <tone.py>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const juce::String script = juce::String::fromUTF8 (argv[1]);

    AppRouter router;
    router.prepare (48000.0, 480);
    juce::Thread::sleep (2500); // let it discover apps

    // Keep every real app Direct for this test.
    int directCount = 0;
    for (auto& s : router.snapshot())
    {
        auto st = router.getSetting (s.key);
        st.group = AppRouter::direct;
        router.setSetting (s.key, st);
        ++directCount;
    }
    std::printf ("set %d existing apps to Direct\n", directCount);

    router.setSinkDevice ("Realtek Digital Output");
    router.group (0).youDb = 0.0f;     // Game: you at 0 dB
    router.group (0).streamDb = -6.0f; // stream 6 dB lower
    router.setMode (AppRouter::Mode::groups);

    // First tone process: gets routed while already playing (moves on its next stream).
    juce::ChildProcess tone1;
    tone1.start (juce::StringArray { "python", "-I", script, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bvm_tone_devices.txt").getFullPathName() });
    juce::Thread::sleep (3000);
    const auto key = exeKeyContaining (router, "python");
    std::printf ("tone app key: %s\n", key.isEmpty() ? "(not found)" : juce::File (key).getFileName().toRawUTF8());
    if (key.isEmpty()) return 1;
    for (auto& s : router.snapshot())
        if (s.key == key)
            std::printf ("first tone : routed %d, moved %d, note '%s'\n", (int) s.routed, (int) s.moved, s.note.toRawUTF8());
    tone1.kill();
    juce::Thread::sleep (1500);

    // Second tone process: same app, starts after routing -> should open on the sink and be mixed in.
    juce::ChildProcess tone2;
    tone2.start (juce::StringArray { "python", "-I", script, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bvm_tone_devices.txt").getFullPathName() });
    juce::AudioBuffer<float> bus (kNumBuses * 2, 480);
    double youSum = 0, streamSum = 0;
    long frames = 0;
    bool movedSeen = false;
    for (int i = 0; i < 600; ++i) // ~6 s of 10 ms blocks, like the audio callback
    {
        bus.clear();
        router.process (bus, kNumBuses, 480);
        if (i > 300)
        {
            youSum += bus.getRMSLevel (0, 0, 480);
            streamSum += bus.getRMSLevel (kNumHwBuses * 2, 0, 480);
            ++frames;
        }
        for (auto& s : router.snapshot()) if (s.key == key && s.moved) movedSeen = true;
        juce::Thread::sleep (10);
    }
    const double you = 20 * std::log10 (youSum / juce::jmax (1L, frames) + 1e-9);
    const double stream = 20 * std::log10 (streamSum / juce::jmax (1L, frames) + 1e-9);
    std::printf ("second tone: moved %d, level in A1 (you) %.1f dBFS, in B1 (stream) %.1f dBFS\n", (int) movedSeen, you, stream);
    tone2.kill();

    // Turn routing off: the app's output must be restored (it is, the next time the app has audio).
    // tone2 restarted so it is playing on the sink at the moment routing is switched off.
    juce::ChildProcess tone2b;
    tone2b.start (juce::StringArray { "python", "-I", script, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bvm_tone_devices.txt").getFullPathName() });
    juce::Thread::sleep (2500);
    router.setMode (AppRouter::Mode::off);
    double drainSum = 0; long drainFrames = 0; bool drainNote = false;
    for (int i = 0; i < 250; ++i)
    {
        bus.clear();
        router.process (bus, kNumBuses, 480);
        if (i > 100) { drainSum += bus.getRMSLevel (0, 0, 480); ++drainFrames; }
        for (auto& s : router.snapshot()) if (s.key == key && s.note.startsWith ("Moving back")) drainNote = true;
        juce::Thread::sleep (10);
    }
    tone2b.kill();
    const double drain = 20 * std::log10 (drainSum / juce::jmax (1L, drainFrames) + 1e-9);
    std::printf ("routing off while playing on sink: still heard in A1 at %.1f dBFS, note shown %d\n", drain, (int) drainNote);
    juce::Thread::sleep (1500);
    juce::ChildProcess tone3;
    tone3.start (juce::StringArray { "python", "-I", script, juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bvm_tone_devices.txt").getFullPathName() });
    juce::Thread::sleep (4000);
    tone3.kill();
    const bool restoreFileGone = ! juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                      .getChildFile ("BetterVoiceMeeter/app-routing-restore.xml").existsAsFile();
    std::printf ("after off  : restore list empty %d\n", (int) restoreFileGone);
    router.shutdown();

    const bool ok = movedSeen && you > -45.0 && std::abs ((you - stream) - 6.0) < 1.5 && drain > -45.0 && restoreFileGone;
    std::printf ("%s\n", ok ? "PASS" : "FAIL");
    CoUninitialize();
    return ok ? 0 : 1;
}
