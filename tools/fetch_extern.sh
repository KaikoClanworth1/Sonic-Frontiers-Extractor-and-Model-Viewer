#!/usr/bin/env bash
# Vendors C++ dependencies into extern/. Network-heavy; run once.
set -e
cd "F:/ClaudeProjects/Sonic Frontiers ExtractorAndModel Viewer"
mkdir -p extern

echo "=== ImGui (docking branch) ==="
if [ ! -d extern/imgui/.git ]; then
  git clone --depth 1 --branch docking https://github.com/ocornut/imgui extern/imgui
else echo "imgui present"; fi

echo "=== stb single-file headers ==="
mkdir -p extern/stb
for h in stb_image.h stb_image_write.h; do
  curl -sSL -o "extern/stb/$h" "https://raw.githubusercontent.com/nothings/stb/master/$h"
  echo "  $h $(stat -c %s extern/stb/$h 2>/dev/null) bytes"
done

echo "=== GLFW 3.4 prebuilt Win64 binaries ==="
if [ ! -d extern/glfw ]; then
  curl -sSL -o extern/glfw.zip "https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip"
  cd extern && unzip -q glfw.zip && mv glfw-3.4.bin.WIN64 glfw && rm glfw.zip && cd ..
  echo "  glfw extracted"
else echo "glfw present"; fi

echo "=== tinyfiledialogs (native file picker) ==="
mkdir -p extern/tinyfd
for f in tinyfiledialogs.c tinyfiledialogs.h; do
  curl -sSL -o "extern/tinyfd/$f" "https://sourceforge.net/projects/tinyfiledialogs/files/$f/download" || true
done
# fallback mirror for tinyfd if sourceforge blocked
if [ ! -s extern/tinyfd/tinyfiledialogs.h ]; then
  curl -sSL -o extern/tinyfd/tinyfiledialogs.h "https://raw.githubusercontent.com/native-toolkit/libtinyfiledialogs/master/tinyfiledialogs.h" || true
  curl -sSL -o extern/tinyfd/tinyfiledialogs.c "https://raw.githubusercontent.com/native-toolkit/libtinyfiledialogs/master/tinyfiledialogs.c" || true
fi
echo "  tinyfd: $(stat -c %s extern/tinyfd/tinyfiledialogs.h 2>/dev/null) header bytes"

echo "=== DONE listing extern ==="
find extern -maxdepth 2 -type d | sort
