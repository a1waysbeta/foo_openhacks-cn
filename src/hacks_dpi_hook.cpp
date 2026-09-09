#include "pch.h"
#include "hacks_dpi_hook.h"
#include "hacks_vars.h"
#include <minhook.h>
#include <atomic>
#include <cstdint>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace
{
// ============================================================================
// DPI override strategy
//
// Hook every public Win32 API that returns screen DPI or DPI-derived
// metrics. When override is active (globally or scoped to Preferences
// dialog lifetime), these APIs return the user-specified DPI.
//
// Preferences mode uses window subclassing to track the dialog's lifetime:
//   - On detection (DialogBox* / CreateDialog*), set gPreferencesActive=true
//   - For modal DialogBox*: flag cleared when original returns
//   - For modeless CreateDialog*: subclass installed; flag cleared on
//     WM_NCDESTROY
//   - During the lifetime, all DPI queries (on any thread) see the override
// ============================================================================

// Local declaration of GetDpiForMonitor signature (avoid shellscalingapi.h
// which is gated on NTDDI_WINBLUE and may be missing from some SDKs).
enum LocalMonitorDpiType { LocalMDT_Default, LocalMDT_Angular, LocalMDT_Raw };
using LocalGetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, LocalMonitorDpiType, UINT*, UINT*);

