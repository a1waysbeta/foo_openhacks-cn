#include "pch.h"
#include "hacks_core.h"
#include "hacks_priv.h"
#include "hacks_colors.h"
#include "hacks_colors_paint.h"
#include "str.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
enum class ColorChildKind : uint8_t
{
    Tab,
    Header,
    // Hijack WM_ERASEBKGND only (fill with scheme background); the window
    // keeps painting its own content. Used for rebar band children (menu
    // band, toolbar panels) so their background matches the scheme.
    BackgroundOnly,
};

struct ColorChildEntry
{
    WNDPROC originProc = nullptr;
    ColorChildKind kind = ColorChildKind::Tab;
};

std::mutex sColorChildMutex;
std::unordered_map<HWND, ColorChildEntry> sColorChildEntries;

std::mutex sProbeMutex;
std::unordered_set<std::wstring> sProbeLoggedClasses;

bool MatchClassName(HWND wnd, wstring_view_t expected)
{
    wchar_t className[128] = {};
    ::GetClassNameW(wnd, className, (int)_countof(className));
    return wstring_view_t(className) == expected;
}

ColorChildKind ClassifyColorChild(HWND wnd)
{
    return MatchClassName(wnd, kDUIHeaderControlClassName) ? ColorChildKind::Header : ColorChildKind::Tab;
}

bool ShouldPaintColorChild(HWND wnd, ColorChildKind kind)
{
    if (OpenHacksColors::IsSchemeEnabled() == false)
        return false;

    switch (kind)
    {
    case ColorChildKind::Tab:
    {
        // Only the standard top-aligned tab layout is hijacked
        const DWORD style = CTabCtrl(wnd).GetStyle();
        return (style & (TCS_VERTICAL | TCS_RIGHT | TCS_BUTTONS)) == 0;
    }

    case ColorChildKind::Header:
    case ColorChildKind::BackgroundOnly:
        return true;

    default:
        return false;
    }
}

// fb2k DUI registers layout containers (splitters, tab hosts, panel frames)
// with "{GUID}"-style class names, e.g. "{97E27FAA-C0B3-4b8e-...}". Matching
// the shape lets us hijack their background without hard-coding every
// closed-source class name.
bool IsGuidLikeClassName(HWND wnd)
{
    wchar_t className[128] = {};
    const int length = ::GetClassNameW(wnd, className, (int)_countof(className));
    constexpr int kGuidClassNameLength = 38; // "{" + 8-4-4-4-12 + "}"
    if (length != kGuidClassNameLength || className[0] != L'{' || className[kGuidClassNameLength - 1] != L'}')
        return false;

    for (int i = 1; i < kGuidClassNameLength - 1; ++i)
    {
        const wchar_t ch = className[i];
        const bool isHex = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
        if (isHex)
            continue;
        if (ch == L'-' && (i == 9 || i == 14 || i == 19 || i == 24))
            continue;
        return false;
    }
    return true;
}

// One-time probe log per class name: helps discover closed-source container
// class names (e.g. DUI splitter panels, tab hosts) at runtime.
void LogUnhandledChildClass(HWND wnd)
{
    wchar_t className[128] = {};
    ::GetClassNameW(wnd, className, (int)_countof(className));

    std::lock_guard<std::mutex> lock(sProbeMutex);
    if (sProbeLoggedClasses.insert(className).second == false)
        return;

    // Build "child > ... > main window" class chain for identification
    std::vector<std::string> chain;
    for (HWND cur = wnd; cur != nullptr && chain.size() < 8; cur = ::GetParent(cur))
    {
        wchar_t buf[128] = {};
        ::GetClassNameW(cur, buf, (int)_countof(buf));
        chain.push_back(Utility::ToUTF8(buf));
    }

    pfc::string8 message("[openhacks] probe: unhandled child window class: ");
    message << Utility::ToUTF8(className).c_str();
    message << " | path:";
    for (auto iter = chain.rbegin(); iter != chain.rend(); ++iter)
    {
        message << " > " << iter->c_str();
    }
    console::print(message);
}
} // namespace

void OpenHacksCore::AttachColorChildWindow(HWND wnd, bool probeLog)
{
    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        if (sColorChildEntries.count(wnd) != 0)
            return;
    }

    ColorChildKind kind;
    if (MatchClassName(wnd, kDUITabControlClassName))
    {
        kind = ColorChildKind::Tab;
    }
    else if (MatchClassName(wnd, kDUIHeaderControlClassName))
    {
        kind = ColorChildKind::Header;
    }
    else
    {
        // Layout containers (splitters, tab hosts, panel frames) use GUID
        // class names; fill their background with the scheme color while they
        // keep painting their own content (safe for unknown panel types).
        if (IsGuidLikeClassName(wnd))
        {
            LogUnhandledChildClass(wnd);
            AttachBackgroundOnlyWindow(wnd);
            return;
        }

        if (probeLog && MatchClassName(wnd, kDUIListControlClassName) == false)
            LogUnhandledChildClass(wnd);
        return;
    }

    const WNDPROC originProc = (WNDPROC)::SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)StaticOpenHacksColorChildProc);
    if (originProc == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        sColorChildEntries[wnd] = ColorChildEntry{originProc, kind};
    }

    OpenHacksColors::RegisterColorWindow(wnd);
}

