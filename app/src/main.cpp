// cott - ReXGlue Recompiled Project

#include "generated/default/cott_init.h"

#include "cott_app.h"

REXCVAR_DEFINE_STRING(driver_env, "", "Debug",
                      "Environment variables for the graphics driver (Mesa/NVK on the Switch), "
                      "NAME=value separated by ';', set before it starts. Example: NVK_SHADER_STATS=1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_DEFINE_APP(cott, CottApp::Create)
