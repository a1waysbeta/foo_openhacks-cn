#include "pch.h"
#include "hacks_colors.h"
#include "hacks_vars.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace
{
std::mutex sColorWindowMutex;
std::vector<HWND> sColorWindows;
} // namespace

namespace OpenHacksColors
{
bool IsSchemeEnabled()
{
    return OpenHacksVars::ColorScheme().enabled;
}

CustomColorScheme GetSchemeColors()
{
    return OpenHacksVars::ColorScheme();
}

COLORREF BlendColors(COLORREF first, COLORREF second, uint32_t weight)
{
    weight = std::clamp(weight, 0u, 255u);
    const auto mix = [weight](unsigned int a, unsigned int b) -> uint8_t
    {
        return static_cast<uint8_t>((a * (255 - weight) + b * weight) / 255);
    };
    return RGB(mix(GetRValue(first), GetRValue(second)),
               mix(GetGValue(first), GetGValue(second)),
               mix(GetBValue(first), GetBValue(second)));
}

void RegisterColorWindow(HWND wnd)
{
    if (!wnd)
        return;

    std::lock_guard<std::mutex> lock(sColorWindowMutex);
    if (std::find(sColorWindows.begin(), sColorWindows.end(), wnd) == sColorWindows.end())
        sColorWindows.push_back(wnd);
}

void UnregisterColorWindow(HWND wnd)
{
    std::lock_guard<std::mutex> lock(sColorWindowMutex);
    sColorWindows.erase(std::remove(sColorWindows.begin(), sColorWindows.end(), wnd), sColorWindows.end());
}

void RefreshColorWindows()
{
    std::vector<HWND> windows;
    {
        std::lock_guard<std::mutex> lock(sColorWindowMutex);
        windows = sColorWindows;
    }

    for (HWND wnd : windows)
    {
        if (::IsWindow(wnd))
            ::RedrawWindow(wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    }
}
} // namespace OpenHacksColors
