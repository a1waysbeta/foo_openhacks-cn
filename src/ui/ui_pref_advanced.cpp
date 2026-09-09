#include "pch.h"
#include "ui_pref_advanced.h"
#include "preferences_page_impl.h"
#include "hacks_core.h"
#include "hacks_vars.h"
#include "hacks_guids.h"
#include "hacks_dpi_hook.h"

DECLARE_PREFERENCES_PAGE("Advanced", UIPrefAdvancedDialog, 100.0, OpenHacksGuids::kAdvancedPageGuid, OpenHacksGuids::kDUIPageGuid);

namespace
{
// DPI percentage -> DPI value mapping for the dropdown.
// Index 0..6 matches the order added in OnInitDialog.
struct DPIOption { int percent; int dpi; };
constexpr DPIOption kDPIOptions[] = {
    {100, 96},
    {125, 120},
    {150, 144},
    {175, 168},
    {200, 192},
    {225, 216},
    {250, 240},
};
constexpr size_t kDPIOptionCount = sizeof(kDPIOptions) / sizeof(kDPIOptions[0]);

int IndexForDPI(int dpi)
{
    for (size_t i = 0; i < kDPIOptionCount; ++i)
        if (kDPIOptions[i].dpi == dpi)
            return static_cast<int>(i);
    return 2; // default to 150% if unknown
}
} // namespace

void UIPrefAdvancedDialog::OnInitDialog()
{
    SetHeaderFont(IDC_PREF_HEADER1);
    SetHeaderFont(IDC_PREF_HEADER7);

    mComboMenuBar.Attach(GetDlgItem(IDC_MENUBAR));
    mComboMenuBar.AddString(TEXT("Show"));
    mComboMenuBar.AddString(TEXT("Hide"));

    mComboStatusBar.Attach(GetDlgItem(IDC_STATUSBAR));
    mComboStatusBar.AddString(TEXT("Show"));
    mComboStatusBar.AddString(TEXT("Hide"));

    mComboDPIValue.Attach(GetDlgItem(IDC_DPI_VALUE));
    for (size_t i = 0; i < kDPIOptionCount; ++i)
    {
        TCHAR buf[16];
        _sntprintf_s(buf, _TRUNCATE, TEXT("%d%%"), kDPIOptions[i].percent);
        mComboDPIValue.AddString(buf);
    }

    LoadUIState();
}

void UIPrefAdvancedDialog::OnApply()
{
    SaveUIState();
    ApplySettings();
}

void UIPrefAdvancedDialog::OnCommand(UINT code, int id, CWindow ctrl)
{
    if (code == CBN_SELCHANGE)
    {
        NotifyStateChanges(true);
    }
    else if (code == BN_CLICKED &&
             (id == IDC_PREF_DPI_BOOST || id == IDC_GLOBAL_DPI_OVERRIDE))
    {
        NotifyStateChanges(true);
    }
}

void UIPrefAdvancedDialog::LoadUIState()
{
    mComboMenuBar.SetCurSel(OpenHacksVars::ShowMainMenu ? 0 : 1);
    mComboStatusBar.SetCurSel(OpenHacksVars::ShowStatusBar ? 0 : 1);

    int dpi = OpenHacksVars::DPIOverrideValue;
    if (dpi <= 0) dpi = 144; // default 150%
    mComboDPIValue.SetCurSel(IndexForDPI(dpi));

    uButton_SetCheck(m_hWnd, IDC_PREF_DPI_BOOST, OpenHacksVars::PreferencesDPIBoost);
    uButton_SetCheck(m_hWnd, IDC_GLOBAL_DPI_OVERRIDE, OpenHacksVars::GlobalDPIOverride);
}

void UIPrefAdvancedDialog::SaveUIState()
{
    OpenHacksVars::ShowMainMenu = mComboMenuBar.GetCurSel() == 1 ? false : true;
    OpenHacksVars::ShowStatusBar = mComboStatusBar.GetCurSel() == 1 ? false : true;

    int sel = mComboDPIValue.GetCurSel();
    if (sel < 0 || sel >= static_cast<int>(kDPIOptionCount))
        sel = 2; // 150% default
    OpenHacksVars::DPIOverrideValue = kDPIOptions[sel].dpi;

    OpenHacksVars::PreferencesDPIBoost = uButton_GetCheck(m_hWnd, IDC_PREF_DPI_BOOST);
    OpenHacksVars::GlobalDPIOverride = uButton_GetCheck(m_hWnd, IDC_GLOBAL_DPI_OVERRIDE);
}

void UIPrefAdvancedDialog::ApplySettings()
{
    auto& api = OpenHacksCore::Get();
    api.ShowOrHideMenuBar(OpenHacksVars::ShowMainMenu);
    api.ShowOrHideStatusBar(OpenHacksVars::ShowStatusBar);

    // Preferences mode is consulted live by the hook on the next Preferences
    // open, so toggling the flag takes effect immediately for new opens.

    // Global mode: also flip the in-memory global flag so any subsequent DPI
    // query within the running session sees the new value. The truly
    // process-wide effect (window metrics, system fonts cached by Windows at
    // startup) still requires a foobar2000 restart, as documented in the UI.
    OpenHacksDpiHook::RefreshGlobalMode();
}
