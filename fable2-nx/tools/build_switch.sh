#!/usr/bin/env bash
# Fable II - Nintendo Switch build helper (Linux/WSL2).
#
#   tools/build_switch.sh --host      build the host code generator (out/host)
#   tools/build_switch.sh --codegen   generate the recompiled game C++
#   tools/build_switch.sh --nro       generate (if needed) and build the NRO
#   tools/build_switch.sh             all of the above
#
# Environment:
#   DEVKITPRO=/opt/devkitpro
#   REXGLUE_SWITCH_NVK_SDK=/path/to/nvk/extracted (default: third_party/nvk/extracted)
#   REXGLUE_USE_DEKO3D=ON|OFF (default ON; see README)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT_DIR="${REPO_ROOT}/port"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
PORTLIBS_PREFIX="${PORTLIBS_PREFIX:-${DEVKITPRO}/portlibs/switch}"
REXSDK_DIR="${REXSDK_DIR:-${REPO_ROOT}/sdk}"
NVK_SDK="${REXGLUE_SWITCH_NVK_SDK:-${REPO_ROOT}/third_party/nvk/extracted}"
JOBS="${JOBS:-$(nproc)}"
MODE="${1:---all}"

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "error: missing $1" >&2; exit 1; }
}

build_host() {
  echo "== host code generator"
  cmake -S "${REXSDK_DIR}" -B "${REPO_ROOT}/out/host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-march=x86-64-v2" \
    -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
    -DREXGLUE_USE_VULKAN=ON >/dev/null
  cmake --build "${REPO_ROOT}/out/host" --target rexglue -j "${JOBS}"
}

rexglue_bin() {
  for candidate in \
    "${REPO_ROOT}/out/host/rexglue" \
    "${REPO_ROOT}/sdk/out/linux-amd64/rexglue" \
    "${REPO_ROOT}/sdk/out/win-amd64/rexglue.exe" \
    "${REPO_ROOT}/out/host/rexglue.exe"; do
    if [[ -x "${candidate}" ]]; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

# The generated C++ is a derivative of the user's default.xex. Regenerating on
# every build is slow, so skip when the XEX and manifest hashes are unchanged.
generate() {
  local xex="${PORT_DIR}/default.xex"
  if [[ ! -f "${xex}" ]]; then
    echo "error: port/default.xex not found." >&2
    echo "       Extract it from your own disc image first:" >&2
    echo "       python3 tools/extract_iso.py game.iso game_files default.xex" >&2
    echo "       cp game_files/default.xex port/default.xex" >&2
    exit 1
  fi
  if ! rexglue_bin >/dev/null; then
    build_host
  fi

  local rexglue
  rexglue="$(rexglue_bin)"
  # The prebuilt Linux rexglue links librexruntime.so from its own directory;
  # make that work when invoked from any working directory.
  if [[ -f "$(dirname "${rexglue}")/librexruntime.so" ]]; then
    export LD_LIBRARY_PATH="$(dirname "${rexglue}")${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
  fi

  local stamp="${PORT_DIR}/generated/.xex-sha256"
  local manifest="${PORT_DIR}/fable_2_manifest.toml"
  local hash
  # Hash the contents, not the paths: moving or renaming the tree must not
  # invalidate the generated code.
  hash="$({ sha256sum < "${xex}"; sha256sum < "${manifest}"; } | sha256sum | cut -d' ' -f1)"
  if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${hash}" \
        && -n "$(ls -A "${PORT_DIR}/generated/default" 2>/dev/null)" ]]; then
    echo "== codegen: already generated for this default.xex"
    return 0
  fi
  echo "== codegen"
  (cd "${PORT_DIR}" && "${rexglue}" codegen fable_2_manifest.toml)
  mkdir -p "${PORT_DIR}/generated"
  printf '%s\n' "${hash}" > "${stamp}"
}

build_switch() {
  export DEVKITPRO DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
  if [[ ! -f "${NVK_SDK}/lib/libvulkan.a" ]]; then
    echo "error: NVK not found at ${NVK_SDK}" >&2
    echo "       run: python3 tools/fetch_nvk.py --dest third_party/nvk/extracted" >&2
    exit 1
  fi
  echo "== Switch configure"
  cmake -S "${PORT_DIR}" -B "${PORT_DIR}/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREXSDK_DIR="${REXSDK_DIR}" \
    -DREXGLUE_SWITCH_NVK_SDK="${NVK_SDK}" \
    -DREXGLUE_PLATFORM_SWITCH=ON \
    -DREXGLUE_ENABLE_TRACY=OFF \
    -DREXGLUE_USE_DEKO3D="${REXGLUE_USE_DEKO3D:-ON}" \
    -DCMAKE_PREFIX_PATH="${PORTLIBS_PREFIX}"
  cmake --build "${PORT_DIR}/build" -j "${JOBS}"
}

need git
need cmake
need ninja
need python3

case "${MODE}" in
  --host)   build_host ;;
  --codegen) generate ;;
  --nro)    generate; build_switch ;;
  *)        generate; build_switch ;;
esac

echo "done: ${MODE}"
