#!/usr/bin/env bash
# Builds the title's Dozen from pinned Mesa source and full source replacements.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
title=${1:?Usage: build-mesa.sh CUSA00552 [jobs]}
jobs=${2:-3}
meson=${MESON:-meson}
case "$title" in CUSA00552|CUSA00900|CUSA02343|CUSA07408|CUSA08344) ;; *) echo 'Unsupported title' >&2; exit 2;; esac
cache="$root/out/downloads"
work=${XSTATION_MESA_WORK:-"$root/out/mesa/shared"}
archive="$cache/mesa-26.2.4.tar.xz"
mkdir -p "$cache" "$work"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 'https://archive.mesa3d.org/mesa-26.2.4.tar.xz' -o "$archive.part"
    mv -- "$archive.part" "$archive"
fi
printf '%s  %s\n' 'bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9' "$archive" | sha256sum --check --status
if [[ ! -d "$work/source" ]]; then
    mkdir "$work/source"
    tar -xJf "$archive" -C "$work/source" --strip-components=1
fi
python3 "$root/graphics/mesa/sync-source.py" "$work/source" \
    "$root/graphics/mesa/common" "$root/graphics/mesa/titles/$title"
python3 "$root/graphics/mesa/sync-source.py" "$work/source/subprojects" \
    "$root/graphics/mesa/dependencies"
setup_args=()
if [[ -f "$work/build/build.ninja" ]]; then setup_args+=(--reconfigure); fi
if [[ ${XSTATION_MESA_CLEARCACHE:-0} == 1 ]]; then setup_args+=(--clearcache); fi
"$meson" setup "${setup_args[@]}" "$work/build" "$work/source" \
    --cross-file "$root/graphics/mesa/mingw-x64.ini" --buildtype=release \
    -Dplatforms=windows -Dvulkan-drivers=microsoft-experimental -Dgallium-drivers= \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Degl=disabled \
    -Dgbm=disabled -Dllvm=disabled -Dspirv-to-dxil=true -Dmicrosoft-clc=disabled \
    -Dshader-cache=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools= \
    -Dspirv-tools=disabled -Dzlib=enabled
ninja -C "$work/build" -j "$jobs" src/microsoft/vulkan/vulkan_dzn.dll
destination="$root/out/mesa/$title"
mkdir -p "$destination"
cp "$work/build/src/microsoft/vulkan/vulkan_dzn.dll" "$destination/vulkan_dzn.dll"
printf 'Built: %s\n' "$destination/vulkan_dzn.dll"
