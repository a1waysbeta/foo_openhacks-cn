#include "pch.h"
#include "hacks_core.h"
#include "hacks_vars.h"
#include "hacks_colors.h"
#include "hacks_colors_paint.h"

LRESULT OpenHacksCore::OpenHacksStatusBarProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCDESTROY:
        OpenHacksColorsPaint::StatusBarCleanup(wnd);
        OpenHacksColors::UnregisterColorWindow(wnd);
        SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)mStatusBarOriginProc);
        break;

    case WM_WINDOWPOSCHANGING:
        if (auto wpos = (PWINDOWPOS)lp)
        {
            if (OpenHacksVars::ShowStatusBar == false)
            {
                wpos->cy = 0;
                return 0;
            }
        }
        break;

    case WM_ERASEBKGND:
        if (OpenHacksColors::IsSchemeEnabled())
        {
            OpenHacksColorsPaint::PaintStatusBarErase(wnd, (HDC)wp);
            return 1;
        }
        break;

    case WM_PAINT:
        if (OpenHacksColors::IsSchemeEnabled())
        {
            PAINTSTRUCT ps = {};
            if (HDC dc = BeginPaint(wnd, &ps))
            {
                OpenHacksColorsPaint::PaintStatusBar(wnd, dc);
                EndPaint(wnd, &ps);
            }
            return 0;
        }
        break;

    case SB_SETTEXTA:
    case SB_SETTEXTW:
        if (OpenHacksColors::IsSchemeEnabled())
            OpenHacksColorsPaint::StatusBarOnSetText(wnd, wp, lp);
        break;

    case SB_SETICON:
        if (OpenHacksColors::IsSchemeEnabled())
            OpenHacksColorsPaint::StatusBarOnSetIcon(wnd, wp, lp);
        break;

    default:
        break;
    }

    return CallWindowProc(mStatusBarOriginProc, wnd, msg, wp, lp);
}
