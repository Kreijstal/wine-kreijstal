#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-arm64}"
PREFIX="${PREFIX:-${HOME}/opt/wine-kreijstal-arm64}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
HOMEBREW_PREFIX="${HOMEBREW_PREFIX:-/opt/homebrew}"

export PATH="${HOMEBREW_PREFIX}/opt/bison/bin:${HOMEBREW_PREFIX}/opt/lld/bin:${HOMEBREW_PREFIX}/opt/llvm/bin:${PATH}"

if ! command -v bison >/dev/null 2>&1; then
    echo "error: bison not found. Try: brew install bison" >&2
    exit 1
fi

if ! pkg-config --exists freetype2; then
    echo "error: freetype2 pkg-config metadata not found. Try: brew install freetype pkg-config" >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

"${ROOT_DIR}/configure" \
    --prefix="${PREFIX}" \
    --enable-archs=aarch64 \
    --with-mingw=clang \
    --without-x \
    --without-gstreamer \
    --without-gnutls \
    --with-freetype \
    --without-opengl \
    --without-vulkan \
    --without-sdl \
    --without-cups \
    --without-dbus \
    --without-ffmpeg \
    --without-opencl

make -j"${JOBS}"

# win32u loads FreeType with dlopen("libfreetype.6.dylib"). Homebrew's lib
# directory is not always in dyld's default search path for build-tree runs.
if [[ -f dlls/win32u/win32u.so ]] && ! otool -l dlls/win32u/win32u.so | grep -q "path ${HOMEBREW_PREFIX}/lib "; then
    install_name_tool -add_rpath "${HOMEBREW_PREFIX}/lib" dlls/win32u/win32u.so
fi

echo "Wine ARM64 macOS build ready: ${BUILD_DIR}/wine"