// ---- Original function pointers ----
int(WINAPI* OriginGetDeviceCaps)(HDC, int) = nullptr;
UINT(WINAPI* OriginGetDpiForWindow)(HWND) = nullptr;
UINT(WINAPI* OriginGetDpiForSystem)(void) = nullptr;
LocalGetDpiForMonitorFn OriginGetDpiForMonitor = nullptr;
BOOL(WINAPI* OriginSystemParametersInfoW)(UINT, UINT, PVOID, UINT) = nullptr;
HFONT(WINAPI* OriginCreateFontIndirectW)(const LOGFONTW*) = nullptr;
HFONT(WINAPI* OriginCreateFontIndirectExW)(const ENUMLOGFONTEXW*, DWORD) = nullptr;
INT_PTR(WINAPI* OriginDialogBoxParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
INT_PTR(WINAPI* OriginDialogBoxIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;

// Win10 1607+ DPI-aware metrics APIs (optional, resolved dynamically).
int(WINAPI* OriginGetSystemMetricsForDpi)(int, UINT) = nullptr;
BOOL(WINAPI* OriginAdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT) = nullptr;
BOOL(WINAPI* OriginSetThreadDpiAwarenessContext)(int) = nullptr; // for detection only

// ---- State ----
std::atomic<bool> gHookInstalled{false};
std::atomic<bool> gGlobalActive{false};
std::atomic<bool> gPreferencesActive{false};
std::atomic<HWND> gPreferencesWnd{nullptr};
std::atomic<uint32_t> gRealSystemDPI{USER_DEFAULT_SCREEN_DPI};

// Subclass ID for Preferences windows (arbitrary unique value).
constexpr UINT_PTR kPrefSubclassId = 0x0FB20001;

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

uint32_t GetOverrideDPI()
{
    int32_t v = OpenHacksVars::DPIOverrideValue;
    if (v < 96) v = 96;
    if (v > 240) v = 240;
    return static_cast<uint32_t>(v);
}

inline bool IsOverrideActive()
{
    return gGlobalActive.load(std::memory_order_relaxed) ||
           gPreferencesActive.load(std::memory_order_relaxed);
}

inline uint32_t ReportDPI()
{
    return GetOverrideDPI();
}

bool IsMainWindowOwner(HWND owner)
{
    if (!owner) return false;
    return owner == core_api::get_main_window();
}

// ============================================================================
// Dialog template inspection — detect Preferences by SysTreeView32 presence
// ============================================================================
bool TemplateHasTreeViewControl(LPCDLGTEMPLATEW pTemplate)
{
    if (!pTemplate) return false;
    const WORD* pw = reinterpret_cast<const WORD*>(pTemplate);
    const bool isEx = (pw[0] == 1 && pw[1] == 0xFFFF);

    WORD cItems; DWORD style; const WORD* pCursor = pw;
    if (isEx)
    {
        style = static_cast<DWORD>(pw[6]) | (static_cast<DWORD>(pw[7]) << 16);
        cItems = pw[8]; pCursor = pw + 13;
    }
    else
    {
        style = static_cast<DWORD>(pw[0]) | (static_cast<DWORD>(pw[1]) << 16);
        cItems = pw[4]; pCursor = pw + 9;
    }

    auto SkipMenuOrClass = [](const WORD*& p) {
        if (*p == 0) p += 1;
        else if (*p == 0xFFFF) p += 2;
        else { while (*p != 0) ++p; ++p; }
    };
    auto SkipTitle = [](const WORD*& p) { while (*p != 0) ++p; ++p; };

    SkipMenuOrClass(pCursor); SkipMenuOrClass(pCursor); SkipTitle(pCursor);

    if (style & DS_SETFONT)
    {
        if (isEx) { pCursor += 3; SkipTitle(pCursor); }
        else      { pCursor += 1; SkipTitle(pCursor); }
    }

    for (WORD i = 0; i < cItems; ++i)
    {
        pCursor = reinterpret_cast<const WORD*>(
            (reinterpret_cast<uintptr_t>(pCursor) + 3) & ~static_cast<uintptr_t>(3));
        pCursor += isEx ? 12 : 10;

        if (*pCursor == 0xFFFF) { pCursor += 2; continue; }

        const WCHAR* className = reinterpret_cast<const WCHAR*>(pCursor);
        static const WCHAR kNeedle[] = L"SysTreeView32";
        const WCHAR *np = kNeedle, *cp = className;
        bool match = true;
        while (*np)
        {
            WCHAR a = *cp, b = *np;
            if (a >= L'A' && a <= L'Z') a = static_cast<WCHAR>(a + 32);
            if (b >= L'A' && b <= L'Z') b = static_cast<WCHAR>(b + 32);
            if (a != b) { match = false; break; }
            ++cp; ++np;
        }
        if (match && *cp == 0) return true;
        while (*pCursor != 0) ++pCursor;
        ++pCursor;

        if (*pCursor == 0xFFFF) pCursor += 2;
        else                    SkipTitle(pCursor);

        WORD cbExtra = *pCursor; ++pCursor;
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
    return pTemplate && TemplateHasTreeViewControl(pTemplate);
}

bool IsPreferencesDialogTemplateIndirect(LPCDLGTEMPLATEW pTemplate)
{
    return pTemplate && TemplateHasTreeViewControl(pTemplate);
}

// ============================================================================
// Preferences mode: window subclassing to track lifetime
// ============================================================================

LRESULT CALLBACK PrefSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam, UINT_PTR idSubclass, DWORD_PTR refData)
{
    if (msg == WM_NCDESTROY)
    {
        gPreferencesActive.store(false, std::memory_order_relaxed);
        gPreferencesWnd.store(nullptr, std::memory_order_relaxed);
        RemoveWindowSubclass(hWnd, PrefSubclassProc, idSubclass);
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

void InstallPreferencesSubclass(HWND hwnd)
{
    if (!hwnd) return;
    gPreferencesWnd.store(hwnd, std::memory_order_relaxed);
    gPreferencesActive.store(true, std::memory_order_relaxed);
    SetWindowSubclass(hwnd, PrefSubclassProc, kPrefSubclassId, 0);
}

// Decide whether to activate Preferences mode for this dialog creation.
// Returns true if this is a Preferences dialog and we should activate.
bool ShouldActivatePreferences(HWND owner, bool isResource, HINSTANCE hInst,
                               LPCWSTR name, LPCDLGTEMPLATEW templatePtr)
{
    if (gGlobalActive.load(std::memory_order_relaxed))
        return false; // global already covers everything
    if (!OpenHacksVars::PreferencesDPIBoost)
        return false;
    if (!IsMainWindowOwner(owner))
        return false;
    if (isResource)
        return IsPreferencesDialogResource(hInst, name);
    else
        return IsPreferencesDialogTemplateIndirect(templatePtr);
}

// ============================================================================
// Hook implementations
// ============================================================================

int WINAPI HookGetDeviceCaps(HDC hdc, int index)
{
    if (IsOverrideActive() && (index == LOGPIXELSX || index == LOGPIXELSY))
        return static_cast<int>(ReportDPI());
    return OriginGetDeviceCaps(hdc, index);
}

UINT WINAPI HookGetDpiForWindow(HWND hwnd)
{
    if (IsOverrideActive()) return ReportDPI();
    return OriginGetDpiForWindow(hwnd);
}

UINT WINAPI HookGetDpiForSystem()
{
    if (IsOverrideActive()) return ReportDPI();
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
    if (ret && IsOverrideActive() && pvParam)
    {
        const uint32_t target = ReportDPI();
        const uint32_t real = gRealSystemDPI.load(std::memory_order_relaxed);
        auto scale = [&](LONG& h) {
            if (h == 0) return;
            LONG scaled = static_cast<LONG>(MulDiv(h < 0 ? -h : h, target, real));
            h = h < 0 ? -scaled : scaled;
        };
        if (uiAction == SPI_GETNONCLIENTMETRICS)
        {
            NONCLIENTMETRICSW* m = static_cast<NONCLIENTMETRICSW*>(pvParam);
            scale(m->lfCaptionFont.lfHeight);
            scale(m->lfSmCaptionFont.lfHeight);
            scale(m->lfMenuFont.lfHeight);
            scale(m->lfStatusFont.lfHeight);
            scale(m->lfMessageFont.lfHeight);
        }
        else if (uiAction == SPI_GETICONMETRICS)
        {
            ICONMETRICSW* m = static_cast<ICONMETRICSW*>(pvParam);
            scale(m->lfFont.lfHeight);
        }
    }
    return ret;
}

void RescaleLogFont(LOGFONTW& lf)
{
    if (lf.lfHeight == 0) return;
    const uint32_t target = ReportDPI();
    const uint32_t real = gRealSystemDPI.load(std::memory_order_relaxed);
    LONG scaled = static_cast<LONG>(MulDiv(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight, target, real));
    lf.lfHeight = lf.lfHeight < 0 ? -scaled : scaled;
}

HFONT WINAPI HookCreateFontIndirectW(const LOGFONTW* lplf)
{
    if (IsOverrideActive() && lplf)
    {
        LOGFONTW copy = *lplf;
        RescaleLogFont(copy);
        return OriginCreateFontIndirectW(&copy);
    }
    return OriginCreateFontIndirectW(lplf);
}

HFONT WINAPI HookCreateFontIndirectExW(const ENUMLOGFONTEXW* lpelfe, DWORD fdwStyle)
{
    if (IsOverrideActive() && lpelfe)
    {
        ENUMLOGFONTEXW copy = *lpelfe;
        RescaleLogFont(copy.elfLogFont);
        return OriginCreateFontIndirectExW(&copy, fdwStyle);
    }
    return OriginCreateFontIndirectExW(lpelfe, fdwStyle);
}

// Win10 1607+: DPI-aware metrics. These accept an explicit DPI parameter;
// callers pass their queried DPI. If override is active, force the DPI
// parameter to our override value.
int WINAPI HookGetSystemMetricsForDpi(int nIndex, UINT dpi)
{
    if (IsOverrideActive()) dpi = ReportDPI();
    return OriginGetSystemMetricsForDpi(nIndex, dpi);
}

BOOL WINAPI HookAdjustWindowRectExForDpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi)
{
    if (IsOverrideActive()) dpi = ReportDPI();
    return OriginAdjustWindowRectExForDpi(lpRect, dwStyle, bMenu, dwExStyle, dpi);
}

// ---- Dialog creation hooks ----

INT_PTR WINAPI HookDialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName,
                                    HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (ShouldActivatePreferences(hWndParent, true, hInstance, lpTemplateName, nullptr))
    {
        gPreferencesActive.store(true, std::memory_order_relaxed);
        INT_PTR r = OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
        gPreferencesActive.store(false, std::memory_order_relaxed);
        gPreferencesWnd.store(nullptr, std::memory_order_relaxed);
        return r;
    }
    return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

INT_PTR WINAPI HookDialogBoxIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW hTemplate,
                                            HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (ShouldActivatePreferences(hWndParent, false, hInstance, nullptr, hTemplate))
    {
        gPreferencesActive.store(true, std::memory_order_relaxed);
        INT_PTR r = OriginDialogBoxIndirectParamW(hInstance, hTemplate, hWndParent, lpDialogFunc, dwInitParam);
        gPreferencesActive.store(false, std::memory_order_relaxed);
        gPreferencesWnd.store(nullptr, std::memory_order_relaxed);
        return r;
    }
    return OriginDialogBoxIndirectParamW(hInstance, hTemplate, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName,
                                    HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    HWND hwnd = OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    if (ShouldActivatePreferences(hWndParent, true, hInstance, lpTemplateName, nullptr))
    {
        InstallPreferencesSubclass(hwnd);
    }
    return hwnd;
}

HWND WINAPI HookCreateDialogIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW lpTemplate,
                                            HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    HWND hwnd = OriginCreateDialogIndirectParamW(hInstance, lpTemplate, hWndParent, lpDialogFunc, dwInitParam);
    if (ShouldActivatePreferences(hWndParent, false, hInstance, nullptr, lpTemplate))
    {
        InstallPreferencesSubclass(hwnd);
    }
    return hwnd;
}

// ============================================================================
// Hook management
// ============================================================================

bool EnableOneHook(void* target, void* detour, void** origin)
{
    if (MH_CreateHook(target, detour, origin) != MH_OK) return false;
    return MH_EnableHook(target) == MH_OK;
}

void DisableAllHooks()
{
    if (!gHookInstalled.exchange(false)) return;
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
    OriginGetSystemMetricsForDpi = nullptr;
    OriginAdjustWindowRectExForDpi = nullptr;
}
} // namespace

namespace OpenHacksDpiHook
{
bool Initialize()
{
    if (gHookInstalled.exchange(true)) return true;

    gRealSystemDPI.store(QuerySystemDPIReal(), std::memory_order_relaxed);

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        gHookInstalled = false;
        return false;
    }

    bool ok = true;

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

    // Optional Win10 1607+ APIs (dynamically resolved).
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto tryHook = [&](const char* name, void* detour, void** origin) -> bool {
            FARPROC p = GetProcAddress(user32, name);
            if (!p) return true; // absent — skip, non-fatal
            return EnableOneHook(reinterpret_cast<void*>(p), detour, origin);
        };
        tryHook("GetDpiForWindow", reinterpret_cast<void*>(&HookGetDpiForWindow),
                 reinterpret_cast<void**>(&OriginGetDpiForWindow));
        tryHook("GetDpiForSystem", reinterpret_cast<void*>(&HookGetDpiForSystem),
                 reinterpret_cast<void**>(&OriginGetDpiForSystem));
        tryHook("GetSystemMetricsForDpi", reinterpret_cast<void*>(&HookGetSystemMetricsForDpi),
                 reinterpret_cast<void**>(&OriginGetSystemMetricsForDpi));
        tryHook("AdjustWindowRectExForDpi", reinterpret_cast<void*>(&HookAdjustWindowRectExForDpi),
                 reinterpret_cast<void**>(&OriginAdjustWindowRectExForDpi));
    }

    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore)
    {
        auto p = GetProcAddress(shcore, "GetDpiForMonitor");
        if (p)
        {
            EnableOneHook(reinterpret_cast<void*>(p),
                          reinterpret_cast<void*>(&HookGetDpiForMonitor),
                          reinterpret_cast<void**>(&OriginGetDpiForMonitor));
        }
    }

    if (!ok)
    {
        DisableAllHooks();
        return false;
    }

    if (OpenHacksVars::GlobalDPIOverride)
        gGlobalActive.store(true, std::memory_order_relaxed);

    return true;
}

void Finalize()
{
    gGlobalActive.store(false, std::memory_order_relaxed);
    gPreferencesActive.store(false, std::memory_order_relaxed);
    DisableAllHooks();
}

bool IsActive() { return gHookInstalled.load(); }
bool IsOverrideActive() { return ::IsOverrideActive(); }
uint32_t CurrentOverrideDPI() { return IsOverrideActive() ? GetOverrideDPI() : 0; }

void RefreshGlobalMode()
{
    gGlobalActive.store(OpenHacksVars::GlobalDPIOverride != 0, std::memory_order_relaxed);
}
} // namespace OpenHacksDpiHook
