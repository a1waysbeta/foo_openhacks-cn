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

namespace
{
enum class ColorChildKind : uint8_t
{
    Tab,
    Header,
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

    // Only the standard top-aligned tab layout is hijacked
    if (kind == ColorChildKind::Tab)
    {
        const DWORD style = CTabCtrl(wnd).GetStyle();
        if ((style & (TCS_VERTICAL | TCS_RIGHT | TCS_BUTTONS)) != 0)
            return false;
    }

    return true;
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
        if (probeLog && MatchClassName(wnd, kDUIListControlClassName) == false)
        {
            wchar_t className[128] = {};
            ::GetClassNameW(wnd, className, (int)_countof(className));

            // One-time probe log per class name: helps discover closed-source
            // container class names (e.g. DUI splitter panels) at runtime.
            std::lock_guard<std::mutex> lock(sProbeMutex);
            if (sProbeLoggedClasses.insert(className).second)
            {
                const auto utf8Name = Utility::ToUTF8(className);
                pfc::string8 message("[openhacks] probe: unhandled child window class: ");
                message << utf8Name.c_str();
                console::print(message);
            }
        }
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
            else
                OpenHacksColorsPaint::PaintHeaderErase(wnd, (HDC)wp);
            return 1;
        }
        break;

    case WM_PAINT:
        if (ShouldPaintColorChild(wnd, entry.kind))
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
