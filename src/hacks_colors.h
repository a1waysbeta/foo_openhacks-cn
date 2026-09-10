#pragma once
#include <Windows.h>
#include "hacks_vars.h"

namespace OpenHacksColors
{
bool IsSchemeEnabled();

CustomColorScheme GetSchemeColors();

COLORREF BlendColors(COLORREF first, COLORREF second, uint32_t weight);

void RegisterColorWindow(HWND wnd);
void UnregisterColorWindow(HWND wnd);

void RefreshColorWindows();
} // namespace OpenHacksColors
