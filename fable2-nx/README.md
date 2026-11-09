# Fable II — Nintendo Switch (ReXGlue static recompilation)

A work-in-progress port of **Fable II** (Xbox 360) to the **Nintendo Switch**, built by
statically recompiling the original `default.xex` with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and running it on the console's
Horizon OS. No game code or data is included in this repository — you build it from your
own disc.

## Status

| Area | State |
|---|---|
| Codegen + Switch build pipeline | Working (597 recompiled functions, ~94 MB NRO) |
| Game boot → main menu (Vulkan/NVK renderer) | Working, but GPU-bound: ~1.6 fps at the menu |
| Boot logos | Skippable via the existing patch (`fable2_config.toml [patches] skip_intro_videos`) |
| Input / audio / saves | Working through the SDK's Horizon layer |
| deko3d native backend | **D2 validated on hardware: 29.6–29.9 fps** at the menu (vs 1.6 fps on Vulkan/NVK). D3 (draws) is implemented and building — DKSH shaders, constants, render targets, full pipeline state, textures and indexed/non-indexed draws — but not yet verified on the console. |
| Shader chain for deko3d | Validated: 28/28 dumped shaders → SPIR-V → SPIRV-Cross → GLSL rewrite → uam → DKSH, packed with per-shader texture binding tables |
| Known crash | A guest null dereference (`sub_822358A8` called with `r3 = 0`) occurs after a few minutes in the menu; not fixed yet |

The Vulkan/NVK path is the fully connected renderer today. The deko3d backend is the
performance direction: on the Switch the Vulkan path spends ~87 % of the frame in
render-target transfers/resolves (1.6 fps → 12.8 fps when they are skipped), while the
same emulation runs the PC build at >60 fps.

## Repository layout

```
fable2-nx/
├── port/                 Switch port project
│   ├── CMakeLists.txt    build (devkitA64 + ReXGlue Switch layer)
│   ├── fable_2_manifest.toml, fable2_switch_tables.toml
│   ├── constants/        XEX profile catalogue (hashes/metadata)
│   └── src/              app, config, patches, mid-asm hooks, hotfunc overrides,
│                         deko3d backend census tools sources
├── sdk/                  ReXGlue SDK fork with the Horizon (Switch) layer
│                         (BSD-3-Clause; third-party licenses inside)
├── tools/                build/extract/fetch/stage scripts, deko3d tooling
├── docs/                 porting plan, deko3d plan + backend notes
├── third_party/          (empty) NVK is fetched by tools/fetch_nvk.py
└── out/                  build outputs (not committed)
```

`game_files/`, `port/generated/`, `port/default.xex`, `port/default_image.bin`,
`port/build/`, `sdk/out/`, `third_party/nvk/` and `out/` are **not** in the repository:
the first four are game data / generated game code, the rest are build outputs or a
prebuilt driver.

## Requirements

- A legally obtained **Fable II** Xbox 360 disc image (the USA/Europe GOTY payload).
  The extracted executable is expected to be SHA-256
  `88c4ef2e18e65409444d1b068eff921d1f7e180a5ae64edc64ba6b0872372662`
  (catalogue profile `goty-us-eu`).
- Linux or WSL2 with:
  - devkitPro with `switch-dev`, `devkitA64`, `libnx` and `deko3d` (the `deko3d`
    package is part of libnx on current devkitPro),
  - CMake ≥ 3.25, Ninja, Python 3, git, and a host C/C++ toolchain (clang).
- An SD card for the console.

## Build

### 1. Get the sources and the NVK driver

```sh
git clone <this repository> fable2-nx
cd fable2-nx
python3 tools/fetch_nvk.py --dest third_party/nvk/extracted
```

`fetch_nvk.py` downloads a pinned, MIT-licensed Mesa NVK build for Horizon (the Vulkan
driver the emulated-Xenos path uses). The deko3d backend does not need it once it can
replace Vulkan.

### 2. Extract your own game files

```sh
python3 tools/extract_iso.py "/path/to/Fable II.iso" game_files
cp game_files/default.xex port/default.xex
```

`extract_iso.py` reads XDVDFS images (plain XISO / XGD1/2/3) and copies the files; it
does not decrypt or decompress the executable itself — the code generator does that.

### 3. Host code generator and codegen

```sh
export DEVKITPRO=/opt/devkitpro
tools/build_switch.sh --host      # builds out/host/rexglue from sdk/
tools/build_switch.sh --codegen   # runs codegen, writes port/generated/default
```

Codegen takes about a minute and produces ~300 MB of C++ (not committed). The manifest
is entrypoint-only (Fable II ships no guest DLL modules). FFmpeg's generated
configuration headers (`config.h`, `avconfig.h`, codec/parser lists) are vendored
under `sdk/thirdparty/FFmpeg`, so a fresh clone needs no separate FFmpeg configure
step.

### 4. Build the NRO

