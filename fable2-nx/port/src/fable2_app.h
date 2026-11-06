// fable_2 - ReXGlue Recompiled Project (Nintendo Switch)
//
// The Switch app class: a trimmed Fable2App. Everything host-desktop specific
// (keyboard gamepad, remote control, mouse look plumbing, window fitting, the
// alloc-watch monitor thread) is left out; what remains is portable and drives
// the emulated play experience on a controller.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/rex_app.h>

#include "game_root.h"
#include "generated/default/fable_2_pch.h"  // PPCImageConfig (guest image info)

class Fable2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fable2App>(new Fable2App(ctx, "fable_2",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostInitLogging() override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPostLoadXexImage() override;
  void OnPostSetup() override;
};
