# Build XStation from source

## Prerequisites

- Windows with Visual Studio 2022 and the C++ desktop/UWP workloads, including C++/WinRT headers and x64 UWP tools.
- Windows SDK 10.0.19041.0 or newer; the previous development build used 10.0.26100.0.
- CMake 3.24 or newer, available on PATH or through the Visual Studio CMake component.
- PowerShell.
- Network access on the first default host build to fetch a pinned OrbisPkgTool source file. The build checks its SHA-256 and generates `out/local-inputs/pkg_keys.h`; nothing is written to the tracked source tree. An existing local header can instead be selected explicitly.

## Build helper

From the repository root:

```powershell
.\build\build-launcher.ps1
```

To use an existing header, pass `-PkgKeysDirectory <folder containing pkg_keys.h>`. Otherwise the default generation step supplies it locally. The helper configures a Visual Studio 2022 x64 WindowsStore project and builds Release into `out/build`. It locates Visual Studio's bundled CMake if CMake is not on PATH. Optional parameters are `-Configuration Debug`, `-CMakePath <cmake.exe>`, and `-BuildDirectory <directory>`.

Expected executable: `out/build/Release/XStation.exe`. Build output and private inputs are ignored by Git.

The host defaults to `XSTATION_PUBLIC=ON`: fake-PKG-only conversion, with development switches disabled. The generated header stays local and is not distributed with source.

## Direct CMake / Visual Studio

```powershell
cmake -S source -B out/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DXSTATION_PKG_KEYS_DIR=C:/LocalInputs/XStation
cmake --build out/build --config Release --parallel
```

Open `out/build/XStation.sln` in Visual Studio after configuring. The `source/CMakeLists.txt` can also be configured using Visual Studio's CMake support with the same cache variables. The included UWP manifest keeps the existing XStation identity and references the selected XS logo assets.

## Logo assets

The generated PNGs are committed, so regeneration is optional:

```powershell
.\build\make-logo-assets.ps1
```

This resizes `docs/branding/xstation-xs-two-tone.png` into `source/Assets`. It preserves the manifest and splash screen.

## Complete runtime package

See [RUNTIMES.md](RUNTIMES.md). `build/build-all.ps1` compiles the five runtime DLLs and Mesa variants, stages their dependencies, and builds the host with `-RuntimeStage out/runtime-stage`. CMake includes that staged content in the generated UWP package. No Xbox deployment is performed by these source-build helpers.

Back up all LocalState before installing or registering any development package on Xbox; registration can reset saves, settings and caches. Source-build validation does not replace gameplay testing.
