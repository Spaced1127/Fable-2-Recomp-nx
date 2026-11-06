# Third-party notices

This port stands on other projects. Their licenses apply to their code and to
the parts of this repository derived from it.

| Project | Used for | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with portions from [Xenia](https://xenia.jp) | `sdk/`: code generator, Xbox 360 kernel/runtime, GPU command processing, audio, VFS | BSD-3-Clause (`sdk/LICENSE`) |
| Horizon (Switch) SDK layer from the nfsmw/svr2011 ReXGlue ports | `sdk/src/**/*_switch*`, `sdk/src/ui/switch_*`, `sdk/src/input/switch/`, `sdk/src/audio/switch/`, `sdk/cmake/rexglue_switch.cmake`, `sdk/switch_compat/` | BSD-3-Clause (as part of the SDK) |
| Fable-2-Recomp by Himdo | `port/src/core`: config, patches, mid-asm hooks, address scan, hotfunc overrides, manifest and patching approach; Windows reference port | MIT; see `LICENSE` |
| [Fable II](https://en.wikipedia.org/wiki/Fable_II) | the game this port runs; NOT included | © Lionhead Studios / Microsoft; you must own it |
| [devkitPro](https://devkitpro.org/) / devkitA64 / libnx | Switch toolchain and system library | per package; see devkitPro |
| deko3d | Switch graphics API (`libdeko3d` in libnx) | zlib |
| [Mesa NVK](https://gitlab.freedesktop.org/mesa/mesa) | the pinned Switch Vulkan driver fetched by `tools/fetch_nvk.py`; not stored here | MIT (Mesa) |
| [uam](https://github.com/devkitPro/uam) | GLSL → DKSH compiler used by the deko3d shader pipeline | zlib; Mesa files MIT |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | SPIR-V → GLSL for the deko3d shader pipeline (host tool) | Apache-2.0 |
| [Dear ImGui](https://github.com/ocornut/imgui), fmt, spdlog, toml++, FFmpeg, SDL3, glslang and the rest of `sdk/thirdparty` | SDK dependencies | see the `LICENSE`/`COPYING` files in each directory |

This repository contains no game data, no disc images, no `default.xex`, no
generated recompiled game code and no keys. Building it requires the user's own
copy of the game.
