#pragma once
#include <JuceHeader.h>

// Per-app output device, the same setting as Windows Settings > System > Sound > Volume mixer >
// [app] > Output device. Uses Windows' undocumented AudioPolicyConfig interface (as EarTrumpet does);
// available on Windows 10 21H2+ / Windows 11. All calls must run on a COM (MTA) thread.
namespace AppOutputRouting
{
bool isAvailable();

// Endpoint ID of a playback device (IMMDevice::GetId) by its friendly name; empty if not found.
juce::String findPlaybackDeviceId (const juce::String& friendlyNameContains);

// The app's chosen output device (empty = follows the Windows default).
juce::String getAppOutput (unsigned long processId);

// Sets (or, with an empty id, clears) the app's output device. Returns false on failure.
bool setAppOutput (unsigned long processId, const juce::String& endpointId);
} // namespace AppOutputRouting
