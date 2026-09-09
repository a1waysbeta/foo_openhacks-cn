#include "pch.h"
#include "hacks_dpi_hook.h"
#include "hacks_vars.h"
#include <minhook.h>
#include <atomic>
#include <cstdint>

namespace
{
// Original function pointers populated by minhook.
// GetDeviceCaps is the DPI query API we override during Preferences creation.
int(WINAPI* OriginGetDeviceCaps)(HDC hdc, int index) = nullptr;

// foobar2000 core creates the Preferences container dialog through one of
// these APIs with owner = main window. We hook them to bracket the creation
// call with GetDeviceCaps override.
INT_PTR(WINAPI* OriginDialogBoxParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
INT_PTR(WINAPI* OriginDialogBoxIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;

// State
std::atomic<bool> gHookInstalled{false};

// thread-local override state: true while we are inside a Preferences dialog
// creation call so that GetDeviceCaps queries for LOGPIXELSX/SY return the
// boosted DPI value.
//
// A depth counter guards against (theoretical) nested main-window-owned
// dialog creation: inner BeginOverride/EndOverride pairs do not perturb the
// outer scope's DPI value.
thread_local uint32_t tOverrideDepth = 0;
thread_local bool tOverrideActive = false;
thread_local uint32_t tOverrideDPI = 0;

// Upper bound for the boost to avoid runaway scaling on extreme DPIs.
constexpr uint32_t kMaxBoostedDPI = 240; // 250%

// Compute the next-step-up DPI value from a given system DPI.
// Steps follow the standard 25% increments:
//   96 (100%) -> 120 (125%) -> 144 (150%) -> 168 (175%) -> 192 (200%) -> 216 (225%) -> 240 (250%)
// Each step is +24 DPI (= +25% of 96).
uint32_t BoostDpi(uint32_t systemDpi)
{
    if (systemDpi == 0)
        systemDpi = USER_DEFAULT_SCREEN_DPI;

    constexpr uint32_t kStep = 24;
    uint32_t boosted = systemDpi + kStep;
    if (boosted > kMaxBoostedDPI)
        boosted = kMaxBoostedDPI;
    return boosted;
}

uint32_t QuerySystemDPI()
{
    if (HDC dc = GetDC(HWND_DESKTOP))
    {
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(HWND_DESKTOP, dc);
        return static_cast<uint32_t>(dpi);
    }
    return USER_DEFAULT_SCREEN_DPI;
}

// Begin a Preferences creation scope: capture system DPI, compute boost,
// set thread-local override. Called right before invoking the original
// dialog creation API.
void BeginOverride()
{
    // Always increment depth so that nested calls are balanced even when the
    // feature is disabled; only the outer-most call applies the override.
    ++tOverrideDepth;
    if (tOverrideDepth > 1)
        return; // inner scope — leave outer override state intact

    if (!OpenHacksVars::PreferencesDPIBoost)
        return;

    // Query real system DPI here. The hook is not yet active (depth was 0
    // before this call), so GetDeviceCaps will call the real implementation.
    const uint32_t sysDpi = QuerySystemDPI();
    tOverrideDPI = BoostDpi(sysDpi);
    tOverrideActive = true;
}

void EndOverride()
{
    if (tOverrideDepth == 0)
        return; // unbalanced — defensive
    --tOverrideDepth;
    if (tOverrideDepth > 0)
        return; // inner scope ending — leave outer state intact

    tOverrideActive = false;
    tOverrideDPI = 0;
}

// Check whether the given owner HWND is the foobar2000 main window.
// We avoid calling core_api::get_main_window() repeatedly here since
// the main window HWND is stable for the process lifetime.
bool IsMainWindowOwner(HWND owner)
{
    if (owner == nullptr)
        return false;
    return owner == core_api::get_main_window();
}

// RAII wrapper that ends override state when destroyed.
class ScopedOverride
{
public:
    ScopedOverride()
    {
        BeginOverride();
    }
    ~ScopedOverride()
    {
        EndOverride();
    }
    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;
};

// Hook implementations
int WINAPI HookGetDeviceCaps(HDC hdc, int index)
{
    if (tOverrideActive && (index == LOGPIXELSX || index == LOGPIXELSY))
    {
        return static_cast<int>(tOverrideDPI);
    }
    return OriginGetDeviceCaps(hdc, index);
}

INT_PTR WINAPI HookDialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent))
    {
        ScopedOverride guard;
        return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

INT_PTR WINAPI HookDialogBoxIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW hDialogTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent))
    {
        ScopedOverride guard;
        return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent))
    {
        ScopedOverride guard;
        return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent))
    {
        ScopedOverride guard;
        return OriginCreateDialogIndirectParamW(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginCreateDialogIndirectParamW(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
}

bool EnableOneHook(void* target, void* detour, void** origin)
{
    if (MH_CreateHook(target, detour, origin) != MH_OK)
        return false;
    return MH_EnableHook(target) == MH_OK;
}

void DisableAllHooks()
{
    if (!gHookInstalled.exchange(false))
        return;

    // Disable (but do not Uninitialize) all hooks. We must not call
    // MH_Uninitialize because the COM module also uses minhook for
    // CLSIDFromProgID interception, and Uninitialize would tear down
    // its hook as well. Disabling our hooks is enough — minhook state
    // will be cleaned up automatically at process exit.
    if (OriginGetDeviceCaps != nullptr)
    {
        std::ignore = MH_DisableHook(&GetDeviceCaps);
        OriginGetDeviceCaps = nullptr;
    }
    if (OriginDialogBoxParamW != nullptr)
    {
        std::ignore = MH_DisableHook(&DialogBoxParamW);
        OriginDialogBoxParamW = nullptr;
    }
    if (OriginDialogBoxIndirectParamW != nullptr)
    {
        std::ignore = MH_DisableHook(&DialogBoxIndirectParamW);
        OriginDialogBoxIndirectParamW = nullptr;
    }
    if (OriginCreateDialogParamW != nullptr)
    {
        std::ignore = MH_DisableHook(&CreateDialogParamW);
        OriginCreateDialogParamW = nullptr;
    }
    if (OriginCreateDialogIndirectParamW != nullptr)
    {
        std::ignore = MH_DisableHook(&CreateDialogIndirectParamW);
        OriginCreateDialogIndirectParamW = nullptr;
    }
}
} // namespace

namespace OpenHacksDpiHook
{
bool Initialize()
{
    if (gHookInstalled.exchange(true))
        return true; // already installed

    // MH_Initialize may have already been called by the COM module
    // (CLSIDFromProgID hook). Calling it again returns
    // MH_ERROR_INITIALIZED, which we treat as success. Any other error
    // is fatal — we cannot proceed without minhook.
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_INITIALIZED)
    {
        gHookInstalled = false;
        return false;
    }

    bool ok = true;
    ok &= EnableOneHook(&GetDeviceCaps, &HookGetDeviceCaps, reinterpret_cast<void**>(&OriginGetDeviceCaps));
    ok &= EnableOneHook(&DialogBoxParamW, &HookDialogBoxParamW, reinterpret_cast<void**>(&OriginDialogBoxParamW));
    ok &= EnableOneHook(&DialogBoxIndirectParamW, &HookDialogBoxIndirectParamW,
                        reinterpret_cast<void**>(&OriginDialogBoxIndirectParamW));
    ok &= EnableOneHook(&CreateDialogParamW, &HookCreateDialogParamW, reinterpret_cast<void**>(&OriginCreateDialogParamW));
    ok &= EnableOneHook(&CreateDialogIndirectParamW, &HookCreateDialogIndirectParamW,
                        reinterpret_cast<void**>(&OriginCreateDialogIndirectParamW));

    if (!ok)
    {
        DisableAllHooks();
        return false;
    }
    return true;
}

void Finalize()
{
    DisableAllHooks();
}

bool IsActive()
{
    return gHookInstalled.load() && OpenHacksVars::PreferencesDPIBoost;
}

bool IsOverrideActive()
{
    return tOverrideActive;
}

uint32_t CurrentOverrideDPI()
{
    return tOverrideActive ? tOverrideDPI : 0;
}
} // namespace OpenHacksDpiHook
