#include "pch.h"
#include "hacks_dpi_hook.h"
#include "hacks_vars.h"
#include <minhook.h>
#include <atomic>
#include <cstdint>

namespace
{
// ============================================================================
// Why hook CreateFontIndirectW/ExW
//
// Windows computes dialog template units (DLU) → pixels using "dialog base
// units", which are derived from the *font* assigned to the dialog (via
// DS_SETFONT in the template). The internal flow during CreateDialogParam*
// is roughly:
//
//   1. Parse the DS_SETFONT field (pointSize, weight, italic, typeface).
//   2. Build a LOGFONT and call CreateFontIndirect{,Ex}W to create the font.
//   3. Use the resulting font's tmAveCharWidth / tmHeight as the dialog
//      base units. Every control's DLU coordinates are then multiplied by
//      these base units and divided by 4 to get pixels.
//
// We hook CreateFontIndirectW/ExW during Preferences dialog creation and
// re-scale lfHeight by (boostedDpi / sysDpi). This makes the dialog font
// grow proportionally, and since base units track the font size, every
// DLU-derived control coordinate scales up automatically — no manual layout
// needed.
//
// We deliberately do NOT hook GetDeviceCaps(LOGPIXELSX/SY) here. Reason:
// Windows may or may not call GetDeviceCaps when building the LOGFONT;
// if it does and we also rescale lfHeight, the font gets scaled twice.
// Hooking only at the font layer gives a clean single-step scaling.
// ============================================================================

// Original function pointers populated by minhook.
HFONT(WINAPI* OriginCreateFontIndirectW)(LOGFONTW* lplf) = nullptr;
HFONT(WINAPI* OriginCreateFontIndirectExW)(ENUMLOGFONTEXW* lpelfe, DWORD fdwStyle) = nullptr;

// foobar2000 core creates the Preferences container dialog through one of
// these APIs with owner = main window. We hook them to bracket the creation
// call with DPI override.
INT_PTR(WINAPI* OriginDialogBoxParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
INT_PTR(WINAPI* OriginDialogBoxIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogParamW)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM) = nullptr;
HWND(WINAPI* OriginCreateDialogIndirectParamW)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM) = nullptr;

// State
std::atomic<bool> gHookInstalled{false};

// thread-local override state: active while we are inside a Preferences
// dialog creation call. A depth counter guards against (theoretical)
// nested main-window-owned dialog creation.
thread_local uint32_t tOverrideDepth = 0;
thread_local bool tOverrideActive = false;
thread_local uint32_t tOverrideDPI = 0;   // boosted DPI value
thread_local uint32_t tSystemDPI = 0;     // real system DPI captured at Begin

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
    tSystemDPI = QuerySystemDPI();
    tOverrideDPI = BoostDpi(tSystemDPI);
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
    tSystemDPI = 0;
}

// Check whether the given owner HWND is the foobar2000 main window.
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

// ============================================================================
// Dialog template inspection
//
// We only want to override DPI for the Preferences dialog, not other
// main-window-owned dialogs (About, Converter, etc.). The reliable signal
// is the presence of a SysTreeView32 control in the dialog template —
// Preferences has a tree-view on the left for category navigation, while
// About/Converter do not.
// ============================================================================

