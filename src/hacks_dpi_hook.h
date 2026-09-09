#pragma once
#include <cstdint>

// Preferences DPI Boost
// Hijacks GetDeviceCaps(LOGPIXELSX/SY) during Preferences dialog creation
// to make foobar2000 render the Preferences dialog at a higher DPI than
// the system DPI, while leaving the main window DPI untouched.
//
// Boost step: +25% (96->120->144->168->192->216->240)
namespace OpenHacksDpiHook
{
// Install hooks. Returns true on success.
bool Initialize();

// Remove hooks. Safe to call even if not installed.
void Finalize();

// Whether the feature is currently active (enabled by user and hooks installed).
bool IsActive();

// Whether DPI override is currently in effect (i.e. we are inside a
// Preferences dialog creation call).
bool IsOverrideActive();

// For diagnostics: returns the boosted DPI value currently in use, or 0
// when no override is active.
uint32_t CurrentOverrideDPI();
} // namespace OpenHacksDpiHook
