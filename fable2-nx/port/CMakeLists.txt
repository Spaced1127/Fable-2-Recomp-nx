# fable_2 - ReXGlue recompiled port of Fable II (Nintendo Switch)
#
# Cross build: the generated C++ must already exist (run codegen with a host
# SDK first - see tools/build_switch.sh). targets:
#   fable_2        the app + the fable_2_recomp object library from codegen
#   fable_2.nro    packaged by rexglue_switch_add_nro
#
# SDK boilerplate is generated/rexglue.cmake (written by rexglue codegen).

cmake_minimum_required(VERSION 3.25)
project(fable_2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(REXGLUE_PLATFORM_SWITCH)
    # devkitA64's newlib hides POSIX names in strict C++ mode; the port uses
    # strdup/posix_memalign/fileno and friends.
    add_compile_definitions(_GNU_SOURCE=1 _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
    set(CMAKE_CXX_EXTENSIONS ON)
endif()

include(generated/rexglue.cmake)

# Strong-symbol override copies of recompiled guest functions (perf
# optimized), globbed per category so new files are picked up automatically.
file(GLOB_RECURSE HOTFUNC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/hotfunc/*.cpp)

set(FABLE2_SOURCES
    src/main.cpp
    src/fable2_app.cpp
    src/core/fable2_config.cpp
    src/core/fable2_patches.cpp
    src/core/fable2_hooks.cpp
    src/core/fable2_address_scan.cpp
    ${HOTFUNC_SOURCES}
)

# Switch is this port's target: the Windows extras (WPF launcher, D3D12
# backend, keyboard gamepad, remote control, alloc-watch thread) are not
# compiled at all.
add_executable(fable_2 ${FABLE2_SOURCES})

# The app's headers live in src/core and src/input and are included by bare
# filename from both the app and the recompiled code. The recomp target is
# created inside rexglue_setup_target, so its include dirs are wired after it.
set(FABLE2_HEADER_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input)
target_include_directories(fable_2 PRIVATE ${FABLE2_HEADER_DIRS})

rexglue_setup_target(fable_2 GPU_PLUGINS xenos)

if(TARGET fable_2_recomp)
    target_include_directories(fable_2_recomp PRIVATE ${FABLE2_HEADER_DIRS})
endif()

# The XEX profile selector xex_verify.h consumes. "goty-compatible" enables
# every catalogue entry validated for the recompiled guest-code group, like
# the Windows build's default. Do NOT "brand" this string - it selects which
# default.xex revisions the launch check accepts.
include(constants/game_versions.cmake)
target_compile_definitions(fable_2 PRIVATE
    FABLE2_BUILD_PROFILE="${FABLE2_CATALOG_COMPATIBLE_PROFILE}"
    FABLE2_NATIVE_SDK="source-built")

# Native zlib inflate (src/core/hotfunc/zlib/NativeInflate.cpp): zlib-ng,
# fetched at configure time (git + network on the first configure).
include(src/core/hotfunc/zlib/native_inflate.cmake)

# Diagnostic build: counting hooks on every sub_ function in the D3D-library
# candidate clusters (see tools/deko3d/gen_d3d_census_hooks.py). Used only to
# identify the Direct3D API functions the deko3d renderer must hook; off by
# default and never part of a release build.
option(FABLE2_D3D_CENSUS "Build the D3D-library call census hooks" OFF)
if(FABLE2_D3D_CENSUS)
    target_sources(fable_2 PRIVATE src/d3d_census.cpp src/d3d_census_hooks.cpp)
    target_compile_definitions(fable_2 PRIVATE FABLE2_D3D_CENSUS=1)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # The SDK's Switch target already links rexruntime, the GPU plugin
    # (whole-archive), rex::nvk and the libnx startup objects. Package the NRO
    # here.
    rexglue_switch_add_nro(fable_2
        NAME "Fable II"
        AUTHOR "Fable2Port"
        VERSION "1.0.0")
    # The prebuilt NVK archive carries its own newlib gap fills (getuid) that
    # the SDK's switch_libc_supplement.c also defines; harmless duplicates.
    target_link_options(fable_2 PRIVATE -Wl,--allow-multiple-definition)
endif()