// Walk a dialog template (DLGTEMPLATE or DLGTEMPLATEEX) and return true if
// any control's class name is "SysTreeView32" (case-insensitive).
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
        // DLGTEMPLATEEX (extended) layout, in WORD units:
        //   0: dlgVer (1)
        //   1: signature (0xFFFF)
        //   2-3: helpID (DWORD)
        //   4-5: exStyle (DWORD)
        //   6-7: style (DWORD)
        //   8: cItems
        //   9: x, 10: y, 11: cx, 12: cy
        style = static_cast<DWORD>(pw[6]) | (static_cast<DWORD>(pw[7]) << 16);
        cItems = pw[8];
        pCursor = pw + 13;
    }
    else
    {
        // DLGTEMPLATE layout, in WORD units:
        //   0-1: style (DWORD)
        //   2-3: exStyle (DWORD)
        //   4: cdit
        //   5: x, 6: y, 7: cx, 8: cy
        style = static_cast<DWORD>(pw[0]) | (static_cast<DWORD>(pw[1]) << 16);
        cItems = pw[4];
        pCursor = pw + 9;
    }

    // Skip menu, class, title (each is 0x0000, or 0xFFFF + atom, or string)
    auto SkipMenuOrClass = [](const WORD*& p) {
        if (*p == 0)
        {
            p += 1; // empty
        }
        else if (*p == 0xFFFF)
        {
            p += 2; // 0xFFFF + atom
        }
        else
        {
            while (*p != 0) ++p;
            ++p; // null terminator
        }
    };

    auto SkipTitle = [](const WORD*& p) {
        while (*p != 0) ++p;
        ++p;
    };

    SkipMenuOrClass(pCursor); // menu
    SkipMenuOrClass(pCursor); // class
    SkipTitle(pCursor);       // title

    if (style & DS_SETFONT)
    {
        if (isEx)
        {
            // pointSize(1), weight(1), italic(1B)+charset(1B) packed in 1 WORD, typeface string
            pCursor += 1; // pointSize
            pCursor += 1; // weight
            pCursor += 1; // italic + charset
            SkipTitle(pCursor); // typeface
        }
        else
        {
            pCursor += 1; // pointSize
            SkipTitle(pCursor); // typeface
        }
    }

    // Iterate items
    for (WORD i = 0; i < cItems; ++i)
    {
        // DWORD-align pCursor (advance to next 4-byte boundary)
        pCursor = reinterpret_cast<const WORD*>(
            (reinterpret_cast<uintptr_t>(pCursor) + 3) & ~static_cast<uintptr_t>(3));

        // Skip item header
        if (isEx)
        {
            // DLGITEMTEMPLATEEX: helpID(2), exStyle(2), style(2), x(1), y(1), cx(1), cy(1), id(2)
            pCursor += 12;
        }
        else
        {
            // DLGITEMTEMPLATE: style(2), exStyle(2), x(1), y(1), cx(1), cy(1), id(2)
            pCursor += 10;
        }

        // class
        if (*pCursor == 0xFFFF)
        {
            pCursor += 2; // 0xFFFF + atom (no string to compare)
        }
        else
        {
            // String class name — compare case-insensitively to "SysTreeView32"
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
            // advance past the string
            while (*pCursor != 0) ++pCursor;
            ++pCursor; // null
        }

        // title
        if (*pCursor == 0xFFFF)
        {
            pCursor += 2; // 0xFFFF + atom (resource id)
        }
        else
        {
            SkipTitle(pCursor);
        }

        // extra data
        WORD cbExtra = *pCursor;
        ++pCursor;
        pCursor += (cbExtra + 1) / 2; // byte count to WORD count, rounded up
    }

    return false;
}

// Load a dialog template from a module's resources and check whether it
// contains a SysTreeView32 control. Used for resource-based dialog creation
// APIs (DialogBoxParamW, CreateDialogParamW).
bool IsPreferencesDialogResource(HINSTANCE hInstance, LPCWSTR name)
{
    if (!name)
        return false;

    // MAKEINTRESOURCE means low-word is resource id, high-word is 0
    if (IS_INTRESOURCE(name))
    {
        HRSRC hRes = FindResourceW(hInstance, name, RT_DIALOG);
        if (!hRes)
            return false;

        HGLOBAL hLoad = LoadResource(hInstance, hRes);
        if (!hLoad)
            return false;

        LPCDLGTEMPLATEW pTemplate = reinterpret_cast<LPCDLGTEMPLATEW>(LockResource(hLoad));
        if (!pTemplate)
            return false;

        return TemplateHasTreeViewControl(pTemplate);
    }

    // String resource name — load by name
    HRSRC hRes = FindResourceW(hInstance, name, RT_DIALOG);
    if (!hRes)
        return false;

    HGLOBAL hLoad = LoadResource(hInstance, hRes);
    if (!hLoad)
        return false;

    LPCDLGTEMPLATEW pTemplate = reinterpret_cast<LPCDLGTEMPLATEW>(LockResource(hLoad));
    if (!pTemplate)
        return false;

    return TemplateHasTreeViewControl(pTemplate);
}

// Check an inline dialog template (passed directly to indirect dialog APIs).
bool IsPreferencesDialogTemplateIndirect(LPCDLGTEMPLATEW pTemplate)
{
    return pTemplate && TemplateHasTreeViewControl(pTemplate);
}

// ============================================================================
// Hook implementations
// ============================================================================

// Re-scale the LOGFONT height to the boosted DPI. lfHeight is sign-bearing:
//   lfHeight > 0: cell height (rarely used by dialog templates)
//   lfHeight < 0: |lfHeight| is the font height (the common case)
//   lfHeight = 0: default — leave alone (Windows picks based on DPI)
//
// We multiply |lfHeight| by boostedDpi/sysDpi, preserving the sign, so the
// dialog font grows proportionally. As a side effect the dialog base units
// grow with the font, so every DLU-derived control coordinate scales up
// automatically.
void RescaleLogFont(LOGFONTW& lf)
{
    if (lf.lfHeight == 0 || tSystemDPI == 0)
        return;

    // Multiply by boost ratio, preserving sign.
    LONG scaled = static_cast<LONG>(
        MulDiv(static_cast<int>(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight),
               static_cast<int>(tOverrideDPI),
               static_cast<int>(tSystemDPI)));
    lf.lfHeight = lf.lfHeight < 0 ? -scaled : scaled;
}

