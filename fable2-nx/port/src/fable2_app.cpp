// fable_2 - ReXGlue Recompiled Project (Nintendo Switch) - app setup.
//
// Paths and startup hooks for the console layout:
//   <NRO dir>/                 <- the NRO, fable2_config.toml, logs/, saves/,
//                                 cache/ (self-contained; see game_root.h)
//   <game root>/               <- default.xex, data/, nxeart/, $SystemUpdate/
//                                 (Game_files/ beside the NRO when present)

#include "fable2_app.h"
#include "game_root.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include "dir_manifest_heal.h"  // fable2::manifestheal (self-heal data/dir.manifest)
#include "fable2_address_scan.h"
#include "fable2_config.h"  // fable2::config::Load / Get
#include "fable2_patches.h"
#include "game_branding.h"
#include "mouse_look.h"  // fable2::mouselook
#include "xex_verify.h"

// Show a fatal message in a way that works on Switch (no window dialogs):
// log + stderr + the SDK's crash report surface where available, then exit.
namespace {

[[noreturn]] void Fatal(const std::string& msg) {
  REXLOG_ERROR("[fable2] {}", msg);
  std::fprintf(stderr, "%s\n", msg.c_str());
  std::exit(1);
}

}  // namespace

// The SDK only loads GPU emulation when a plugin name is configured. This app
// is the only place that knows which plugin it ships with; with an empty name
// every Vd* call is ignored ("no GPU emulation loaded (gpu_plugin not set)")
// and the game sits on its first frame waiting for GPU progress - a black
// screen while the guest stays alive (Home works). svr2011-nx hits the same
// trap and defaults it in OnPreSetup; do the same here.
void Fable2App::OnPreSetup(rex::RuntimeConfig& config) {
  if (config.gpu_plugin.empty()) {
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    config.gpu_plugin = "xenos";
    REXLOG_WARN("[fable2] no gpu_plugin configured - using the built-in "
                "xenos plugin (Vulkan/NVK)");
  }
}

// The full-screen floating overlay (F3) and its stats plumbing are Windows
// Debug-build tools; on Switch the NRO logs guest frame stats via perf
// counters instead (the runtime's own overlay reads the same counters).

// Self-contained layout: everything lives next to the NRO.
//   <nro>/                 <- the program (and on Switch the launcher config)
//   <nro>/Game_files/      <- content root (default.xex, data/, nxeart/,
//                             $SystemUpdate/) when the SD uses the two-folder
//                             layout; otherwise the NRO's own folder is the
//                             content root (one-folder staging layout)
//   <nro>/saves/           <- user data: save files, settings, profiles
//   <nro>/cache/           <- runtime caches (shader cache, etc.)
//   <nro>/fable2_config.toml     <- the port's own config (see fable2_config.h)
//   <nro>/fable_2.toml     <- cvar config (SDK default)
//   <nro>/logs/            <- game logs (SDK default)
// Override the content location with the game_data_root cvar.
void Fable2App::OnConfigurePaths(rex::PathConfig& paths) {
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  std::error_code ec;

  if (paths.game_data_root.empty()) {
    const std::filesystem::path game_files = exe_dir / "Game_files";
    paths.game_data_root = std::filesystem::is_directory(game_files, ec)
        ? game_files : exe_dir;
  }
  fable2::g_game_root_path = paths.game_data_root;

  if (paths.game_data_root.empty() ||
      !std::filesystem::is_directory(paths.game_data_root / "data", ec)) {
    Fatal(std::format("The game content (data/) was not found in {}; stage "
        "Game_files/ beside fable_2.nro or set the game_data_root cvar.",
        paths.game_data_root.string()));
  }
  REXLOG_INFO("[fable2] game root: {}", paths.game_data_root.string());

  // Keeps saves and caches beside the NRO (self-contained; the SDK's default
  // per-user platform location does not exist on Switch). Set unconditionally
  // because the SDK defaults are non-empty.
  paths.user_data_root = exe_dir / "saves";
  std::filesystem::create_directories(paths.user_data_root, ec);
  paths.cache_root = exe_dir / "cache";
  std::filesystem::create_directories(paths.cache_root, ec);

  // Mount the $SystemUpdate folder (inside the content root) as update:\
  // so VdSetGraphicsInterruptCallback-era update partition lookups resolve
  // instead of failing with "device not found".
  if (paths.update_data_root.empty() && !paths.game_data_root.empty()) {
    auto update_dir = paths.game_data_root / "$SystemUpdate";
    if (std::filesystem::is_directory(update_dir, ec)) {
      paths.update_data_root = update_dir;
    }
  }
}

