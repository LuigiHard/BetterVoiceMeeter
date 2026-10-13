#include "AppOutputRouting.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl/client.h>
#pragma comment(lib, "runtimeobject.lib")

using Microsoft::WRL::ComPtr;

namespace
{
// Windows.Media.Internal.AudioPolicyConfig factory, Windows 10 21H2+ / 11 layout
// (interface layout as used by EarTrumpet: github.com/File-New-Project/EarTrumpet).
MIDL_INTERFACE ("ab3d4648-e242-459f-b02f-541c70306324")
IAudioPolicyConfigFactory : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE add_CtxVolumeChange() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_CtxVolumeChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_RingerVibrateStateChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_RingerVibrateStateChange() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolumeGroupGainForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupGainForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetActiveVolumeGroupForEndpointId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupsForEndpoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentVolumeContext() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolumeGroupMuteForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupMuteForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRingerVibrateState() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRingerVibrateState() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentChatApplications() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ChatContextChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ChatContextChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint (UINT processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint (UINT processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

// The policy API wants the device-interface path, not the bare MMDevice id.
const wchar_t* mmdevapiToken = L"\\\\?\\SWD#MMDEVAPI#";
const wchar_t* renderInterface = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

ComPtr<IAudioPolicyConfigFactory> factory()
{
    static const wchar_t* name = L"Windows.Media.Internal.AudioPolicyConfig";
    HSTRING cls = nullptr;
    if (FAILED (WindowsCreateString (name, (UINT32) wcslen (name), &cls)))
        return {};
    ComPtr<IAudioPolicyConfigFactory> f;
    RoGetActivationFactory (cls, __uuidof (IAudioPolicyConfigFactory), &f);
    WindowsDeleteString (cls);
    return f;
}
} // namespace

bool AppOutputRouting::isAvailable() { return factory() != nullptr; }

juce::String AppOutputRouting::findPlaybackDeviceId (const juce::String& needle)
{
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en)))
        || FAILED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &list)))
        return {};
    UINT count = 0;
    list->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> d;
        ComPtr<IPropertyStore> props;
        if (FAILED (list->Item (i, &d)) || FAILED (d->OpenPropertyStore (STGM_READ, &props)))
            continue;
        PROPVARIANT v;
        PropVariantInit (&v);
        juce::String name;
        if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
            name = juce::String (v.pwszVal);
        PropVariantClear (&v);
        if (! name.containsIgnoreCase (needle))
            continue;
        LPWSTR id = nullptr;
        juce::String result;
        if (SUCCEEDED (d->GetId (&id)))
        {
            result = juce::String (id);
            CoTaskMemFree (id);
        }
        return result;
    }
    return {};
}

juce::String AppOutputRouting::getAppOutput (unsigned long pid)
{
    auto f = factory();
    if (f == nullptr) return {};
    HSTRING s = nullptr;
    if (FAILED (f->GetPersistedDefaultAudioEndpoint ((UINT) pid, eRender, eMultimedia, &s)) || s == nullptr)
        return {};
    UINT32 len = 0;
    juce::String path (WindowsGetStringRawBuffer (s, &len));
    WindowsDeleteString (s);
    // "\\?\SWD#MMDEVAPI#{id}#{interface}" -> "{id}"
    return path.fromFirstOccurrenceOf ("MMDEVAPI#", false, false).upToLastOccurrenceOf ("#{", false, false);
}

bool AppOutputRouting::setAppOutput (unsigned long pid, const juce::String& endpointId)
{
    auto f = factory();
    if (f == nullptr) return false;
    HSTRING s = nullptr;
    if (endpointId.isNotEmpty())
    {
        const juce::String path = juce::String (mmdevapiToken) + endpointId + renderInterface;
        if (FAILED (WindowsCreateString (path.toWideCharPointer(), (UINT32) path.length(), &s)))
            return false;
    }
    const bool ok = SUCCEEDED (f->SetPersistedDefaultAudioEndpoint ((UINT) pid, eRender, eMultimedia, s))
                 && SUCCEEDED (f->SetPersistedDefaultAudioEndpoint ((UINT) pid, eRender, eConsole, s));
    if (s != nullptr) WindowsDeleteString (s);
    return ok;
}
