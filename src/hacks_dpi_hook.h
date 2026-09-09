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

// Re-evaluate whether global mode should be active based on the current
// config. Called by the preferences Apply handler when the user toggles the
// global checkbox.
//
// Note: toggling at runtime flips the in-memory flag, but the main window and
// its controls were already laid out during startup (before_ui_init) using
// whatever GlobalDPIOverride value was set then. So the change only takes
// full effect after restarting foobar2000 (at which point before_ui_init
// reads the new value and installs the override before the main window is
// created).
void RefreshGlobalMode();
} // namespace OpenHacksDpiHook
