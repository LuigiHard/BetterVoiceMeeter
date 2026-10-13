#include "AppSessions.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <wrl/client.h>
#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;

namespace
{
juce::String deviceName (IMMDevice* d)
{
    ComPtr<IPropertyStore> props;
    if (FAILED (d->OpenPropertyStore (STGM_READ, &props))) return {};
    PROPVARIANT v;
    PropVariantInit (&v);
    juce::String name;
    if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        name = juce::String (v.pwszVal);
    PropVariantClear (&v);
    // "Alto-falantes (Focusrite USB Audio)" -> "Focusrite USB Audio"
    if (name.containsChar ('(') && name.endsWithChar (')'))
        name = name.fromFirstOccurrenceOf ("(", false, false).dropLastCharacters (1);
    return name;
}

juce::String processPath (DWORD pid)
{
    HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = (DWORD) std::size (buf);
    juce::String path;
    if (QueryFullProcessImageNameW (h, 0, buf, &len))
        path = juce::String (buf, (size_t) len);
    CloseHandle (h);
    return path;
}

// "Google Chrome" from the exe's version info, else the file name.
juce::String friendlyName (const juce::String& path)
{
    const auto file = juce::File (path);
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW (path.toWideCharPointer(), &dummy);
    if (size > 0)
    {
        juce::HeapBlock<char> data (size);
        if (GetFileVersionInfoW (path.toWideCharPointer(), 0, size, data))
        {
            struct LangCode { WORD lang, code; }* langs = nullptr;
            UINT bytes = 0;
            if (VerQueryValueW (data, L"\\VarFileInfo\\Translation", (LPVOID*) &langs, &bytes) && bytes >= sizeof (LangCode))
            {
                wchar_t key[64];
                swprintf_s (key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].code);
                wchar_t* desc = nullptr;
                UINT descLen = 0;
                if (VerQueryValueW (data, key, (LPVOID*) &desc, &descLen) && descLen > 1)
                {
                    auto s = juce::String (desc).trim();
                    if (s.isNotEmpty() && s.length() < 48) return s;
                }
            }
        }
    }
    return file.getFileNameWithoutExtension();
}

// Exe icon -> juce::Image (32x32, with alpha).
juce::Image iconFor (const juce::String& path)
{
    HICON hicon = nullptr;
    if (ExtractIconExW (path.toWideCharPointer(), 0, &hicon, nullptr, 1) == 0 || hicon == nullptr)
        return {};
    constexpr int size = 32;
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC (nullptr);
    HBITMAP bmp = CreateDIBSection (dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    juce::Image img;
    if (bmp != nullptr && bits != nullptr)
    {
        auto old = SelectObject (dc, bmp);
        memset (bits, 0, size * size * 4);
        DrawIconEx (dc, 0, 0, hicon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject (dc, old);
        img = juce::Image (juce::Image::ARGB, size, size, true);
        juce::Image::BitmapData out (img, juce::Image::BitmapData::writeOnly);
        auto* px = static_cast<const uint8_t*> (bits);
        bool anyAlpha = false;
        for (int i = 0; i < size * size; ++i) anyAlpha |= px[i * 4 + 3] != 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto* p = px + (y * size + x) * 4;
                const uint8_t a = anyAlpha ? p[3] : (p[0] | p[1] | p[2]) ? 255 : 0; // old icons without alpha
                out.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0], a));
            }
    }
    if (bmp) DeleteObject (bmp);
    DeleteDC (dc);
    DestroyIcon (hicon);
    return img;
}
} // namespace

struct AppSessions::Session
{
    ComPtr<IAudioSessionControl2> control;
    ComPtr<ISimpleAudioVolume> volume;
    ComPtr<IAudioMeterInformation> meter;
    juce::String key, device;
};

AppSessions::AppSessions() : juce::Thread ("App sessions") {}

AppSessions::~AppSessions() { stop(); }

void AppSessions::start() { if (! isThreadRunning()) startThread(); }

void AppSessions::stop() { stopThread (2000); }

std::vector<AppSessions::App> AppSessions::snapshot() const
{
    std::lock_guard<std::mutex> g (lock);
    return published;
}

void AppSessions::setVolume (const juce::String& key, float v)
{
    std::lock_guard<std::mutex> g (lock);
    commands.emplace_back (key, 0, juce::jlimit (0.0f, 1.0f, v));
}

