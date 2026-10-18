// cott - ReXGlue Recompiled Project

#include "generated/default/cott_init.h"

#include "cott_app.h"

REXCVAR_DEFINE_STRING(driver_env, "", "Debug",
                      "Environment variables for the graphics driver (Mesa/NVK on the Switch), "
                      "NAME=value separated by ';', set before it starts. Example: NVK_SHADER_STATS=1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(cott_switch_gpu_mhz, 460, "Graphics",
                     "GPU clock asked of the system in handheld mode, through its official performance "
                     "configurations (no overclock): 460 (460.8 MHz, the fastest one a game can ask for), "
                     "384, or 0 to leave the system's 307.2 MHz. Docked the GPU runs at 768 MHz anyway.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_DEFINE_APP(cott, CottApp::Create)
