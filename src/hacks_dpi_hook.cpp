#include "pch.h"
#include "hacks_dpi_hook.h"
#include "hacks_vars.h"
#include <minhook.h>
#include <atomic>
#include <cstdint>

namespace
{
// ============================================================================
// DPI override strategy
//
// We hook every public Win32 API that returns screen DPI or DPI-derived
// metrics. When our override is active (globally or scoped to Preferences
// creation), these APIs return the user-specified DPI instead of the real
// system DPI.
//
// Hooked APIs:
//   * GetDeviceCaps(hdc, LOGPIXELSX/SY)        - legacy DPI query
//   * GetDpiForWindow(hwnd)                    - Win10 1607+ per-window DPI
//   * GetDpiForSystem()                        - Win10 1607+ system DPI
//   * GetDpiForMonitor(hmon, type, *x, *y)     - shcore.dll, monitor DPI
//   * SystemParametersInfoW(SPI_GETNONCLIENTMETRICS) - system font metrics
//   * CreateFontIndirectW/ExW                  - re-scale lfHeight
//
// We cannot change the Windows-internal process DPI cache (set at process
// start), so DLU calculations done by CreateDialogParam may still use the
// real DPI. The combination of GetDeviceCaps + CreateFontIndirect hooks
// covers the visible code paths that apps (including foobar2000 core and
// third-party preference pages) actually use to query DPI and create fonts.
// ============================================================================

// Local declaration of GetDpiForMonitor signature (avoid pulling in
// shellscalingapi.h which is gated on NTDDI_WINBLUE and may be missing
// from some SDK configurations).
enum LocalMonitorDpiType { LocalMDT_Default, LocalMDT_Angular, LocalMDT_Raw };
using LocalGetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, LocalMonitorDpiType, UINT*, UINT*);

// ---- Original function pointers ----
int(WINAPI* OriginGetDeviceCaps)(HDC hdc, int index) = nullptr;
UINT(WINAPI* OriginGetDpiForWindow)(HWND hwnd) = nullptr;
UINT(WINAPI* OriginGetDpiForSystem)(void) = nullptr;
LocalGetDpiForMonitorFn OriginGetDpiForMonitor = nullptr;
BOOL(WINAPI* OriginSystemParametersInfoW)(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni) = nullptr;
HFONT(WINAPI* OriginCreateFontIndirectW)(LOGFONTW* lplf) = nullptr;
HFONT(WINAPI* OriginCreateFontIndirectExW)(ENUMLOGFONTEXW* lpelfe, DWORD fdwStyle) = nullptr;

