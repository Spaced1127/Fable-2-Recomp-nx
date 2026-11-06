# fable2-nx porting plan

Static recompilation port of **Fable II** (Xbox 360) to **Nintendo Switch**,
built with the ReXGlue SDK's Horizon layer — the same architecture as
svr2011-nx. This document tracks what is done and what each next phase needs.

## Phase 0 - done

- ISO extracted with `tools/extract_fable2_iso.sh` into `game_files/`
  (default.xex, data/, nxeart/, $SystemUpdate/).
- `game_files/default.xex` staged to `port/default.xex` for codegen.
- Host codegen run with the prebuilt SDK tool
  (`sdk/out/linux-amd64/rexglue codegen fable_2_manifest.toml` from `port/`):
  296 recompilation units + init/register/pch emitted in ~60 s.
- `port/src/` holds the portable game-facing sources copied from
  Fable-2-Recomp-master (config, patches, midasm hooks, address scan, hotfunc
  overrides, manifest heal, XEX verify). Windows-only tooling (keyboard
  gamepad, remote control, deadbeef/alloc watch, GUI probes, F5-Lua, console)
  is not ported.
- `port/CMakeLists.txt` + `tools/build_switch.sh` wire the devkitA64 build
  (REXGLUE_PLATFORM_SWITCH=ON, NVK from `third_party/nvk/extracted`).
- `FrameLimiterWait` hotfunc override de-Windows-ified (svcSleepThread-backed
  nanosecond pause).

## Known facts recorded at first build time

- Extracted XEX (base game rev 2, title 4D5307F1, media 716F0A0D, version
  0.0.0.26, filetime 2009-06-09, 21.217.280 bytes):
  SHA-256 `88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662`.
  **CONFIRMED ON HARDWARE (2026-11-05): this hash IS catalogue entry
  `goty-us-eu` - the GOTY USA/Europe disc ships the byte-identical
  executable payload.** No skipHash needed; the launch gate accepts it once
  the app target defines `FABLE2_BUILD_PROFILE="goty-compatible"` (the
  recatalog-compatible default, like the Windows build). An earlier NRO
  wrongly defined the macro as "switch", disabling every catalogue entry -
  that produced the first-boot hash-mismatch stop, not a real mismatch.
- First hardware boot log (before the fix): guest arena mapped, handheld
  1280x720, 514 files mounted, function table code=82170000-832C2A3C, config
  created with defaults. The SDK logs "Too few processor cores - scheduling
  will be wonky" (hbmenu/RM-time core visibility) - a warning to watch.
- Fable II is single XEX/game-title: manifest is entrypoint-only, SDL-less
  Switch build uses the SDK's inline "xenos" Vulkan (NVK) plugin, statically
  whole-archived by `rexglue_switch_configure_target`.

## Phase 1 - first NRO boots (CURRENT STATE: NRO BUILDS)

DONE (2026-11-05):
1. Configure + compile `port/` for aarch64 with devkitA64 PASSED - all ~300
   recompiled units + app built, `port/build/fable_2.nro` packaged (~99 MB).
2. First HARDWARE boot reached the launch gate (arena mapped, 514 files,
   handheld 1280x720) and stopped on the hash check only because the app
   target wrongly defined `FABLE2_BUILD_PROFILE="switch"`. Fixed to the
   catalogue's `goty-compatible` (include constants/game_versions.cmake) -
   NRO rebuilt. The disc's XEX is catalogue entry `goty-us-eu` (same hash),
   so verification passes with no skipHash.
3. Compile fixes applied along the way:
   - `ProcessGameFrame_82276C30.cpp`: x86-only `target("fma")` attribute now
     guarded to x86-64 (aarch64 uses native fmadd via `__builtin_fma`).
   - `fable2_app.h`: includes generated `fable_2_pch.h` (PPCImageConfig).
   - `-DREXGLUE_ENABLE_TRACY=OFF` (Tracy has no Switch port in the SDK).
   - `-Wl,--allow-multiple-definition` (NVK archive + SDK libc supplement both
     define getuid - same as svr2011-nx).
3. NEXT: stage onto SD via `tools/stage_sd.ps1` and boot via hbmenu.

## Phase 2 - runtime bring-up (IN PROGRESS)

DONE (2026-11-05, second hardware boot):
- Hash gate passed (catalogue `goty-us-eu`, cached marker written).
- Boot reached the guest: GameThread, XMA decoder, audio pump, VFS threads all
  running; guest 166 MB mapped.
- BLACK SCREEN root cause: the app never set a GPU plugin, so the SDK logged
  "VdSetGraphicsInterruptCallback: no GPU emulation loaded (gpu_plugin not
  set); call ignored" and the guest spun forever in its frame limiter waiting
  for GPU progress (0.0 fps while GameThread used ~44% CPU). svr2011-nx has
  the same documented failure and defaults `gpu_plugin = "xenos"` in
  OnPreSetup - mirrored in Fable2App::OnPreSetup (with the cvar set too).