// Loads the port's own config (fable2_config.toml) from beside the NRO,
// creating it with defaults when missing. The guest language profile is
// detected from the XEX hash; patches whose toggles exist on Switch are
// seeded into cvars.
void Fable2App::OnPostInitLogging() {
  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
  fable2::config::Load(exe_dir / "fable2_config.toml");

  // Seed cvars from the config only while they hold their compiled default
  // (fable_2.toml, environment and command line - where supported - win).
  const auto seed_cvar = [](std::string_view cvar, std::string_view value) {
    if (!rex::cvar::SetFlagByName(cvar, value)) {
      REXLOG_WARN("[fable2-config] cvar {} rejected config value '{}'",
                  cvar, std::string_view{value});
      return;
    }
    REXLOG_INFO("[fable2-config] seeded cvar {} from fable2_config.toml",
                std::string_view{cvar});
  };

  // XLanguage profile default; the cvar wins over the config in the SDK's
  // own precedence chain.
  const fable2::config::Values& cfg = fable2::config::Get();
  std::string profile_hash;
  if (fable2::xexverify::Sha256File(fable2::GameRoot() / "default.xex",
                                    profile_hash) &&
      fable2::xexverify::IsAcceptedHash(profile_hash)) {
    const auto* version = fable2::versions::Find(profile_hash);
    if (rex::cvar::GetFlagSource("user_language") ==
        rex::cvar::Source::kDefault) {
      seed_cvar("user_language", std::to_string(version->language));
    }
    REXLOG_INFO("[build-profile] detected profile {}", version->name);
  } else {
    // Not a catalogue hash (e.g. a base-game revision). Logged, not fatal:
    // the launch hash enforcement is in OnLoadXexImage.
    REXLOG_INFO("[build-profile] default.xex is not a catalogue profile "
                "hash ({}); language default is used", profile_hash);
  }

  // hotfunc_yield_every does not back a cvar (the hotfunc reads the config
  // directly) - nothing to seed there.
  (void)cfg;
  (void)seed_cvar;
}

// Apply the game patches once the SDK has decrypted default.xex into the
// guest arena, before the module launches, then hand the guest memory to the
// subsystems that read it (address-scan hook, mouse-look hooks).
void Fable2App::OnPostLoadXexImage() {
  fable2::patches::ApplyAll(runtime()->memory(), PPCImageConfig);
  // Lets the address-scan hook read the guest page tables.
  fable2::addressscan::SetMemory(runtime()->memory());
  // Lets the direct-mouse-look hook translate the camera object's guest
  // address to a host pointer (inert on Switch - no host mouse - but kept so
  // the same hook file builds everywhere).
  fable2::mouselook::set_memory(runtime()->memory());
}