void AppSessions::setMute (const juce::String& key, bool m)
{
    std::lock_guard<std::mutex> g (lock);
    commands.emplace_back (key, 1, m ? 1.0f : 0.0f);
}

void AppSessions::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    juce::uint32 lastScan = 0;
    while (! threadShouldExit())
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (lastScan == 0 || now - lastScan > 1500)
        {
            rescan();
            lastScan = now;
        }
        applyCommands();
        poll();
        wait (50);
    }
    sessions.clear();
    CoUninitialize();
}

void AppSessions::rescan()
{
    sessions.clear();
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en))))
        return;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED (en->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
        return;
    UINT count = 0;
    devices->GetCount (&count);
    for (UINT d = 0; d < count; ++d)
    {
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioSessionManager2> mgr;
        ComPtr<IAudioSessionEnumerator> list;
        if (FAILED (devices->Item (d, &dev))
            || FAILED (dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr))
            || FAILED (mgr->GetSessionEnumerator (&list)))
            continue;
        const auto devName = deviceName (dev.Get());
        int n = 0;
        list->GetCount (&n);
        for (int i = 0; i < n; ++i)
        {
            ComPtr<IAudioSessionControl> c1;
            auto s = std::make_unique<Session>();
            if (FAILED (list->GetSession (i, &c1)) || FAILED (c1.As (&s->control)))
                continue;
            AudioSessionState state;
            if (SUCCEEDED (s->control->GetState (&state)) && state == AudioSessionStateExpired)
                continue;
            if (s->control->IsSystemSoundsSession() == S_OK)
                s->key = "system";
            else
            {
                DWORD pid = 0;
                s->control->GetProcessId (&pid);
                const auto path = processPath (pid);
                if (path.isEmpty()) continue;
                s->key = path.toLowerCase();
                if (info.find (s->key) == info.end())
                {
                    App a;
                    a.key = s->key;
                    a.name = friendlyName (path);
                    a.icon = iconFor (path);
                    info[s->key] = a;
                }
            }
            if (s->key == "system" && info.find ("system") == info.end())
            {
                App a;
                a.key = "system";
                a.name = "System sounds";
                info["system"] = a;
            }
            c1.As (&s->volume);
            c1.As (&s->meter);
            s->device = devName;
            sessions.push_back (std::move (s));
        }
    }
}

void AppSessions::applyCommands()
{
    std::vector<std::tuple<juce::String, int, float>> todo;
    {
        std::lock_guard<std::mutex> g (lock);
        todo.swap (commands);
    }
    for (auto& [key, kind, value] : todo)
        for (auto& s : sessions)
            if (s->key == key && s->volume)
            {
                if (kind == 0) s->volume->SetMasterVolume (value, nullptr);
                else s->volume->SetMute (value > 0.5f, nullptr);
            }
}

void AppSessions::poll()
{
    std::map<juce::String, App> apps;
    std::vector<juce::String> order;
    for (auto& s : sessions)
    {
        auto it = apps.find (s->key);
        if (it == apps.end())
        {
            App a = info[s->key];
            a.devices.clear();
            a.peak = 0;
            a.active = false;
            if (s->volume)
            {
                float v = 1.0f;
                BOOL m = FALSE;
                s->volume->GetMasterVolume (&v);
                s->volume->GetMute (&m);
                a.volume = v;
                a.mute = m != FALSE;
            }
            it = apps.emplace (s->key, a).first;
            order.push_back (s->key);
        }
        auto& a = it->second;
        if (! a.devices.contains (s->device))
            a.devices += (a.devices.isEmpty() ? "" : ", ") + s->device;
        AudioSessionState state;
        if (SUCCEEDED (s->control->GetState (&state)) && state == AudioSessionStateActive)
            a.active = true;
        float pk = 0;
        if (s->meter && SUCCEEDED (s->meter->GetPeakValue (&pk)))
            a.peak = juce::jmax (a.peak, pk);
    }
    std::vector<App> out;
    for (auto& k : order)
        out.push_back (apps[k]);
    // Playing apps first, system sounds last, otherwise alphabetical (stable while the panel is open).
    std::stable_sort (out.begin(), out.end(), [] (const App& a, const App& b)
    {
        if ((a.key == "system") != (b.key == "system")) return b.key == "system";
        return a.name.compareIgnoreCase (b.name) < 0;
    });
    std::lock_guard<std::mutex> g (lock);
    published = std::move (out);
}
