#include "pch.h"
#include "ui_pref_advanced.h"
#include "preferences_page_impl.h"
#include "hacks_core.h"
#include "hacks_vars.h"
#include "hacks_guids.h"

DECLARE_PREFERENCES_PAGE("Advanced", UIPrefAdvancedDialog, 100.0, OpenHacksGuids::kAdvancedPageGuid, OpenHacksGuids::kDUIPageGuid);

void UIPrefAdvancedDialog::OnInitDialog()
{
    SetHeaderFont(IDC_PREF_HEADER1);

    mComboMenuBar.Attach(GetDlgItem(IDC_MENUBAR));
    mComboMenuBar.AddString(TEXT("Show"));
    mComboMenuBar.AddString(TEXT("Hide"));

    mComboStatusBar.Attach(GetDlgItem(IDC_STATUSBAR));
    mComboStatusBar.AddString(TEXT("Show"));
    mComboStatusBar.AddString(TEXT("Hide"));

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
    else if (code == BN_CLICKED && id == IDC_PREF_DPI_BOOST)
    {
        NotifyStateChanges(true);
    }
}

void UIPrefAdvancedDialog::LoadUIState()
{
    mComboMenuBar.SetCurSel(OpenHacksVars::ShowMainMenu ? 0 : 1);
    mComboStatusBar.SetCurSel(OpenHacksVars::ShowStatusBar ? 0 : 1);
    uButton_SetCheck(m_hWnd, IDC_PREF_DPI_BOOST, OpenHacksVars::PreferencesDPIBoost);
}

void UIPrefAdvancedDialog::SaveUIState()
{
    OpenHacksVars::ShowMainMenu = mComboMenuBar.GetCurSel() == 1 ? false : true;
    OpenHacksVars::ShowStatusBar = mComboStatusBar.GetCurSel() == 1 ? false : true;
    OpenHacksVars::PreferencesDPIBoost = uButton_GetCheck(m_hWnd, IDC_PREF_DPI_BOOST);
}

void UIPrefAdvancedDialog::ApplySettings()
{
    auto& api = OpenHacksCore::Get();
    api.ShowOrHideMenuBar(OpenHacksVars::ShowMainMenu);
    api.ShowOrHideStatusBar(OpenHacksVars::ShowStatusBar);

    // The DPI boost hook is always installed (when foo_openhacks loaded
    // successfully); the runtime override is gated by this flag, so simply
    // flipping the config takes effect the next time Preferences is opened.
    // No need to call any install/uninstall API here.
}
