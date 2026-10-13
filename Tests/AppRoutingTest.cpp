// Moves a playing app to another output device with AppOutputRouting, checks where Windows
// plays it and that process-loopback capture still receives it, then restores the app.
// Usage: AppRoutingTest <pid> <sink device name part>
#include <JuceHeader.h>
#include "../Source/AppOutputRouting.h"
#include "../Source/ProcessCapture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static juce::String activeDevicesOf (DWORD pid)
{
    juce::StringArray out;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> devs;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en));
    en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devs);
    UINT count = 0; devs->GetCount (&count);
    for (UINT d = 0; d < count; ++d)
    {
        ComPtr<IMMDevice> dev; ComPtr<IAudioSessionManager2> mgr; ComPtr<IAudioSessionEnumerator> list; ComPtr<IPropertyStore> props;
        devs->Item (d, &dev);
        if (FAILED (dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr)) || FAILED (mgr->GetSessionEnumerator (&list)))
            continue;
        int n = 0; list->GetCount (&n);
        for (int i = 0; i < n; ++i)
        {
            ComPtr<IAudioSessionControl> c; ComPtr<IAudioSessionControl2> c2;
            list->GetSession (i, &c); c.As (&c2);
            DWORD p = 0; c2->GetProcessId (&p);
            AudioSessionState st; c2->GetState (&st);
            if (p != pid || st != AudioSessionStateActive) continue;
            dev->OpenPropertyStore (STGM_READ, &props);
            PROPVARIANT v; PropVariantInit (&v);
            props->GetValue (PKEY_Device_FriendlyName, &v);
            out.add (juce::String (v.pwszVal));
            PropVariantClear (&v);
        }
    }
    return out.isEmpty() ? juce::String ("(none)") : out.joinIntoString (", ");
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("usage: AppRoutingTest <pid> <sink device name part>\n"); return 2; }
    const DWORD pid = (DWORD) std::atoi (argv[1]);
    const juce::String sinkName (argv[2]);
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    std::printf ("routing API available: %s\n", AppOutputRouting::isAvailable() ? "yes" : "no");
    const auto sinkId = AppOutputRouting::findPlaybackDeviceId (sinkName);
    std::printf ("sink '%s' id: %s\n", sinkName.toRawUTF8(), sinkId.isEmpty() ? "NOT FOUND" : "found");

    std::atomic<double> sumSq { 0 }; std::atomic<long> count { 0 };
    ProcessCapture cap;
    cap.onAudio = [&] (const float* l, const float* r, int n)
    {
        double s = 0;
        for (int i = 0; i < n; ++i) s += 0.5 * (l[i] * l[i] + r[i] * r[i]);
        sumSq = sumSq + s; count += n;
    };
    cap.start (pid);
    auto level = [&]
    {
        sumSq = 0; count = 0;
        juce::Thread::sleep (1200);
        return 20.0 * std::log10 (std::sqrt (count > 0 ? sumSq / (double) count : 0.0) + 1e-9);
    };

    juce::Thread::sleep (300);
    std::printf ("before : app output setting '%s', playing on %s, captured %.1f dBFS\n",
                 AppOutputRouting::getAppOutput (pid).isEmpty() ? "default" : "custom", activeDevicesOf (pid).toRawUTF8(), level());

    const bool set = AppOutputRouting::setAppOutput (pid, sinkId);
    juce::Thread::sleep (300);
    const bool moved = AppOutputRouting::getAppOutput (pid) == sinkId;
    juce::String during = activeDevicesOf (pid);
    for (int i = 0; i < 20 && during == "(none)"; ++i) // wait for the app to (re)start playing
    {
        juce::Thread::sleep (500);
        during = activeDevicesOf (pid);
    }
    juce::Thread::sleep (300);
    const double capDuring = level();
    std::printf ("moved  : set ok %d, setting matches %d, playing on %s, captured %.1f dBFS\n", (int) set, (int) moved, during.toRawUTF8(), capDuring);

    AppOutputRouting::setAppOutput (pid, {});
    juce::Thread::sleep (1500);
    std::printf ("restore: app output setting '%s', playing on %s\n",
                 AppOutputRouting::getAppOutput (pid).isEmpty() ? "default" : "custom", activeDevicesOf (pid).toRawUTF8());
    cap.stop();

    const bool ok = set && moved && during.containsIgnoreCase (sinkName) && capDuring > -45.0;
    std::printf ("%s\n", ok ? "PASS: app moved to the sink and capture kept its audio" : "FAIL");
    CoUninitialize();
    return ok ? 0 : 1;
}