// Dialog creation APIs that bracket the override scope (Preferences mode only).
INT_PTR(WINAPI* OriginDialogBoxParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
INT_PTR(WINAPI* OriginDialogBoxIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;

// ---- State ----
std::atomic<bool> gHookInstalled{false};

// thread-local scope state. In Preferences mode, the override is only active
// while we are inside a Preferences dialog creation call. In Global mode,
// gGlobalActive is set once at startup and stays on for the process lifetime.
thread_local uint32_t tOverrideDepth = 0;
thread_local bool tOverrideActive = false;
std::atomic<bool> gGlobalActive{false};

// Cached system DPI (the real value, captured once at install time).
uint32_t gRealSystemDPI = USER_DEFAULT_SCREEN_DPI;

uint32_t QuerySystemDPIReal()
{
    if (HDC dc = GetDC(HWND_DESKTOP))
    {
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(HWND_DESKTOP, dc);
        return static_cast<uint32_t>(dpi);
    }
    return USER_DEFAULT_SCREEN_DPI;
}

// Read the user-configured override DPI value. 0 means "use real DPI" (no
// override). We clamp to the standard range to avoid nonsensical values.
uint32_t GetOverrideDPI()
{
    int32_t v = OpenHacksVars::DPIOverrideValue;
    if (v < 96) v = 96;
    if (v > 240) v = 240;
    return static_cast<uint32_t>(v);
}

// Whether the override should be in effect right now on the current thread.
// Active when (Global mode on) OR (Preferences mode on AND we are inside
// a Preferences dialog creation scope).
inline bool IsOverrideActive()
{
    return gGlobalActive.load(std::memory_order_relaxed) || tOverrideActive;
}

// Returns the DPI value to report when overriding.
inline uint32_t ReportDPI()
{
    return GetOverrideDPI();
}

// ---- Preferences-scoped RAII override ----
void BeginPreferencesOverride()
{
    ++tOverrideDepth;
    if (tOverrideDepth > 1)
        return; // nested scope — leave outer state intact

    if (!OpenHacksVars::PreferencesDPIBoost)
        return; // feature disabled by user

    tOverrideActive = true;
}

void EndPreferencesOverride()
{
    if (tOverrideDepth == 0)
        return;
    --tOverrideDepth;
    if (tOverrideDepth > 0)
        return;

    tOverrideActive = false;
}

class ScopedPreferencesOverride
{
public:
    ScopedPreferencesOverride() { BeginPreferencesOverride(); }
    ~ScopedPreferencesOverride() { EndPreferencesOverride(); }
    ScopedPreferencesOverride(const ScopedPreferencesOverride&) = delete;
    ScopedPreferencesOverride& operator=(const ScopedPreferencesOverride&) = delete;
};

bool IsMainWindowOwner(HWND owner)
{
    if (owner == nullptr)
        return false;
    return owner == core_api::get_main_window();
}

// ============================================================================
// Dialog template inspection — detect Preferences by SysTreeView32 presence
// ============================================================================
bool TemplateHasTreeViewControl(LPCDLGTEMPLATEW pTemplate)
{
    if (!pTemplate)
        return false;

    const WORD* pw = reinterpret_cast<const WORD*>(pTemplate);
    const bool isEx = (pw[0] == 1 && pw[1] == 0xFFFF);

    WORD cItems;
    DWORD style;
    const WORD* pCursor = pw;

    if (isEx)
    {
        style = static_cast<DWORD>(pw[6]) | (static_cast<DWORD>(pw[7]) << 16);
        cItems = pw[8];
        pCursor = pw + 13;
    }
    else
    {
        style = static_cast<DWORD>(pw[0]) | (static_cast<DWORD>(pw[1]) << 16);
        cItems = pw[4];
        pCursor = pw + 9;
    }

    auto SkipMenuOrClass = [](const WORD*& p) {
        if (*p == 0) { p += 1; }
        else if (*p == 0xFFFF) { p += 2; }
        else { while (*p != 0) ++p; ++p; }
    };
    auto SkipTitle = [](const WORD*& p) {
        while (*p != 0) ++p; ++p;
    };

    SkipMenuOrClass(pCursor);
    SkipMenuOrClass(pCursor);
    SkipTitle(pCursor);

    if (style & DS_SETFONT)
    {
        if (isEx)
        {
            pCursor += 1; // pointSize
            pCursor += 1; // weight
            pCursor += 1; // italic + charset
            SkipTitle(pCursor);
        }
        else
        {
            pCursor += 1;
            SkipTitle(pCursor);
        }
    }

    for (WORD i = 0; i < cItems; ++i)
    {
        pCursor = reinterpret_cast<const WORD*>(
            (reinterpret_cast<uintptr_t>(pCursor) + 3) & ~static_cast<uintptr_t>(3));

        if (isEx) pCursor += 12;
        else      pCursor += 10;

        if (*pCursor == 0xFFFF)
        {
            pCursor += 2;
        }
        else
        {
            const WCHAR* className = reinterpret_cast<const WCHAR*>(pCursor);
            static const WCHAR kNeedle[] = L"SysTreeView32";
            const WCHAR* np = kNeedle;
            const WCHAR* cp = className;
            bool match = true;
            while (*np)
            {
                WCHAR a = *cp;
                WCHAR b = *np;
                if (a >= L'A' && a <= L'Z') a = static_cast<WCHAR>(a + 32);
                if (b >= L'A' && b <= L'Z') b = static_cast<WCHAR>(b + 32);
                if (a != b) { match = false; break; }
                ++cp; ++np;
            }
            if (match && *cp == 0)
                return true;
            while (*pCursor != 0) ++pCursor;
            ++pCursor;
        }

        if (*pCursor == 0xFFFF) pCursor += 2;
        else                    SkipTitle(pCursor);

        WORD cbExtra = *pCursor;
        ++pCursor;
        pCursor += (cbExtra + 1) / 2;
    }
    return false;
}

bool IsPreferencesDialogResource(HINSTANCE hInstance, LPCWSTR name)
{
    if (!name) return false;
    HRSRC hRes = FindResourceW(hInstance, name, RT_DIALOG);
    if (!hRes) return false;
    HGLOBAL hLoad = LoadResource(hInstance, hRes);
    if (!hLoad) return false;
    LPCDLGTEMPLATEW pTemplate = reinterpret_cast<LPCDLGTEMPLATEW>(LockResource(hLoad));
    if (!pTemplate) return false;
    return TemplateHasTreeViewControl(pTemplate);
}

bool IsPreferencesDialogTemplateIndirect(LPCDLGTEMPLATEW pTemplate)
{
    return pTemplate && TemplateHasTreeViewControl(pTemplate);
}

// ============================================================================
// Hook implementations
// ============================================================================

int WINAPI HookGetDeviceCaps(HDC hdc, int index)
{
    if (IsOverrideActive() && (index == LOGPIXELSX || index == LOGPIXELSY))
    {
        return static_cast<int>(ReportDPI());
    }
    return OriginGetDeviceCaps(hdc, index);
}

UINT WINAPI HookGetDpiForWindow(HWND hwnd)
{
    if (IsOverrideActive())
    {
        return ReportDPI();
    }
    return OriginGetDpiForWindow(hwnd);
}

UINT WINAPI HookGetDpiForSystem()
{
    if (IsOverrideActive())
    {
        return ReportDPI();
    }
    return OriginGetDpiForSystem();
}

HRESULT WINAPI HookGetDpiForMonitor(HMONITOR hmon, LocalMonitorDpiType type, UINT* x, UINT* y)
{
    if (IsOverrideActive())
    {
        UINT v = ReportDPI();
        if (x) *x = v;
        if (y) *y = v;
        return S_OK;
    }
    return OriginGetDpiForMonitor(hmon, type, x, y);
}

BOOL WINAPI HookSystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
    BOOL ret = OriginSystemParametersInfoW(uiAction, uiParam, pvParam, fWinIni);
    if (ret && IsOverrideActive() && pvParam != nullptr)
    {
        // Re-scale font heights inside NONCLIENTMETRICS / ICONMETRICS to the
        // override DPI so callers (including foobar2000 core) build fonts at
        // the boosted size.
        const uint32_t target = ReportDPI();
        if (uiAction == SPI_GETNONCLIENTMETRICS)
        {
            NONCLIENTMETRICSW* m = static_cast<NONCLIENTMETRICSW*>(pvParam);
            auto scale = [&](LONG& h) {
                if (h == 0) return;
                LONG scaled = static_cast<LONG>(MulDiv(static_cast<int>(h < 0 ? -h : h),
                                                       static_cast<int>(target),
                                                       static_cast<int>(gRealSystemDPI)));
                h = h < 0 ? -scaled : scaled;
            };
            scale(m->lfCaptionFont.lfHeight);
            scale(m->lfSmCaptionFont.lfHeight);
            scale(m->lfMenuFont.lfHeight);
            scale(m->lfStatusFont.lfHeight);
            scale(m->lfMessageFont.lfHeight);
        }
        else if (uiAction == SPI_GETICONMETRICS)
        {
            ICONMETRICSW* m = static_cast<ICONMETRICSW*>(pvParam);
            if (m->lfFont.lfHeight != 0)
            {
                LONG scaled = static_cast<LONG>(MulDiv(static_cast<int>(m->lfFont.lfHeight < 0 ? -m->lfFont.lfHeight : m->lfFont.lfHeight),
                                                       static_cast<int>(target),
                                                       static_cast<int>(gRealSystemDPI)));
                m->lfFont.lfHeight = m->lfFont.lfHeight < 0 ? -scaled : scaled;
            }
        }
    }
    return ret;
}

// Re-scale LOGFONT.lfHeight to the override DPI.
void RescaleLogFont(LOGFONTW& lf)
{
    if (lf.lfHeight == 0 || gRealSystemDPI == 0)
        return;
    const uint32_t target = ReportDPI();
    LONG scaled = static_cast<LONG>(MulDiv(static_cast<int>(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight),
                                            static_cast<int>(target),
                                            static_cast<int>(gRealSystemDPI)));
    lf.lfHeight = lf.lfHeight < 0 ? -scaled : scaled;
}

HFONT WINAPI HookCreateFontIndirectW(LOGFONTW* lplf)
{
    if (IsOverrideActive() && lplf != nullptr)
    {
        RescaleLogFont(*lplf);
    }
    return OriginCreateFontIndirectW(lplf);
}

HFONT WINAPI HookCreateFontIndirectExW(ENUMLOGFONTEXW* lpelfe, DWORD fdwStyle)
{
    if (IsOverrideActive() && lpelfe != nullptr)
    {
        RescaleLogFont(lpelfe->elfLogFont);
    }
    return OriginCreateFontIndirectExW(lpelfe, fdwStyle);
}

INT_PTR WINAPI HookDialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (!gGlobalActive.load() && IsMainWindowOwner(hWndParent) && IsPreferencesDialogResource(hInstance, lpTemplateName))
    {
        ScopedPreferencesOverride guard;
        return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

INT_PTR WINAPI HookDialogBoxIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW hDialogTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (!gGlobalActive.load() && IsMainWindowOwner(hWndParent) && IsPreferencesDialogTemplateIndirect(hDialogTemplate))
    {
        ScopedPreferencesOverride guard;
        return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (!gGlobalActive.load() && IsMainWindowOwner(hWndParent) && IsPreferencesDialogResource(hInstance, lpTemplateName))
    {
        ScopedPreferencesOverride guard;
        return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (!gGlobalActive.load() && IsMainWindowOwner(hWndParent) && IsPreferencesDialogTemplateIndirect(lpTemplate))
    {
        ScopedPreferencesOverride guard;
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

    // Disable ALL minhook-installed hooks at once. This is safer than
    // disabling by the public export address, because for APIs resolved
    // via GetProcAddress (GetDpiForWindow etc.) we cannot reliably recover
    // the original target address after the fact.
    (void)MH_DisableHook(MH_ALL_HOOKS);

    OriginGetDeviceCaps = nullptr;
    OriginCreateFontIndirectW = nullptr;
    OriginCreateFontIndirectExW = nullptr;
    OriginDialogBoxParamW = nullptr;
    OriginDialogBoxIndirectParamW = nullptr;
    OriginCreateDialogParamW = nullptr;
    OriginCreateDialogIndirectParamW = nullptr;
    OriginGetDpiForWindow = nullptr;
    OriginGetDpiForSystem = nullptr;
    OriginGetDpiForMonitor = nullptr;
    OriginSystemParametersInfoW = nullptr;
}
} // namespace

namespace OpenHacksDpiHook
{
bool Initialize()
{
    if (gHookInstalled.exchange(true))
        return true;

    gRealSystemDPI = QuerySystemDPIReal();

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        gHookInstalled = false;
        return false;
    }

    bool ok = true;

    // Always-installed hooks (legacy APIs available on all supported Windows).
    // Use EnableOneHook + reinterpret_cast because some SDK functions have
    // const-qualified parameters (e.g. CreateFontIndirectW takes const LOGFONTW*),
    // which would make a type-deducing template ambiguous.
    ok &= EnableOneHook(reinterpret_cast<void*>(&GetDeviceCaps),
                        reinterpret_cast<void*>(&HookGetDeviceCaps),
                        reinterpret_cast<void**>(&OriginGetDeviceCaps));
    ok &= EnableOneHook(reinterpret_cast<void*>(&CreateFontIndirectW),
                        reinterpret_cast<void*>(&HookCreateFontIndirectW),
                        reinterpret_cast<void**>(&OriginCreateFontIndirectW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&CreateFontIndirectExW),
                        reinterpret_cast<void*>(&HookCreateFontIndirectExW),
                        reinterpret_cast<void**>(&OriginCreateFontIndirectExW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&DialogBoxParamW),
                        reinterpret_cast<void*>(&HookDialogBoxParamW),
                        reinterpret_cast<void**>(&OriginDialogBoxParamW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&DialogBoxIndirectParamW),
                        reinterpret_cast<void*>(&HookDialogBoxIndirectParamW),
                        reinterpret_cast<void**>(&OriginDialogBoxIndirectParamW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&CreateDialogParamW),
                        reinterpret_cast<void*>(&HookCreateDialogParamW),
                        reinterpret_cast<void**>(&OriginCreateDialogParamW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&CreateDialogIndirectParamW),
                        reinterpret_cast<void*>(&HookCreateDialogIndirectParamW),
                        reinterpret_cast<void**>(&OriginCreateDialogIndirectParamW));
    ok &= EnableOneHook(reinterpret_cast<void*>(&SystemParametersInfoW),
                        reinterpret_cast<void*>(&HookSystemParametersInfoW),
                        reinterpret_cast<void**>(&OriginSystemParametersInfoW));

    // Optional modern APIs. These are exported by user32.dll on Win10 1607+
    // and may be missing on older Windows. Failure to hook them is non-fatal;
    // the legacy GetDeviceCaps path covers the same logical query.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        auto tryHookUser32 = [&](const char* name, void* detour, void** origin) -> bool {
            FARPROC p = GetProcAddress(user32, name);
            if (!p) return true; // API absent — treat as success (skip)
            return EnableOneHook(reinterpret_cast<void*>(p), detour, origin);
        };
        ok &= tryHookUser32("GetDpiForWindow", reinterpret_cast<void*>(&HookGetDpiForWindow),
                            reinterpret_cast<void**>(&OriginGetDpiForWindow));
        ok &= tryHookUser32("GetDpiForSystem", reinterpret_cast<void*>(&HookGetDpiForSystem),
                            reinterpret_cast<void**>(&OriginGetDpiForSystem));
    }

    // GetDpiForMonitor lives in shcore.dll.
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr)
    {
        auto p = GetProcAddress(shcore, "GetDpiForMonitor");
        if (p != nullptr)
        {
            ok &= EnableOneHook(reinterpret_cast<void*>(p),
                                reinterpret_cast<void*>(&HookGetDpiForMonitor),
                                reinterpret_cast<void**>(&OriginGetDpiForMonitor));
        }
    }

    if (!ok)
    {
        DisableAllHooks();
        return false;
    }

    // If global mode is configured, enable it now. (Hooks must already be
    // installed because the override state is consulted by every hook.)
    if (OpenHacksVars::GlobalDPIOverride)
    {
        gGlobalActive.store(true, std::memory_order_relaxed);
    }
    return true;
}

void Finalize()
{
    gGlobalActive.store(false, std::memory_order_relaxed);
    DisableAllHooks();
}

bool IsActive()
{
    return gHookInstalled.load();
}

bool IsOverrideActive()
{
    return ::IsOverrideActive();
}

uint32_t CurrentOverrideDPI()
{
    return IsOverrideActive() ? GetOverrideDPI() : 0;
}

void RefreshGlobalMode()
{
    gGlobalActive.store(OpenHacksVars::GlobalDPIOverride != 0, std::memory_order_relaxed);
}
} // namespace OpenHacksDpiHook
