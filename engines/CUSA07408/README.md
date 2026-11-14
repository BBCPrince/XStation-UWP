# God of War runtime (CUSA07408)

This directory contains the local compatibility-runtime source snapshot used for XStation's source build. `src/` contains native guest API implementations, memory/thread handling, and the UWP entry point. `gpu/` contains the renderer, shader translator, and required source dependencies. `defaults/` contains per-title startup settings.

Build target: `xstation-runtime`, output `tlou-uwp-runtime.dll`. Use `build/build-runtimes.ps1` from the repository root; see `docs/RUNTIMES.md` for toolchains and dependency setup.

The CMake project has been retargeted to the UWP runtime; unrelated desktop executables and historical trial targets are not part of this build. Optional PGO training data and game-derived payloads are not included. Rebuilt binaries are not asserted byte-identical to the older installed binaries; the original hashes are recorded in `engines/manifest.json` for provenance.

This is a modified upstream project. Original license texts and source notices are retained. The top-level `THIRD_PARTY_NOTICES.md` identifies the Bloodborne/Uncharted/bbport/shadPS4 source chain; source dependencies retain their own licenses. No game executable or game asset is provided.
