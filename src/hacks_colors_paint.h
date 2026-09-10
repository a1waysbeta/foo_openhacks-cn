#pragma once
#include <Windows.h>

// Subclass-level paint routines for hijacked chrome elements.
// These routines paint unconditionally with the custom color scheme;
// the subclass procs are responsible for checking OpenHacksColors::IsSchemeEnabled()
// before calling them, so that a disabled scheme falls through to default drawing.

namespace OpenHacksColorsPaint
{
// --- Tabs (SysTabControl32) ---
void PaintTabsErase(HWND wnd, HDC dc);
void PaintTabs(HWND wnd, HDC dc, const RECT* rcPaint);

// --- Header control (SysHeader32) ---
void PaintHeaderErase(HWND wnd, HDC dc);
void PaintHeader(HWND wnd, HDC dc, const RECT* rcPaint);

// --- Status bar (msctls_statusbar32) ---
// The subclass proc must forward SB_SETTEXT / SB_SETICON notifications here
// so owner-draw parts and icon sizes can be tracked.
void StatusBarOnSetText(HWND wnd, WPARAM wp, LPARAM lp);
void StatusBarOnSetIcon(HWND wnd, WPARAM wp, LPARAM lp);
void StatusBarCleanup(HWND wnd);
void PaintStatusBarErase(HWND wnd, HDC dc);
void PaintStatusBar(HWND wnd, HDC dc);

// --- ReBar (ATL:ReBarWindow32) ---
void PaintReBarErase(HWND wnd, HDC dc);
void PaintReBar(HWND wnd, HDC dc, const RECT* rcPaint);

// --- Splitter bars / panel gutters ---
} // namespace OpenHacksColorsPaint
