// cott - Crash of the Titans (Xbox 360) recompiled with ReXGlue.
//
// The app hooks follow nfsmw-nx's NfsmwApp: the same problems (no command line on the Switch, cvars that
// must be on by default) with the same answers.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

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
    // Without a GPU plugin the game runs but every graphics call is discarded: black screen.
    SetIfNotRequested("gpu_plugin", "xenos");
#if !REX_PLATFORM_SWITCH
    // Keyboard and mouse as a controller on PC. Not on the Switch: it would shadow the libnx pads.
    SetIfNotRequested("mnk_mode", "true");
#endif
  }

 private:
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
