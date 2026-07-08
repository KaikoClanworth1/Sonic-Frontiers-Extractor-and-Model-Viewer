#!/usr/bin/env bash
# Configure + build with MSVC 2019 BuildTools (bundled CMake + Ninja).
set -e
VS="/c/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools"
CMAKE="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
NINJA="$VS/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"
VCVARS="$VS/VC/Auxiliary/Build/vcvars64.bat"
ROOT="F:/ClaudeProjects/Sonic Frontiers ExtractorAndModel Viewer"
BUILD="$ROOT/build"
TARGET="${1:-all}"

# Run cmake+ninja inside a vcvars64 environment (batch -> cmd).
cmd //c "call \"$(cygpath -w "$VCVARS")\" >nul && \
  \"$(cygpath -w "$CMAKE")\" -S \"$(cygpath -w "$ROOT")\" -B \"$(cygpath -w "$BUILD")\" \
    -G Ninja -DCMAKE_MAKE_PROGRAM=\"$(cygpath -w "$NINJA")\" -DCMAKE_BUILD_TYPE=Release && \
  \"$(cygpath -w "$CMAKE")\" --build \"$(cygpath -w "$BUILD")\" ${TARGET:+--target $TARGET}"
