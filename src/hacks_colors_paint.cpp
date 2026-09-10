#include "pch.h"
#include "hacks_colors_paint.h"
#include "hacks_colors.h"
#include "hacks_vars.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace
{
	// Minimal RAII helpers for GDI object selection / DC state
	class GdiSelectScope
	{
	public:
		GdiSelectScope(HDC dc, HGDIOBJ obj) : mDC(dc), mOld(::SelectObject(dc, obj)) {}
		~GdiSelectScope()
		{
			if (mOld != nullptr)
				::SelectObject(mDC, mOld);
		}
		GdiSelectScope(const GdiSelectScope&) = delete;
		GdiSelectScope& operator=(const GdiSelectScope&) = delete;

	private:
		HDC mDC = nullptr;
		HGDIOBJ mOld = nullptr;
	};

	COLORREF SchemeColor(uint32_t value)
	{
		return static_cast<COLORREF>(value);
	}

	struct StatusBarState
	{
		uint32_t ownerDrawMask = 0;
		SIZE iconSizes[32] = {};
	};

	std::mutex sStatusBarMutex;
	std::unordered_map<HWND, StatusBarState> sStatusBarStates;

	// =================================================
	// Tabs (ported from libPPUI DrawTab / PaintTabs / PaintTabsErase)
	// =================================================

	void DrawSingleTab(CTabCtrl tabs, CDCHandle dc, int iTab, bool selected, const RECT* rcPaint)
	{
		CRect rc;
		if (!tabs.GetItemRect(iTab, rc))
			return;

		if (rcPaint != nullptr)
		{
			CRect rcClip;
			if (!rcClip.IntersectRect(rc, rcPaint))
				return;
		}

		const auto colors = OpenHacksColors::GetSchemeColors();
		const int edgeCX = std::max(1, MulDiv(1, (int)OpenHacksVars::DPI, 120));
		const COLORREF colorBackground = SchemeColor(selected ? colors.selection : colors.background);
		dc.FillSolidRect(rc, colorBackground);

		{
			CPen pen;
			if (pen.CreatePen(PS_SOLID, edgeCX, SchemeColor(colors.frame)))
			{
				GdiSelectScope scope(dc, pen);
				dc.MoveTo(rc.left, rc.bottom);
				dc.LineTo(rc.left, rc.top);
				dc.LineTo(rc.right, rc.top);
				dc.LineTo(rc.right, rc.bottom);
			}
		}

		wchar_t text[512] = {};
		TCITEM item = {};
		item.mask = TCIF_TEXT;
		item.pszText = text;
		item.cchTextMax = (int)(std::size(text) - 1);
		if (tabs.GetItem(iTab, &item))
		{
			GdiSelectScope fontScope(dc, tabs.GetFont());
			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(SchemeColor(selected ? colors.selectionText : colors.text));
			dc.DrawText(text, (int)wcslen(text), rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
		}
	}

	// =================================================
	// Header control
	// =================================================

	void DrawSortArrow(CDCHandle dc, const CRect& rcItem, bool up, COLORREF color)
	{
		const int arrowCX = 9;
		const int arrowCY = 5;
		const int x = rcItem.right - arrowCX - 4;
		const int cy = (rcItem.top + rcItem.bottom) / 2;

		POINT pts[3];
		if (up)
		{
			pts[0] = POINT{x, cy + arrowCY / 2};
			pts[1] = POINT{x + arrowCX, cy + arrowCY / 2};
			pts[2] = POINT{x + arrowCX / 2, cy - arrowCY / 2};
		}
		else
		{
			pts[0] = POINT{x, cy - arrowCY / 2};
			pts[1] = POINT{x + arrowCX, cy - arrowCY / 2};
			pts[2] = POINT{x + arrowCX / 2, cy + arrowCY / 2};
		}

		CBrush brush;
		CPen pen;
		if (brush.CreateSolidBrush(color) && pen.CreatePen(PS_SOLID, 1, color))
		{
			GdiSelectScope brushScope(dc, brush);
			GdiSelectScope penScope(dc, pen);
			dc.Polygon(pts, 3);
		}
	}

	void DrawHeaderBitmap(CDCHandle dc, const CRect& rcItem, HBITMAP bitmap)
	{
		BITMAP bm = {};
		if (::GetObject(bitmap, sizeof(bm), &bm) == 0)
			return;

		CDC memDC;
		if (!memDC.CreateCompatibleDC(dc))
			return;

		HBITMAP old = memDC.SelectBitmap(bitmap);
		dc.BitBlt(rcItem.left + 4, (rcItem.top + rcItem.bottom - bm.bmHeight) / 2, bm.bmWidth, bm.bmHeight, memDC, 0, 0, SRCCOPY);
		memDC.SelectBitmap(old);
	}

	// WTL CIconT has no GetIconSize; query via ICONINFO bitmaps
	SIZE GetIconPixelSize(HICON icon)
	{
		SIZE size = {};
		ICONINFO ii = {};
		if (icon != nullptr && ::GetIconInfo(icon, &ii))
		{
			BITMAP bm = {};
			if (ii.hbmColor != nullptr)
			{
				if (::GetObject(ii.hbmColor, sizeof(bm), &bm) != 0)
				{
					size.cx = bm.bmWidth;
					size.cy = bm.bmHeight;
				}
				::DeleteObject(ii.hbmColor);
			}
			if (ii.hbmMask != nullptr)
			{
				if (size.cx == 0 && ::GetObject(ii.hbmMask, sizeof(bm), &bm) != 0)
				{
					size.cx = bm.bmWidth;
					size.cy = bm.bmHeight / 2; // mask-only icons are double height
				}
				::DeleteObject(ii.hbmMask);
			}
		}
		return size;
	}

	void DrawStatusBarSizeGrip(CDCHandle dc, const CRect& rcClient, COLORREF gripColor)
	{
		CPen pen;
		if (!pen.CreatePen(PS_SOLID, 1, gripColor))
			return;

		GdiSelectScope scope(dc, pen);
		const int inset = 3;
		for (int i = 0; i < 4; ++i)
		{
			const int offset = inset + i * 3;
			dc.MoveTo(rcClient.right - offset, rcClient.bottom - inset - 1);
			dc.LineTo(rcClient.right - inset - 1, rcClient.bottom - offset);
		}
	}
} // namespace

namespace OpenHacksColorsPaint
{
	// =================================================
	// Tabs
	// =================================================

	void PaintTabsErase(HWND wnd, HDC dc)
	{
		CRect rcClient;
		::GetClientRect(wnd, &rcClient);
		CDCHandle(dc).FillSolidRect(rcClient, SchemeColor(OpenHacksColors::GetSchemeColors().background));
	}

	void PaintTabs(HWND wnd, HDC dc, const RECT* rcPaint)
	{
		CTabCtrl tabs(wnd);
		CDCHandle hdc(dc);

		CRect rcClient;
		tabs.GetClientRect(rcClient);

		// Only the standard top-aligned tab layout is hijacked
		const DWORD style = tabs.GetStyle();
		if ((style & (TCS_VERTICAL | TCS_RIGHT | TCS_BUTTONS)) != 0)
			return;

		CRect rcArea = rcClient;
		tabs.AdjustRect(FALSE, rcArea);
		const int dx = rcClient.bottom - rcArea.bottom;
		const int dy = rcClient.right - rcArea.right;
		CRect rcFrame = rcArea;
		rcFrame.InflateRect(dx / 2, dy / 2);

		const auto colors = OpenHacksColors::GetSchemeColors();
		GdiSelectScope brushScope(hdc, ::GetStockObject(DC_BRUSH));
		::SetDCBrushColor(hdc, SchemeColor(colors.frame));
		hdc.FrameRect(rcFrame, (HBRUSH)::GetStockObject(DC_BRUSH));

		const int tabCount = tabs.GetItemCount();
		const int tabSelected = tabs.GetCurSel();
		for (int iTab = 0; iTab < tabCount; ++iTab)
		{
			if (iTab != tabSelected)
				DrawSingleTab(tabs, hdc, iTab, false, rcPaint);
		}
		if (tabSelected >= 0)
			DrawSingleTab(tabs, hdc, tabSelected, true, rcPaint);
	}

	// =================================================
	// Header control
	// =================================================

	void PaintHeaderErase(HWND wnd, HDC dc)
	{
		CRect rcClient;
		::GetClientRect(wnd, &rcClient);
		CDCHandle(dc).FillSolidRect(rcClient, SchemeColor(OpenHacksColors::GetSchemeColors().background));
	}

	void PaintHeader(HWND wnd, HDC dc, const RECT* rcPaint)
	{
		CHeaderCtrl header(wnd);
		CDCHandle hdc(dc);
		const auto colors = OpenHacksColors::GetSchemeColors();

		CRect rcClient;
		header.GetClientRect(rcClient);
		hdc.FillSolidRect(rcClient, SchemeColor(colors.background));

		if (const HFONT font = header.GetFont())
			hdc.SelectFont(font);
		hdc.SetBkMode(TRANSPARENT);
		hdc.SetTextColor(SchemeColor(colors.text));

		CRect rcInvalid = rcClient;
		if (rcPaint != nullptr)
			rcInvalid.IntersectRect(rcClient, rcPaint);

		const int count = header.GetItemCount();
		for (int i = 0; i < count; ++i)
		{
			CRect rc;
			if (!header.GetItemRect(i, rc))
				continue;
			if (rc.right < rcInvalid.left || rc.left > rcInvalid.right)
				continue;

			wchar_t text[512] = {};
			HDITEMW item = {};
			item.mask = HDI_TEXT | HDI_FORMAT | HDI_BITMAP | HDI_LPARAM;
			item.pszText = text;
			item.cchTextMax = (int)(std::size(text) - 1);
			header.GetItem(i, &item);

			if ((item.fmt & HDF_OWNERDRAW) != 0)
			{
				DRAWITEMSTRUCT ds = {};
				ds.CtlType = ODT_HEADER;
				ds.CtlID = header.GetDlgCtrlID();
				ds.itemID = (UINT)i;
				ds.itemAction = ODA_DRAWENTIRE;
				ds.hwndItem = wnd;
				ds.hDC = dc;
				ds.rcItem = rc;
				ds.itemData = item.lParam;

				::DCStateScope scope(dc);
				::SendMessage(::GetParent(wnd), WM_DRAWITEM, (WPARAM)ds.CtlID, (LPARAM)&ds);
				continue;
			}

			// right-edge divider between items
			CRect rcLine(rc.right - 1, rc.top, rc.right, rc.bottom);
			hdc.FillSolidRect(rcLine, SchemeColor(colors.frame));

			if ((item.fmt & HDF_STRING) != 0 && text[0] != 0)
			{
				UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
				switch (item.fmt & HDF_JUSTIFYMASK)
				{
				case HDF_RIGHT:
					flags |= DT_RIGHT;
					break;
				case HDF_CENTER:
					flags |= DT_CENTER;
					break;
				default:
					flags |= DT_LEFT;
					break;
				}

				CRect rcText = rc;
				rcText.DeflateRect(6, 0, 6, 0);
				if ((item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0)
					rcText.right -= 14;
				hdc.DrawText(text, (int)wcslen(text), rcText, flags);
			}

			if ((item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0)
				DrawSortArrow(hdc, rc, (item.fmt & HDF_SORTUP) != 0, SchemeColor(colors.text));

			if ((item.fmt & HDF_BITMAP) != 0 && item.hbm != nullptr)
				DrawHeaderBitmap(hdc, rc, item.hbm);
		}
	}

	// =================================================
	// Status bar
	// =================================================

	void StatusBarOnSetText(HWND wnd, WPARAM wp, LPARAM lp)
	{
		(void)lp;
		const unsigned idx = (unsigned)(wp & 0xFF);
		if (idx < 32)
		{
			std::lock_guard<std::mutex> lock(sStatusBarMutex);
			auto& state = sStatusBarStates[wnd];
			const uint32_t flag = 1u << idx;
			if ((wp & SBT_OWNERDRAW) != 0)
				state.ownerDrawMask |= flag;
			else
				state.ownerDrawMask &= ~flag;
		}
	}

	void StatusBarOnSetIcon(HWND wnd, WPARAM wp, LPARAM lp)
	{
		const unsigned idx = (unsigned)wp;
		if (idx < 32)
		{
			SIZE size = {};
			if (lp != 0)
				size = GetIconPixelSize((HICON)lp);

			std::lock_guard<std::mutex> lock(sStatusBarMutex);
			sStatusBarStates[wnd].iconSizes[idx] = size;
		}
	}

	void StatusBarCleanup(HWND wnd)
	{
		std::lock_guard<std::mutex> lock(sStatusBarMutex);
		sStatusBarStates.erase(wnd);
	}

	void PaintStatusBarErase(HWND wnd, HDC dc)
	{
		CRect rcClient;
		::GetClientRect(wnd, &rcClient);
		CDCHandle(dc).FillSolidRect(rcClient, SchemeColor(OpenHacksColors::GetSchemeColors().background));
	}

	void PaintStatusBar(HWND wnd, HDC dc)
	{
		CStatusBarCtrl sb(wnd);
		CDCHandle hdc(dc);
		const auto colors = OpenHacksColors::GetSchemeColors();

		CRect rcClient;
		sb.GetClientRect(rcClient);
		hdc.FillSolidRect(rcClient, SchemeColor(colors.background));

		if (const HFONT font = sb.GetFont())
			hdc.SelectFont(font);
		hdc.SetBkMode(TRANSPARENT);
		hdc.SetTextColor(SchemeColor(colors.text));

		CPen pen;
		if (!pen.CreatePen(PS_SOLID, 1, SchemeColor(colors.highlight)))
			return;
		hdc.SelectPen(pen);

		StatusBarState state;
		{
			std::lock_guard<std::mutex> lock(sStatusBarMutex);
			if (auto iter = sStatusBarStates.find(wnd); iter != sStatusBarStates.end())
				state = iter->second;
		}

		const int count = sb.GetParts(0, nullptr);
		for (int iPart = 0; iPart < count; ++iPart)
		{
			CRect rcPart;
			if (!sb.GetRect(iPart, rcPart))
				continue;

			if (rcPart.left > 0)
			{
				hdc.MoveTo(rcPart.left, rcPart.top);
				hdc.LineTo(rcPart.left, rcPart.bottom);
			}

			int iconMargin = 0;
			if (HICON icon = sb.GetIcon(iPart); icon != nullptr && iPart < 32)
			{
				const SIZE size = state.iconSizes[iPart];
				hdc.DrawIconEx(rcPart.left + size.cx / 4, (rcPart.top + rcPart.bottom) / 2 - size.cy / 2, icon, size.cx, size.cy, 0, nullptr, DI_NORMAL);
				iconMargin = MulDiv(size.cx, 3, 2);
			}

			if ((state.ownerDrawMask & (1u << iPart)) != 0)
			{
				DRAWITEMSTRUCT ds = {};
				ds.CtlType = ODT_STATIC;
				ds.CtlID = sb.GetDlgCtrlID();
				ds.itemID = (UINT)iPart;
				ds.itemAction = ODA_DRAWENTIRE;
				ds.hwndItem = wnd;
				ds.hDC = dc;
				ds.rcItem = rcPart;

				::DCStateScope scope(dc);
				::SendMessage(::GetParent(wnd), WM_DRAWITEM, (WPARAM)ds.CtlID, (LPARAM)&ds);
			}
			else
			{
				const LRESULT info = ::SendMessage(wnd, SB_GETTEXTLENGTH, (WPARAM)iPart, 0);
				if ((HIWORD(info) & SBT_OWNERDRAW) == 0 && LOWORD(info) > 0)
				{
					CString text;
					::SendMessage(wnd, SB_GETTEXT, (WPARAM)iPart, (LPARAM)text.GetBuffer(LOWORD(info)));
					text.ReleaseBuffer();

					CRect rcText = rcPart;
					const int defMargin = rcText.Height() / 4;
					const int l = iconMargin > 0 ? iconMargin : defMargin;
					rcText.DeflateRect(l, 0, defMargin, 0);
					hdc.DrawText(text, text.GetLength(), rcText, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
				}
			}
		}

		if ((sb.GetStyle() & SBARS_SIZEGRIP) != 0)
		{
			const COLORREF gripColor = OpenHacksColors::BlendColors(SchemeColor(colors.text), SchemeColor(colors.background), 128);
			DrawStatusBarSizeGrip(hdc, rcClient, gripColor);
		}
	}

	// =================================================
	// ReBar
	// =================================================

	void PaintReBarErase(HWND wnd, HDC dc)
	{
		CRect rcClient;
		::GetClientRect(wnd, &rcClient);
		CDCHandle(dc).FillSolidRect(rcClient, SchemeColor(OpenHacksColors::GetSchemeColors().background));
	}

	void PaintReBar(HWND wnd, HDC dc, const RECT* rcPaint)
	{
		(void)rcPaint;
		CReBarCtrl rebar(wnd);
		CDCHandle hdc(dc);
		const auto colors = OpenHacksColors::GetSchemeColors();

		const int total = rebar.GetBandCount();
		for (int iBand = 0; iBand < total; ++iBand)
		{
			CRect rc;
			if (!rebar.GetRect(iBand, rc))
				continue;

			wchar_t buffer[256] = {};
			REBARBANDINFO info = {sizeof(info)};
			info.fMask = RBBIM_TEXT | RBBIM_CHILD | RBBIM_STYLE;
			info.lpText = buffer;
			info.cch = (UINT)std::size(buffer);
			rebar.GetBandInfo(iBand, &info);

			// MS implementation disregards fonts; overriding breaks the layout
			GdiSelectScope fontScope(dc, (HFONT)::GetStockObject(DEFAULT_GUI_FONT));
			hdc.SetTextColor(SchemeColor(colors.text));
			hdc.SetBkMode(TRANSPARENT);

			CRect rcText = rc;
			if ((info.fStyle & RBBS_NOGRIPPER) == 0)
			{
				const COLORREF color = OpenHacksColors::BlendColors(SchemeColor(colors.frame), SchemeColor(colors.background), 128);
				GdiSelectScope penScope(dc, ::GetStockObject(DC_PEN));
				::SetDCPenColor(dc, color);
				hdc.MoveTo(rcText.left, rcText.top);
				hdc.LineTo(rcText.left, rcText.bottom);
				rcText.left += 6; // not DPI-scaled because rebar layout isn't either
			}
			else
			{
				rcText.left += 2;
			}
			hdc.DrawText(buffer, (int)wcslen(buffer), rcText, DT_VCENTER | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
		}
	}
} // namespace OpenHacksColorsPaint
