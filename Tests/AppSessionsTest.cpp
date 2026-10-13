// Lists per-app audio sessions, then sets the "System sounds" volume, checks Windows reports
// the new value through an independent query, and restores the original volume.
#include <JuceHeader.h>
#include "../Source/AppSessions.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <cstdio>

static float systemVolumeIndependent()
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioSessionManager2> mgr; ComPtr<IAudioSessionEnumerator> list;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en));
    en->GetDefaultAudioEndpoint (eRender, eConsole, &dev);
    dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr);
    mgr->GetSessionEnumerator (&list);
    int n = 0; list->GetCount (&n);
    for (int i = 0; i < n; ++i)
    {
        ComPtr<IAudioSessionControl> c; ComPtr<IAudioSessionControl2> c2; ComPtr<ISimpleAudioVolume> v;
        list->GetSession (i, &c); c.As (&c2); c.As (&v);
        if (c2->IsSystemSoundsSession() == S_OK) { float f = -1; v->GetMasterVolume (&f); return f; }
    }
    return -1;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    AppSessions s;
    s.start();
    juce::Thread::sleep (700);
    auto apps = s.snapshot();
    for (auto& a : apps)
        std::printf ("  %-28s vol %3d%%  mute %d  active %d  icon %s  on %s\n", a.name.toRawUTF8(), (int) std::round (a.volume * 100),
                     (int) a.mute, (int) a.active, a.icon.isValid() ? "yes" : "no ", a.devices.toRawUTF8());
    const float original = systemVolumeIndependent();
    s.setVolume ("system", 0.37f);
    juce::Thread::sleep (300);
    const float changed = systemVolumeIndependent();
    s.setVolume ("system", original);
    juce::Thread::sleep (300);
    const float restored = systemVolumeIndependent();
    s.stop();
    std::printf ("system sounds volume: original %.2f -> set 0.37 -> Windows reports %.2f -> restored %.2f\n", original, changed, restored);
    const bool ok = std::abs (changed - 0.37f) < 0.01f && std::abs (restored - original) < 0.01f && ! apps.empty();
    std::printf ("%s\n", ok ? "PASS" : "FAIL");
    CoUninitialize();
    return ok ? 0 : 1;
}
