#!/usr/bin/env bash
# Regenerate the package directories from Arch's PKGBUILDs (ref/) plus our patches (patches/ and src/ clones).
# Bump a package's revision below whenever its patches change, so installs upgrade (pkgrel <arch>.<rev>).
set -euo pipefail
cd "$(dirname "$0")"
AQUAMARINE_REL=11 # 11: offer the vgem render node (linux-dmabuf); 10: explicit host cursor warps; 9: no MAP_POPULATE on shm buffers
HYPRLAND_REL=8 # 8: linux-dmabuf on a vgem render node; 7: synchronize explicit WSL cursor warps; 6: two shm swapchain buffers
MESA_REL=8 # 8: inf comparisons (Edge crosses on rounded buttons); 7: memory budget read once per batch; 6: EGL dmabuf export of unshareable images; 5: device-select crash on vgem; 4: d3d12 shader cache; 3: WSL sync for shared buffers (flush waits, native fence fds for Chromium); 2: rebuild for LLVM 23.1
PY=$(command -v python3 >/dev/null && python3 -c 'print(1)' >/dev/null 2>&1 && echo python3 || echo python)

bash refresh-patches.sh
$PY derive-pkgbuild.py ref/aquamarine.PKGBUILD aquamarine/PKGBUILD 'aquamarine-$pkgver' --rel $AQUAMARINE_REL \
  -- aquamarine/0*.patch
# Needs the explicit-warp entry point (1.10) and the vgem render node (1.11); the soname is unchanged.
$PY derive-pkgbuild.py ref/hyprland.PKGBUILD hyprland/PKGBUILD hyprland-source --rel $HYPRLAND_REL \
  --require 'aquamarine>=0.15.1-1.11' --only hyprland \
  -- hyprland/0*.patch
$PY mesa-womarchy/make-pkgbuild.py --rel $MESA_REL ref/mesa.PKGBUILD mesa-womarchy/PKGBUILD mesa-womarchy/0*.patch