// Startup integrity check: the recompiled code assumes a specific default.xex
// layout (the catalogue's hashes - see constants/game_versions.h). A mismatch
// aborts (log) unless the skipHash cvar is set.
void Fable2App::OnLoadXexImage(std::string& xex_image) {
  // Self-heal the guest VFS index before the title runs: the game indexes
  // all of data/ from data/dir.manifest, and a missing entry means the guest
  // sees null (crashes at startup). Idempotent.
  fable2::manifestheal::EnsureComplete(fable2::GameRoot());

  // Resolve the host path the same way the SDK does (game:\ / d:\ ->
  // game_data_root). The SDK's path may use either slash direction.
  std::string_view tail = xex_image;
  if (tail.starts_with("game:")) tail.remove_prefix(5);
  else if (tail.starts_with("d:")) tail.remove_prefix(2);
  if (!tail.empty() && (tail.front() == '\\' || tail.front() == '/'))
    tail.remove_prefix(1);
  std::string host_tail{tail};
  std::replace(host_tail.begin(), host_tail.end(), '\\', '/');
  const std::filesystem::path xex = fable2::GameRoot() / host_tail;

  const std::filesystem::path cache = cache_root();
  const std::filesystem::path marker = cache / "default.xex.sha256";

  // skipHash (cvar, see main.cpp): compute and log the default.xex hash, but
  // do not let a mismatch (or an unreadable file) block the launch.
  const bool skip_hash = rex::cvar::GetFlagByName("skipHash") == "true";
  if (skip_hash) {
    REXLOG_INFO("[xex-verify] skipHash is set: the default.xex hash will be "
                "logged but NOT enforced");
  }

  REXLOG_INFO("[xex-verify] {}", xex.string());
  REXLOG_INFO("[xex-verify] accepted SHA-256: {}",
              fable2::xexverify::kExpectedHashes);
  const auto r = fable2::xexverify::Check(xex, marker);
  switch (r.result) {
    case fable2::xexverify::Result::VerifiedCached:
      REXLOG_INFO("[xex-verify] actual SHA-256: {} (verified on a previous "
                  "start; hash pass skipped)", r.actual_hash);
      break;
    case fable2::xexverify::Result::VerifiedFresh:
      REXLOG_INFO("[xex-verify] actual SHA-256: {} (MATCH; recorded in {})",
                  r.actual_hash, marker.string());
      break;
    case fable2::xexverify::Result::Mismatch: {
      if (skip_hash) {
        REXLOG_INFO("[xex-verify] actual SHA-256: {} (MISMATCH vs expected "
                    "{}; not enforced because skipHash is set)",
                    r.actual_hash, fable2::xexverify::kExpectedHashes);
        break;
      }
      REXLOG_ERROR("[xex-verify] actual SHA-256: {}", r.actual_hash);
      REXLOG_ERROR("[xex-verify] expected SHA-256: {} (MISMATCH)",
                   fable2::xexverify::kExpectedHashes);
      Fatal(std::format(
          "default.xex hash mismatch\n\n"
          "This build was coded to the catalogue's verified executables; the\n"
          "XEX in the selected game folder is not one of them.\n\n"
          "File:     {}\n"
          "Actual:   {}\n"
          "Expected: {}\n\n"
          "If this is a supported revision whose hash is simply not in the\n"
          "list yet, set skipHash = true as a flat key in fable_2.toml to\n"
          "run anyway. Do not mix game files across versions.",
          xex.string(), r.actual_hash, fable2::xexverify::kExpectedHashes));
      break;
    }
    case fable2::xexverify::Result::ReadFailed:
    default:
      if (skip_hash) {
        REXLOG_WARN("[xex-verify] could not hash {} (not enforced because "
                    "skipHash is set)", xex.string());
        break;
      }
      Fatal(  "default.xex could not be read. Check the game folder.");
      break;
  }
}

void Fable2App::OnPostSetup() {
  fable2::branding::Apply(window());

  // Hero/dog black-texture fix (just-harry's Unofficial Xenia femtofork
  // approach - see plans/hero-dog-texture-readback.md). The GPU plugin's
  // cvar is registered by now (the plugin is loaded in OnPreSetup).
  {
    const fable2::config::Values& cfg = fable2::config::Get();
    const std::string_view force_addr =
        cfg.hero_dog_texture_readback ? "0x12704000" : "";
    if (!force_addr.empty() &&
        rex::cvar::GetFlagSource("readback_resolve_force_addresses") ==
            rex::cvar::Source::kDefault) {
      if (rex::cvar::SetFlagByName("readback_resolve_force_addresses",
                                   force_addr)) {
        REXLOG_INFO("[fable2-config] seeded readback_resolve_force_addresses "
                    "from fable2_config.toml (hero_dog_texture_readback=true)");
      } else {
        REXLOG_WARN("[fable2-config] cvar readback_resolve_force_addresses "
                    "rejected '{}'", std::string{force_addr});
      }
    }
    // Dog fur Z-fighting fix: the shell-fur pixel shader relies on exact
    // 24-bit depth ties; a small host depth bias restores them.
    if (cfg.dog_fur_depth_bias &&
        rex::cvar::GetFlagSource("depth_bias_shader") ==
            rex::cvar::Source::kDefault) {
      if (rex::cvar::SetFlagByName("depth_bias_shader",
                                   "014F8A02DB7B19CA") &&
          rex::cvar::SetFlagByName("depth_bias_shader_units", "16")) {
        REXLOG_INFO("[fable2-config] seeded dog fur depth bias");
      } else {
        REXLOG_WARN("[fable2-config] dog fur depth bias cvars missing "
                    "(SDK without depth_bias_shader?)");
      }
    }
  }
  // The realtimetexturemorphing patch hook reads the config directly on
  // every call (fable2_hooks.cpp); nothing to seed here.
}