- Config trap fixed: the SDK's cvar loader prefixes TOML table names
  ([Launch] skipHash -> Launch_skipHash), so fable_2.toml must use FLAT keys;
  the staged file and all messages/instructions were corrected.
- NRO rebuilt and restaged with the fix.

Ranked next checks after the plugin fix:

DONE/MEASURED (2026-11-05, third boot: GPU now draws):
- NVK device up (Tegra X1, Mesa 25.0.7), 1280x720 swapchain, logo videos
  playing at 0.3-0.9 fps. GameThread ~99% CPU, ~140-149k guest read faults/s.
- The perf report's "vigilancia: desmapeo (cada lectura falla)" is the cause
  family: Horizon refuses svcSetProcessMemoryPermission on the guest window's
  svcMapProcessMemory views, so GPU-watched (read-only) physical pages are
  UNMAPPED and every guest read faults through the shadow (SDK comment: ~2 us
  each, "practically the whole core"). The SDK tries permissions once; it is
  instrumented now to log the kernel Result code on the first refusal.
- The other half of the cost is intrinsic: 720p Bink video decoded by the
  recompiled guest CPU at handheld clocks (perf showed CPU 1020 MHz / GPU 307
  when the SysClk profile dropped; 1963/998/2133 when boosted).
- Workaround staged: fable2_config.toml [patches] skip_intro_videos = true
  (existing mid-asm hook) so the boot path stops at the logos; and
  fable_2.toml now carries svr2011-nx's measured SD I/O tuning
  (nfsmw_io_ventana_kb=1024, ventanas_max=32, cache_mb=64, rangos_mb=32).

Next after that:

FOURTH BOOT (menu, intro skipped): 1.6 fps in the main menu, GPU-bound.
- Fence waits ~0.9-1.2 s per second; CPU only ~1.1 cores. Frame cost scales
  with scene complexity: 26 draws/1 resolve = 20.7 fps during load; menu =
  385 draws, 5 submits, 18 resolves (4 full-screen tiles), 418 render-target
  transfers, ~195 screen-areas of scissor = 0.5-1.6 fps.
- Console clocks are already overclocked (CPU 1963.5 / GPU 998.4 / RAM 2133).
- Horizon refused svcSetProcessMemoryPermission with 0xD401 (invalid state):
  the read-only/watched-page path cannot be enabled on these mappings; the
  SDK stays in unmapped mode. It was NOT the menu bottleneck here (0 faults).
- The emulated Xenos->NVK renderer is the slow configuration; svr2011-nx's
  playable build uses a game-specific native renderer (native_renderer.cpp,
  ~262 KB) - not reusable as-is for Fable II. Tested lever staged:
  native_2x_msaa=false in fable_2.toml.
- Also hit: "Unhandled guest access violation: read of guest 0x00000024" on
  the GPU Commands thread ~2.5 min into the menu. Symbolized from the crash
  dump: fault PC = sub_82235698 (fable_2_recomp.125.cpp:1345, lwz 36(r3)),
  called by sub_822357D0 with r3 = its own argument, itself called by
  sub_822358A8 with r3 = 0 (fable_2_recomp.233.cpp:1411). A guest null object
  pointer mid-menu; six call sites of sub_822358A8 - likely the idle/attract
  transition. Not the upstream AllocFixed trap (our SDK already fails
  re-reserves). Needs a trigger analysis once performance allows a session.

Ranked work now:

- Input: the SDK's libnx pad driver feeds XInput-state. Fable II expects a
  360 pad; verify trigger/axis ranges and the pause button behavior.
- Crash triage: Switch crash hooks write reports beside the NRO
  (`saves/`-adjacent `crash_reports/`); map each PC back to a generated
  function with `generated/default/codegen.partition.json`.
- The title update question: the catalogue expects GOTY content (`gold_version.txt`,
  `data/language/de-de/...` gates). A base-game rev 2 dump is fine for boot;
  content-compat gate only runs when the hash is a known profile.
- Audio: FFmpeg/bnk paths are SDK-side; verify boot BGM.
- GPU: readback fixes (`readback_resolve_force_addresses`) are Xenos/Vulkan
  plugin cvars; the hero/dog texture fix and depth bias are seeded in
  OnPostSetup.

## Phase 3 - polish / release

- Check `tools/check_no_game_data.py` passes on anything committed
  (game_files/, port/generated/default, sdk/out, third_party/nvk are all
  forbidden in the repo; the release is a builder + staged folder).
- Write `docs/BUILD.md` documenting the WSL build steps and the SD staging
  layout, and how the user extracts their own ISO
  (tools/extract_fable2_iso.sh).
