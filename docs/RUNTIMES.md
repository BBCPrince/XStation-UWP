# Full software build

The actual native runtime source is under `engines/<title-id>/`. These are modified local snapshots, not empty upstream references or links to private binaries. Mesa is built from its pinned upstream source archive and the full replacement C/C++ files under `graphics/mesa/`. No pre-existing local object archive is used by the new build.

Use a short checkout path such as `C:/src/xstationuwp`. The vendored AMD generated headers have long names; enable Git long paths (`git config --global core.longpaths true`) before cloning on Windows if needed.

## Windows / host

Install Visual Studio 2022 C++/UWP tools and Windows SDK 10.0.19041 or newer. The host uses MSVC/CMake. `build/build-launcher.ps1` builds it alone and prepares its local PKG-reader header from pinned, hash-checked OrbisPkgTool source.

## Windows / runtimes

Install MSYS2 CLANG64. In its CLANG64 shell install the build packages:

```sh
pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,vulkan-headers,vulkan-loader,glslang,fmt,boost,tsl-robin-map,vulkan-memory-allocator,xxhash,SDL3,ffmpeg,zydis,protobuf,nlohmann-json,openssl,zlib,miniupnpc}
```

The exact installed development package versions are recorded in `engines/CLANG64-PACKAGES.lock.txt`; the command above selects packages from your configured MSYS2 repositories and does not promise those historical versions remain available. These libraries are standard external build dependencies, not privately supplied XStation runtime binaries.

From PowerShell:

```powershell
.\build\build-runtimes.ps1 -MsysRoot C:\msys64
```

This builds Release DLLs under `out/engines/<title>/`. Three compiler workers are the default. `-Titles CUSA00552,CUSA00900` builds a subset; `-Jobs N` changes concurrency. LTO is optional; private PGO profiles are not required or included.

## WSL / Mesa

Install an Ubuntu WSL distribution and the source-build tools:

```sh
sudo apt-get install build-essential gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 ninja-build pkg-config python3-venv curl xz-utils bison flex
python3 -m venv ~/mesa-tools
~/mesa-tools/bin/pip install -r /mnt/c/PATH/TO/xstationuwp/graphics/mesa/requirements.txt
```

Adjust the checkout path. Meson 1.9.2 is pinned in the requirements file. The cross file limits pkg-config to Windows dependency paths, avoiding accidental Linux header/library imports. DirectX-Headers and zlib source fallbacks and notices are included. The Mesa 26.2.4 download is SHA-256 checked before extraction. Its download and generated build files stay in `out/`.

From WSL:

```sh
MESON=~/mesa-tools/bin/meson bash build/build-mesa.sh CUSA00552 3
```

Repeat for each desired title. A shared build directory reuses common compilation work; each result is copied to `out/mesa/<title>/vulkan_dzn.dll`. For a faster Linux-filesystem build, set `XSTATION_MESA_WORK` to a dedicated directory outside `/mnt/c` or `/mnt/e`. The script only writes there; it does not delete a previous workspace.

## Assemble and build the app

With all toolchains installed, run from PowerShell:

```powershell
.\build\build-all.ps1 -MsysRoot C:\msys64 -MesonPath /home/YOU/mesa-tools/bin/meson
```

Use `-WslDistribution` if your distribution has another name. The helper compiles runtimes and drivers, discovers imported CLANG64 DLL dependencies with LLVM, copies their installed notices, regenerates numeric red-zone site files with hash checks, and builds an MSVC UWP package containing the staged components. Standard Windows/VCLibs dependencies remain provided by Windows/Xbox. Output is under `out/build/AppPackages/`.

For separate staging/build steps:

```powershell
.\build\stage-runtimes.ps1 -MsysRoot C:\msys64
.\build\build-launcher.ps1 -RuntimeStage "$PWD\out\runtime-stage"
```

No game data is assembled by these commands. Bloodborne on-console preparation additionally expects the matching locally supplied `patches.bin` profile, SHA-256 `3bcde5d4485ba7b129550243740d3c377572669c6c7e8a6aa9d7d9ca2db7cd6f`; supply `-BloodbornePatchFile <path>` to `build-all.ps1` or `stage-runtimes.ps1`. This game-specific input is excluded from GitHub. The title runtime itself builds without it. Existing owned/prepared game folders remain external inputs as well. The host also includes a Detroit 1.00 on-console preparation recipe.

## Validation and licensing

`engines/manifest.json` records the older installed binary hashes for provenance. Fresh builds use the exported sources and a new build recipe, without private PGO profiles, so byte equality or identical performance with those binaries is not claimed. Build success is separate from full-game compatibility. Nothing is deployed to Xbox automatically.

Upstream source notices remain in the snapshots, including bbport/shadPS4, FSR/AMD, ImGui, LibAtrac9, miniz and other components. Mesa retains its own licenses; local changes are identified by the source override manifests. The dynamic toolchain dependencies also have their own terms and corresponding-source obligations where applicable. Creating this private source repository is not a certification of a future binary release.

Local validation results and rebuilt component hashes are recorded in [BUILD-VALIDATION.json](BUILD-VALIDATION.json). All five title DLLs and Mesa variants compiled, and the resulting MSIX was inspected for the ten matching x64 DLLs and three metadata file hashes. The test package used the locally supplied Bloodborne patch input. This was a build/package check, not a new gameplay run.
