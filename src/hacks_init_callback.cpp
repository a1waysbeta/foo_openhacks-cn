#include "pch.h"
#include "hacks_core.h"
#include "hacks_vars.h"
#include "hacks_dpi_hook.h"

namespace
{

class open_hacks_init_stage_callback : public init_stage_callback
{
public:
    void on_init_stage(t_uint32 stage) override
    {
        if (stage == init_stages::after_config_read)
        {
            OpenHacksVars::InitialseOpenHacksVars();
        }
        else if (stage == init_stages::before_ui_init)
        {
            // Install DPI override hooks BEFORE the main window is created.
            // This is critical for Global mode: foobar2000 core queries DPI
            // while building the main window and its child controls; if the
            // hooks are installed later (in on_init, after the main window
            // exists), the whole main UI has already been laid out at the
            // real system DPI and cannot be rescaled without a restart.
            // Preferences mode does not depend on early installation; it is
            // scoped to Preferences dialog creation and works either way.
            OpenHacksDpiHook::Initialize();

            if (!OpenHacksCore::Get().CheckIncompatibleComponents())
                return;

            if (!OpenHacksCore::Get().InstallWindowHooks())
                return;
        }
    }
};

class open_hacks_initquit : public initquit
{
public:
    // on_init is called after the main window has been created.
    void on_init() override
    {
        OpenHacksCore::Get().Initialize();
    }
    // on_quit is called before the main window is destroyed.
    void on_quit() override
    {
        // safe to clean up
        OpenHacksCore::Get().Finalize();
    }
};

static initquit_factory_t<open_hacks_init_stage_callback> g_open_hacks_init_stage_callback;
static initquit_factory_t<open_hacks_initquit> g_open_hacks_initquit;
} // namespace