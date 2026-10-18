// cott - ReXGlue Recompiled Project

#include "generated/default/cott_init.h"

#include "cott_app.h"

REXCVAR_DEFINE_STRING(driver_env, "", "Debug",
                      "Environment variables for the graphics driver (Mesa/NVK on the Switch), "
                      "NAME=value separated by ';', set before it starts. Example: NVK_SHADER_STATS=1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(cott_internal_resolution, "720p", "Graphics",
                      "Resolution the game draws at: 720p (1280x720, the Xbox 360's) or 480p (640x480, the "
                      "game's own widescreen SD mode, stretched to the screen). On the Switch in handheld mode "
                      "480p cuts the GPU time per frame from ~45 to ~18 ms (episode 2: 19-23 -> 26-30 FPS), "
                      "but is much blurrier.")
    .allowed({"720p", "480p"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(cott_switch_gpu_mhz, 460, "Graphics",
                     "GPU clock asked of the system in handheld mode, through its official performance "
                     "configurations (no overclock): 460 (460.8 MHz, the fastest one a game can ask for), "
                     "384, or 0 to leave the system's 307.2 MHz. Docked the GPU runs at 768 MHz anyway.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(cott_switch_gpu_force_mhz, 0, "Graphics",
                     "Testing only: GPU clock set directly (clkrst), past the handheld configurations. "
                     "768 is the docked clock, to measure docked performance without a dock. 0 = off.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_DEFINE_APP(cott, CottApp::Create)
