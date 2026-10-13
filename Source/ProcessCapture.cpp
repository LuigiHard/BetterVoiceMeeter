#include "ProcessCapture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#pragma comment(lib, "mmdevapi.lib")

using Microsoft::WRL::ComPtr;

// Declared in audioclientactivationparams.h (Windows SDK 10.0.20348+); defined here for older SDKs.
namespace pl
{
enum ActivationType { activationDefault = 0, activationProcessLoopback = 1 };
enum LoopbackMode { includeTargetProcessTree = 0, excludeTargetProcessTree = 1 };
struct ProcessLoopbackParams { DWORD targetProcessId; LoopbackMode mode; };
struct ActivationParams { ActivationType type; ProcessLoopbackParams loopback; };
constexpr const wchar_t* virtualDevice = L"VAD\\Process_Loopback";
} // namespace pl

namespace
{
class Completion : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                      Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
    Completion() { done = CreateEventW (nullptr, TRUE, FALSE, nullptr); }
    ~Completion() override { CloseHandle (done); }

    STDMETHOD (ActivateCompleted) (IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT hrActivate = E_FAIL;
        ComPtr<IUnknown> unk;
        result = op->GetActivateResult (&hrActivate, &unk);
        if (SUCCEEDED (result)) result = hrActivate;
        if (SUCCEEDED (result)) result = unk.As (&client);
        SetEvent (done);
        return S_OK;
    }

    HANDLE done;
    HRESULT result = E_FAIL;
    ComPtr<IAudioClient> client;
};
} // namespace

ProcessCapture::ProcessCapture() : juce::Thread ("App capture") {}

ProcessCapture::~ProcessCapture() { stop(); }

juce::String ProcessCapture::start (unsigned long processId)
{
    stop();
    pid = processId;
    startError.clear();
    started.reset();
    startThread (juce::Thread::Priority::highest);
    started.wait (5000);
    if (startError.isNotEmpty())
        stop();
    return startError;
}

void ProcessCapture::stop()
{
    stopThread (2000);
}

void ProcessCapture::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    auto fail = [this] (const juce::String& e) { startError = e; started.signal(); };

    pl::ActivationParams params { pl::activationProcessLoopback, { (DWORD) pid, pl::includeTargetProcessTree } };
    PROPVARIANT pv;
    PropVariantInit (&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof (params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*> (&params);

    auto handler = Microsoft::WRL::Make<Completion>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync (pl::virtualDevice, __uuidof (IAudioClient), &pv, handler.Get(), &op);
    if (FAILED (hr) || WaitForSingleObject (handler->done, 5000) != WAIT_OBJECT_0 || FAILED (handler->result))
    {
        CoUninitialize();
        return fail ("This app's audio can't be captured (process loopback unavailable, error 0x"
                     + juce::String::toHexString ((int) (FAILED (hr) ? hr : handler->result)) + ")");
    }
    auto client = handler->client;

    WAVEFORMATEX fmt {};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD) sampleRate;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    HANDLE event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             200000 /* 20 ms */, 0, &fmt, nullptr);
    ComPtr<IAudioCaptureClient> capture;
    if (FAILED (hr) || FAILED (client->SetEventHandle (event)) || FAILED (client->GetService (IID_PPV_ARGS (&capture)))
        || FAILED (client->Start()))
    {
        CloseHandle (event);
        CoUninitialize();
        return fail ("Could not start app capture (0x" + juce::String::toHexString ((int) hr) + ")");
    }
    started.signal();

    std::vector<float> l (8192), r (8192);
    while (! threadShouldExit())
    {
        if (WaitForSingleObject (event, 100) != WAIT_OBJECT_0)
            continue;
        UINT32 packet = 0;
        while (SUCCEEDED (capture->GetNextPacketSize (&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED (capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                break;
            const int n = (int) juce::jmin<UINT32> (frames, (UINT32) l.size());
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
            {
                std::fill (l.begin(), l.begin() + n, 0.0f);
                std::fill (r.begin(), r.begin() + n, 0.0f);
            }
            else
            {
                auto* s = reinterpret_cast<const float*> (data);
                for (int i = 0; i < n; ++i) { l[(size_t) i] = s[i * 2]; r[(size_t) i] = s[i * 2 + 1]; }
            }
            capture->ReleaseBuffer (frames);
            if (onAudio) onAudio (l.data(), r.data(), n);
        }
    }
    client->Stop();
    CloseHandle (event);
    CoUninitialize();
}