```sh
tools/build_switch.sh --nro
```

The NRO is written to `port/build/fable_2.nro`. The deko3d backend is enabled by default
in the Switch build (`-DREXGLUE_USE_DEKO3D=ON`); it currently presents a test pattern
while the guest runs at full speed. To use the fully rendered Vulkan path instead,
configure with `-DREXGLUE_USE_DEKO3D=OFF` (then the Vulkan/NVK path is selected).

## Running on the console

Stage with:

```powershell
powershell -File tools\stage_sd.ps1 -SdRoot E:\      # or -Dest <folder>
```

Layout on the SD:

```
sd:/switch/Fable II/fable_2.nro
sd:/switch/Fable II/fable_2.toml        (flat cvars, see below)
sd:/switch/Fable II/Game_files/         default.xex, data/, nxeart/, $SystemUpdate/
sd:/switch/Fable II/fable2_config.toml  port settings (created on first launch)
```

Launch from hbmenu (use a full-memory/title-takeover launch where possible; applet mode
gives the guest less RAM). Config files:

- `fable_2.toml` — SDK cvars, **flat keys only**: the loader prefixes TOML table names,
  so `[Launch] skipHash` would be read as `Launch_skipHash`. Keys used by this port:
  `gpu_plugin = "xenos"`, `gpu_backend = "deko3d"` (or `"vulkan"`), `skipHash`,
  `dump_shaders`, `video_mode_width/height`, `native_2x_msaa`, `gpu_log_waits`.
- `fable2_config.toml` — the port's own settings (`[patches]` toggles such as
  `skip_intro_videos`, `unlock_website`, `unlock_ce`, `high_tick_rate`,
  `disable_motion_blur`; `[input]` mouse-look settings are desktop-oriented).

## Known issues

- **Guest crash entering the world:** a guest null dereference
  (`sub_822358A8` called with `r3 = 0`, faulting on `read of guest 0x00000024`).
  It is independent of the renderer - it also occurs on the Vulkan path - and the
  crash report is written beside the NRO. Not fixed yet.
- **deko3d D2 (validated on hardware):** shared memory, host render targets,
  clears and native presentation run at the guest's 30 Hz cap. Milestones and the
  hardware lessons are in `docs/DEKO3D_BACKEND.md`.
- **deko3d D3 (draws) is implemented but unverified:** the DKSH shaders, constant
  upload, render-target binding, pipeline state, texture descriptors and both
  indexed and non-indexed draws are in place. Until it is confirmed on the
  console the screen may still show only the game's clears. Every skipped draw is
  counted and logged by reason (`deko3d: frame N drew X, skipped Y (...)`), so a
  black screen comes with the cause. Memexport draws and EDRAM ownership
  transfers are not implemented.
- **Shader pack coverage:** three menu shaders are missing
  (`vs_AB1E86137A0240E8`, `vs_D5E12A97B3A84881`, `ps_93E7F7B5D05626AA`), and any
  shader variant whose translator modification was not dumped. Refresh by running
  the **Switch** build with `gpu_backend = "vulkan"` and `dump_shaders` set (the
  PC build uses D3D12 and dumps nothing), then `tools/deko3d/convert_shaders.py`
  and `pack_shaders.py`.
- **Vulkan path performance:** ~1.6 fps at the menu at 720p on hardware, dominated by
  render-target transfers/resolves under NVK.
- The verifier expects the catalogue executable; other `default.xex` revisions need
  `skipHash = true` (the hash is still logged).

## Legal

This repository contains **no game code, game data, disc images, keys or generated
recompiled game code**. It is a set of build tools and original port code. To build it
you must supply your own legally obtained copy of Fable II; nothing here grants rights to
the game. Fable II is © Lionhead Studios / Microsoft. This project is unofficial and not
affiliated with or endorsed by Microsoft, Lionhead or devkitPro.

The ReXGlue SDK and its third-party components keep their own licenses (BSD-3-Clause,
MIT, Apache-2.0, LGPL, zlib and others — see `sdk/LICENSE` and the `LICENSE` files under
`sdk/thirdparty`). The port sources under `port/src` include code from the
Fable-2-Recomp project by Himdo (MIT, Copyright 2026 Himdo); see
`THIRD_PARTY_NOTICES.md`.

Development-only tooling for the deko3d shader chain (`tools/deko3d/`) needs the host
programs `spirv-cross` (Apache-2.0) and `uam` (from devkitPro/Mesa; zlib/MIT). Set
`SPIRV_CROSS`/`UAM` to their paths or put them on `PATH`.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and Xenia contributors.
- The Horizon/Switch SDK layer from the nfsmw/svr2011 ReXGlue ports.
- The Fable-2-Recomp project by Himdo (MIT): the Windows reference port the
  app-level sources, manifest and patching approach come from.
- devkitPro, libnx, deko3d, Mesa NVK, uam and SPIRV-Cross.
