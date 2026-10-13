// Experiment: does process-loopback capture still receive an app's audio when that app's
// Windows session is muted / turned down? Usage: ProcessCaptureTest <pid of a process playing a tone>
#include <JuceHeader.h>
#include "../Source/ProcessCapture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static void forSessionsOf (DWORD pid, const std::function<void (ISimpleAudioVolume*)>& fn)
{
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> devs;
    CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en));
    en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devs);
    UINT count = 0; devs->GetCount (&count);
    for (UINT d = 0; d < count; ++d)
    {
        ComPtr<IMMDevice> dev; ComPtr<IAudioSessionManager2> mgr; ComPtr<IAudioSessionEnumerator> list;
        devs->Item (d, &dev);
        if (FAILED (dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr)) || FAILED (mgr->GetSessionEnumerator (&list)))
            continue;
        int n = 0; list->GetCount (&n);
        for (int i = 0; i < n; ++i)
        {
            ComPtr<IAudioSessionControl> c; ComPtr<IAudioSessionControl2> c2; ComPtr<ISimpleAudioVolume> v;
            list->GetSession (i, &c); c.As (&c2); c.As (&v);
            DWORD p = 0; c2->GetProcessId (&p);
            if (p == pid && v) fn (v.Get());
        }
    }
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: ProcessCaptureTest <pid>\n"); return 2; }
    const DWORD pid = (DWORD) std::atoi (argv[1]);
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    std::atomic<double> sumSq { 0 };
    std::atomic<long> count { 0 };
    ProcessCapture cap;
    cap.onAudio = [&] (const float* l, const float* r, int n)
    {
        double s = 0;
        for (int i = 0; i < n; ++i) s += 0.5 * (l[i] * l[i] + r[i] * r[i]);
        sumSq = sumSq + s;
        count += n;
    };
    if (auto err = cap.start (pid); err.isNotEmpty()) { std::printf ("start failed: %s\n", err.toRawUTF8()); return 1; }

    auto measure = [&] (const char* label)
    {
        sumSq = 0; count = 0;
        juce::Thread::sleep (1500);
        const double rms = count > 0 ? std::sqrt (sumSq / (double) count) : 0.0;
        std::printf ("  %-34s captured level %6.1f dBFS (%ld samples)\n", label, 20.0 * std::log10 (rms + 1e-9), (long) count.load());
        return rms;
    };

    juce::Thread::sleep (300);
    const double normal = measure ("app at 100%, unmuted");
    forSessionsOf (pid, [] (ISimpleAudioVolume* v) { v->SetMute (TRUE, nullptr); });
    const double muted = measure ("app MUTED in Windows mixer");
    forSessionsOf (pid, [] (ISimpleAudioVolume* v) { v->SetMute (FALSE, nullptr); v->SetMasterVolume (0.1f, nullptr); });
    const double low = measure ("app volume 10%");
    forSessionsOf (pid, [] (ISimpleAudioVolume* v) { v->SetMasterVolume (1.0f, nullptr); });
    cap.stop();

    auto db = [] (double x) { return 20.0 * std::log10 (x + 1e-9); };
    std::printf ("\nRESULT: capture %s when the app is muted (%.1f dB vs normal), and is %s app volume (10%% -> %.1f dB)\n",
                 muted > normal * 0.5 ? "KEEPS audio" : "goes SILENT", db (muted) - db (normal),
                 std::abs (db (low) - db (normal)) < 3 ? "independent of" : "affected by", db (low) - db (normal));
    CoUninitialize();
    return 0;
}