HFONT WINAPI HookCreateFontIndirectW(LOGFONTW* lplf)
{
    if (tOverrideActive && lplf != nullptr)
    {
        RescaleLogFont(*lplf);
    }
    return OriginCreateFontIndirectW(lplf);
}

HFONT WINAPI HookCreateFontIndirectExW(ENUMLOGFONTEXW* lpelfe, DWORD fdwStyle)
{
    if (tOverrideActive && lpelfe != nullptr)
    {
        RescaleLogFont(lpelfe->elfLogFont);
    }
    return OriginCreateFontIndirectExW(lpelfe, fdwStyle);
}

INT_PTR WINAPI HookDialogBoxParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent) && IsPreferencesDialogResource(hInstance, lpTemplateName))
    {
        ScopedOverride guard;
        return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

INT_PTR WINAPI HookDialogBoxIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW hDialogTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent) && IsPreferencesDialogTemplateIndirect(hDialogTemplate))
    {
        ScopedOverride guard;
        return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginDialogBoxIndirectParamW(hInstance, hDialogTemplate, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogParamW(HINSTANCE hInstance, LPCWSTR lpTemplateName, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent) && IsPreferencesDialogResource(hInstance, lpTemplateName))
    {
        ScopedOverride guard;
        return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
    }
    return OriginCreateDialogParamW(hInstance, lpTemplateName, hWndParent, lpDialogFunc, dwInitParam);
}

HWND WINAPI HookCreateDialogIndirectParamW(HINSTANCE hInstance, LPCDLGTEMPLATEW lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM dwInitParam)
{
    if (IsMainWindowOwner(hWndParent) && IsPreferencesDialogTemplateIndirect(lpTemplate))
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

    // Disable (but do not Uninitialize) all hooks. The COM module also uses
    // minhook for CLSIDFromProgID interception; Uninitialize would tear its
    // hook down too. Disabling ours is enough.
    if (OriginCreateFontIndirectW != nullptr)
    {
        (void)MH_DisableHook(&CreateFontIndirectW);
        OriginCreateFontIndirectW = nullptr;
    }
    if (OriginCreateFontIndirectExW != nullptr)
    {
        (void)MH_DisableHook(&CreateFontIndirectExW);
        OriginCreateFontIndirectExW = nullptr;
    }
    if (OriginDialogBoxParamW != nullptr)
    {
        (void)MH_DisableHook(&DialogBoxParamW);
        OriginDialogBoxParamW = nullptr;
    }
    if (OriginDialogBoxIndirectParamW != nullptr)
    {
        (void)MH_DisableHook(&DialogBoxIndirectParamW);
        OriginDialogBoxIndirectParamW = nullptr;
    }
    if (OriginCreateDialogParamW != nullptr)
    {
        (void)MH_DisableHook(&CreateDialogParamW);
        OriginCreateDialogParamW = nullptr;
    }
    if (OriginCreateDialogIndirectParamW != nullptr)
    {
        (void)MH_DisableHook(&CreateDialogIndirectParamW);
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
    // MH_ERROR_ALREADY_INITIALIZED, which we treat as success.
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        gHookInstalled = false;
        return false;
    }

    bool ok = true;
    ok &= EnableOneHook(&CreateFontIndirectW, &HookCreateFontIndirectW,
                        reinterpret_cast<void**>(&OriginCreateFontIndirectW));
    ok &= EnableOneHook(&CreateFontIndirectExW, &HookCreateFontIndirectExW,
                        reinterpret_cast<void**>(&OriginCreateFontIndirectExW));
    ok &= EnableOneHook(&DialogBoxParamW, &HookDialogBoxParamW, reinterpret_cast<void**>(&OriginDialogBoxParamW));
    ok &= EnableOneHook(&DialogBoxIndirectParamW, &HookDialogBoxIndirectParamW,
                        reinterpret_cast<void**>(&OriginDialogBoxIndirectParamW));
    ok &= EnableOneHook(&CreateDialogParamW, &HookCreateDialogParamW,
                        reinterpret_cast<void**>(&OriginCreateDialogParamW));
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
