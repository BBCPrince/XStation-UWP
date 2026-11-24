# XStation UWP

<img src="source/Assets/XS-Square310x310Logo.png" alt="XStation linked XS logo" width="160">

**A controller-driven game library and UWP host for Xbox Developer Mode.**

XStation provides a console dashboard, game details and settings, USB discovery, Wi-Fi transfer, and on-console import/preparation for supported PS4 game dumps supplied by the user.

Version **0.1.4.25** addresses The Last of Us's startup buffer-arena crash reported on Xbox Series S, includes Bloodborne stability fixes, and improves PKG conversion error messages. Download the MSIX from [Releases](https://github.com/BBCPrince/XStation-UWP/releases).

[Game compatibility and FPS](docs/COMPATIBILITY.md) | [Build from source](docs/BUILD.md) | [Runtime build guide](docs/RUNTIMES.md) | [Source scope](SOURCE-SCOPE.md) | [Third-party notices](THIRD_PARTY_NOTICES.md)

## Games and measured Xbox performance

Recorded **Xbox Series X Developer Mode** results from earlier development binaries; **no full campaign or Series S result is verified**. The newly source-built binaries have passed build/package checks only. FPS varies by scene and settings.

| Game | Tested game version | Recorded FPS | Verified progress |
| --- | --- | --- | --- |
| Bloodborne | **1.09** | **58–60**, 540p, dynamic shadows off (no mods) | Saved character, movement/menus, Hunter's Dream |
| The Last of Us Remastered | **1.00** (base game) | **25–30** in recorded XStation checks; 30 cap | Menus and in-game saved checkpoints; chapter completion not established |
| Uncharted 1: Drake's Fortune (Nathan Drake Collection) | **1.00** (base collection) | **30 capped**; **58–59 average uncapped** on early boat deck | Early jungle and boat-deck gameplay |
| God of War (2018) | **1.35** | **16–18**, later standalone development build | Opening gameplay / axe-swing prompt; later trial not integrated into packaged engine |
| Detroit: Become Human | **1.00** (base game) | **About 28 average**, later tuned development build | Chapter 1: The Hostage tutorial/hallway; packaged snapshot differs |
| Uncharted 2 / Uncharted 3 | Not verified | Not measured | Gameplay not verified |

Use the listed game version and matching title ID from the [compatibility list](docs/COMPATIBILITY.md). Other updates or regions are unverified and may fail preparation. These are game versions, separate from XStation's own app version.

**Bloodborne mod note:** XStation ships no game files and no mods. Since November 22, 2026 the developer's Bloodborne testing has used the [Potatoborne](https://www.nexusmods.com/bloodborne/mods/387) performance mod (by Kyoski), installed separately in the game's files; the FPS above were recorded earlier, without it. See [Bloodborne and the Potatoborne mod](docs/COMPATIBILITY.md#bloodborne-and-the-potatoborne-mod).

These are scene-specific results, not whole-game FPS guarantees. TLOU's optional frame-generated output is separate from real game FPS. See the **[full compatibility list](docs/COMPATIBILITY.md)** for settings, test references, known limits and build differences.

## App source

The application is written in **C++20**, using C++/WinRT and Windows UWP APIs. The host is built with MSVC; title runtimes use C/C++ and MSYS2 CLANG64. Mesa uses its upstream Python-based build generators, which run during the graphics-driver build.

| Location | Contents |
| --- | --- |
| [`source/dashboard.cpp`](source/dashboard.cpp) | Library screen, game details, settings, and controller navigation |
| [`source/game_app.cpp`](source/game_app.cpp) | UWP window, input, game startup, and per-title host integration |
| [`source/game_prepare.cpp`](source/game_prepare.cpp) | On-console preparation of supported game images |
| [`source/tlou_assets.cpp`](source/tlou_assets.cpp) | Texture reduction and shader-pack preparation from the user's game files |
| [`source/pkg_extract.cpp`](source/pkg_extract.cpp) | PKG extraction and import |
| [`source/transfer.cpp`](source/transfer.cpp) | Local-network transfer receiver |
| [`source/usb_files.cpp`](source/usb_files.cpp) | USB discovery and file access |
| [`source/native_cache_bridge.cpp`](source/native_cache_bridge.cpp) | Native compiler-cache integration |
| [`source/Assets`](source/Assets) | App icons and splash |
| [`engines`](engines) | Five native title runtimes, renderer/shader-translator sources, source dependencies, and defaults |
| [`graphics/mesa`](graphics/mesa) | Pinned Mesa source build and full local C/C++ source replacements |
| [`build`](build) | Source-build, dependency-preparation, staging, and asset helpers |

The repository contains application, runtime and graphics source, build helpers, and project documentation.

## Build

For the complete software package, install Visual Studio 2022 C++/UWP tools, MSYS2 CLANG64 dependencies, and the WSL Mesa build dependencies described in [docs/RUNTIMES.md](docs/RUNTIMES.md):

```powershell
.\build\build-all.ps1 -MsysRoot C:\msys64 -MesonPath /home/YOU/mesa-tools/bin/meson
```

For the host alone, run `build/build-launcher.ps1`. It generates its local PKG-reader header from hash-checked, pinned OrbisPkgTool source. No key-bearing generated header is committed. See [docs/BUILD.md](docs/BUILD.md).

## Scope and compatibility

This repository contains the dashboard, host, preparation code, all five title-runtime source snapshots, and the local Mesa/Dozen changes. The build compiles the runtimes and drivers and stages them into the UWP package. It does not supply a game. Bloodborne's matching game-specific patch payload is an optional locally supplied staging input; see the runtime build guide.

A game requires its matching runtime, supported version, and prepared image. This is not universal PS4 compatibility. Game files, game artwork, saves, console keys, compiled caches, prebuilt runtime DLLs, and prebuilt driver DLLs are not included. Third-party FSR shaders distributed with their source dependency are distinct from game shaders. The dashboard loads game artwork from the user's own game folders.

Testing has been on Xbox Series X in Developer Mode. Performance and memory behavior depend on the game, runtime, settings, and scene. Full-game stability and Series S behavior are not established by the source cleanup.

## Credits

Development uses the [Bloodborne PC project](https://github.com/Supermedo/bloodborne_pc) and [Uncharted: The Nathan Drake Collection PC project](https://github.com/Supermedo/uncharted_pc) by Supermedo / Mohammed Albarghouthi and contributors, with local UWP/Xbox adaptations. Earlier work includes [bbport by deadinside28](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4). The graphics path uses [Mesa Dozen](https://www.mesa3d.org/).

The PKG reader is adapted from [OrbisPkgTool](https://github.com/pearlxcore/OrbisPkgTool), with [miniz](https://github.com/richgel999/miniz) for decompression. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [provenance.json](provenance.json).

## License

The existing software license is **GPL-2.0-or-later**; third-party components retain their notices and applicable licenses. The runtime snapshots and Mesa changes retain their upstream source notices. Standard toolchain libraries remain external build dependencies, with their installed version inventory recorded in `engines/CLANG64-PACKAGES.lock.txt`. Redistributing a built package also requires satisfying those libraries' applicable license/source obligations.

XStation is independent and is not affiliated with or endorsed by Microsoft, Sony Interactive Entertainment, or any game publisher. Product names identify compatibility.