void OpenHacksCore::AttachBackgroundOnlyWindow(HWND wnd)
{
    if (wnd == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        if (sColorChildEntries.count(wnd) != 0)
            return;
    }

    const WNDPROC originProc = (WNDPROC)::SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)StaticOpenHacksColorChildProc);
    if (originProc == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        sColorChildEntries[wnd] = ColorChildEntry{originProc, ColorChildKind::BackgroundOnly};
    }

    OpenHacksColors::RegisterColorWindow(wnd);
}

void OpenHacksCore::AttachColorChildWindows(HWND parent)
{
    if (parent == nullptr)
        return;

    EnumChildWindows(parent, [](HWND wnd, LPARAM lp) -> BOOL
    {
        OpenHacksCore::Get().AttachColorChildWindow(wnd, lp != 0);
        return TRUE;
    }, (LPARAM)1);
}

void OpenHacksCore::InstallColorWinEventHook()
{
    if (mColorWinEventHook != nullptr)
        return;

    // Watch window creation/show events in this process; fb2k DUI creates
    // layout panels (tabs, splitters) asynchronously after Initialize().
    mColorWinEventHook = ::SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, StaticOpenHacksWinEventProc, ::GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    if (mColorWinEventHook == nullptr)
    {
        console::print("[openhacks] failed to install color WinEvent hook; dynamically created panels won't be hijacked");
    }
}

void OpenHacksCore::UninstallColorWinEventHook()
{
    if (mColorWinEventHook != nullptr)
    {
        ::UnhookWinEvent(mColorWinEventHook);
        mColorWinEventHook = nullptr;
    }
}

void OpenHacksCore::OnColorWinEvent(DWORD event, HWND wnd, LONG idObject, LONG idChild)
{
    (void)event;
    if (wnd == nullptr || idObject != OBJID_WINDOW || idChild != 0)
        return;
    if (mMainWindow == nullptr || ::IsWindow(wnd) == false)
        return;
    if (::IsChild(mMainWindow, wnd) == FALSE)
        return;

    AttachColorChildWindow(wnd, true);

    // ReBar band children (menu band, toolbar panels): unify their background
    if (mRebarWindow != nullptr && ::IsChild(mRebarWindow, wnd) != FALSE)
        AttachBackgroundOnlyWindow(wnd);
}

LRESULT OpenHacksCore::OpenHacksColorChildProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ColorChildEntry entry;
    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        if (const auto iter = sColorChildEntries.find(wnd); iter != sColorChildEntries.end())
            entry = iter->second;
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        if (ShouldPaintColorChild(wnd, entry.kind))
        {
            if (entry.kind == ColorChildKind::Tab)
                OpenHacksColorsPaint::PaintTabsErase(wnd, (HDC)wp);
            else if (entry.kind == ColorChildKind::Header)
                OpenHacksColorsPaint::PaintHeaderErase(wnd, (HDC)wp);
            else
                OpenHacksColorsPaint::PaintBackgroundErase(wnd, (HDC)wp);
            return 1;
        }
        break;

    case WM_PAINT:
        if (entry.kind != ColorChildKind::BackgroundOnly && ShouldPaintColorChild(wnd, entry.kind))
        {
            PAINTSTRUCT ps = {};
            if (HDC dc = ::BeginPaint(wnd, &ps))
            {
                if (entry.kind == ColorChildKind::Tab)
                    OpenHacksColorsPaint::PaintTabs(wnd, dc, &ps.rcPaint);
                else
                    OpenHacksColorsPaint::PaintHeader(wnd, dc, &ps.rcPaint);
                ::EndPaint(wnd, &ps);
            }
            return 0;
        }
        break;

    case WM_NCDESTROY:
    {
        std::lock_guard<std::mutex> lock(sColorChildMutex);
        sColorChildEntries.erase(wnd);
        OpenHacksColors::UnregisterColorWindow(wnd);
        if (entry.originProc != nullptr)
            ::SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)entry.originProc);
        break;
    }

    default:
        break;
    }

    return entry.originProc != nullptr ? ::CallWindowProc(entry.originProc, wnd, msg, wp, lp) : ::DefWindowProc(wnd, msg, wp, lp);
}
