// cott - Crash of the Titans (Xbox 360) recompiled with ReXGlue.
//
// The app hooks follow nfsmw-nx's NfsmwApp: the same problems (no command line on the Switch, cvars that
// must be on by default) with the same answers.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

REXCVAR_DECLARE(std::string, driver_env);
REXCVAR_DECLARE(int32_t, cott_switch_gpu_mhz);
REXCVAR_DECLARE(int32_t, cott_switch_gpu_force_mhz);
REXCVAR_DECLARE(std::string, cott_internal_resolution);

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if REX_PLATFORM_SWITCH
#include <spdlog/sinks/stdout_sinks.h>
#include <netinet/in.h>  // struct in_addr, for __nxlink_host
#include <switch.h>

extern "C" void RexSwitchApmPedirGpuMhz(int mhz);
extern "C" void RexSwitchApmAplicar(void);
extern "C" void RexSwitchApmForzarGpuMhz(int mhz);
#endif

class CottApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<CottApp>(new CottApp(ctx, "cott",
        PPCImageConfig));
  }

 protected:
  // The game's files next to the executable, when --game_data_root is not given (the Switch has no
  // command line: the NRO lives in sdmc:/switch/cott-nx/ with the disc beside it). In this order:
  //   1. an .iso named like the executable;  2. any other .iso, alphabetically;  3. a game_root/ folder.
  // This runs before cott.toml is read (SDK order), so game_data_root cannot come from the toml.
  // Also points content_backup_root at <executable folder>/saves, so every save leaves a copy the
  // player can see (saves/<profile>/actual and anterior).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    StartNxlinkStdout();
    ChooseSaveBackupFolder();
    if (!paths.game_data_root.empty()) {
      return;  // given on the command line; it takes precedence.
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
      return;
    }
    std::string self;
    if (const auto exe = rex::filesystem::GetExecutablePath(); !exe.empty()) {
      self = Lower(exe.stem().string());
    }
    std::filesystem::path preferred;
    std::vector<std::filesystem::path> others;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
      if (ec) break;
      if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".iso") continue;
      if (!self.empty() && Lower(e.path().stem().string()) == self) {
        preferred = e.path();
      } else {
        others.push_back(e.path());
      }
    }
    if (!preferred.empty()) {
      paths.game_data_root = preferred;
    } else if (!others.empty()) {
      std::sort(others.begin(), others.end());
      paths.game_data_root = others.front();
    } else if (std::filesystem::is_directory(folder / "game_root", ec)) {
      paths.game_data_root = folder / "game_root";
    }
    // Nothing found: left empty on purpose, the SDK's own message is clearer.
  }

  // Settings that must be on for a plain start with no arguments. Only those nobody set explicitly
  // (command line or cott.toml) are touched.
  void OnPostInitLogging() override {
    ApplyDriverEnv();
    // The game asks the kernel for the video mode and draws at 1280x720 or, for anything smaller, at
    // its 640x480 widescreen mode (it has no other: asked for 1024x576, it picks 640x480).
    if (REXCVAR_GET(cott_internal_resolution) == "480p") {
      SetIfNotRequested("video_mode_width", "640");
      SetIfNotRequested("video_mode_height", "480");
    }
    // Without a GPU plugin the game runs but every graphics call is discarded: black screen.
    SetIfNotRequested("gpu_plugin", "xenos");
    // Render targets through fragment shader interlock (RADV on the PC, NVK on the Tegra X1). With
    // the default host render target path (FBO) the Switch never shows a frame: the first runs stopped
    // after 8 pipelines with a black screen, no sound and a sluggish HOME menu (GPU stuck). On the PC
    // FBO reaches the title but goes black after it. 2026-10-16, Switch runs 1-6.
    SetIfNotRequested("render_target_path_vulkan", "fsi");
    // The native renderer (Phase 3) needs buffer device addresses and descriptor arrays for the
    // translated shaders, and the folder they were generated into (tools/shaders; never committed).
    if (rex::cvar::GetFlagInfo("gpu_backend") != nullptr &&
        Lower(rex::cvar::GetFlagByName("gpu_backend")) == "native") {
      SetIfNotRequested("vulkan_native_shader_features", "true");
#if REX_PLATFORM_SWITCH
      SetIfNotRequested("native_shader_path", "sdmc:/switch/cott-nx/shaders");
#endif
    }
#if !REX_PLATFORM_SWITCH
    // Keyboard and mouse as a controller on PC. Not on the Switch: it would shadow the libnx pads.
    SetIfNotRequested("mnk_mode", "true");
#else
    // Face buttons by position, as on the Xbox pad the game was made for: the bottom one is A.
    SetIfNotRequested("input_xbox_layout", "true");
    // The GPU is the limit in handheld mode (307.2 MHz). The system's performance configuration
    // 0x92220008 gives 460.8 MHz with the same memory clock, and any game may ask for it (nfsmw-nx
    // does; sdk/src/ui/switch_apm.cpp). It is undone when the console is docked or the game ends.
    RexSwitchApmPedirGpuMhz(REXCVAR_GET(cott_switch_gpu_mhz));
    RexSwitchApmAplicar();
    RexSwitchApmForzarGpuMhz(REXCVAR_GET(cott_switch_gpu_force_mhz));
    // Started by nxlink: the log also goes to the PC's terminal, live (see StartNxlinkStdout).
    if (nxlink_stdout_) {
      rex::AddSink(std::make_shared<spdlog::sinks::stdout_sink_mt>());
    }
#endif
  }

 private:
  // driver_env: NAME=value;NAME=value, exported before the Vulkan driver loads (it reads them with
  // getenv). For driver diagnostics on the Switch, where there is no shell to set them.
  static void ApplyDriverEnv() {
    const std::string env = REXCVAR_GET(driver_env);
    size_t start = 0;
    while (start < env.size()) {
      size_t end = env.find(';', start);
      if (end == std::string::npos) end = env.size();
      const std::string item = env.substr(start, end - start);
      const size_t equals = item.find('=');
      if (equals != std::string::npos && equals > 0) {
        setenv(item.substr(0, equals).c_str(), item.substr(equals + 1).c_str(), 1);
        REXLOG_INFO("[cott] driver environment: {}", item);
      }
      start = end + 1;
    }
  }

  // When hbmenu's netloader started us (tools/switch/send.sh), libnx fills in __nxlink_host from the
  // arguments. stdout then goes to `nxlink -s` on the PC, so the log can be read while the game runs
  // and is not lost when it hangs. Only stdout: stderr stays in logs/rex/rex_stderr.log, where
  // switch_crash_hooks.c sends it. Runs in the first hook after main (before logging starts).
  void StartNxlinkStdout() {
#if REX_PLATFORM_SWITCH
    if (nxlink_stdout_ || __nxlink_host.s_addr == 0) {
      return;
    }
    if (R_FAILED(socketInitializeDefault())) {
      return;
    }
    nxlink_stdout_ = nxlinkConnectToHost(true, false) >= 0;
    if (nxlink_stdout_) {
      setvbuf(stdout, nullptr, _IOLBF, 0);
      std::printf("cott: nxlink stdout connected\n");
    } else {
      socketExit();
    }
#endif
  }

  bool nxlink_stdout_ = false;

  static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
  }

  static void SetIfNotRequested(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) {
      return;
    }
    if (rex::cvar::SetFlagByName(name, value)) {
      REXLOG_DEBUG("cott default: {} = {}", name, value);
    }
  }

  void ChooseSaveBackupFolder() {
    if (rex::cvar::GetFlagInfo("content_backup_root") == nullptr ||
        !rex::cvar::GetFlagByName("content_backup_root").empty()) {
      return;
    }
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    if (!rex::cvar::SetFlagByName("content_backup_root", rex::path_to_utf8(folder / "saves"))) {
      REXLOG_WARN("cott: could not set the save backup folder to {}",
                  rex::path_to_utf8(folder / "saves"));
    }
  }
};
