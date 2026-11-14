# Source scope

## Included

- C++20/C++/WinRT UWP dashboard and host, import/preparation logic, USB/Wi-Fi access, settings and per-title state.
- Native C/C++ runtime snapshots for CUSA00552 (The Last of Us Remastered), CUSA00900 (Bloodborne), CUSA02343 (The Nathan Drake Collection), CUSA07408 (God of War), and CUSA08344 (Detroit).
- Their renderer, shader translator, UWP entry, memory/thread/API implementations, source dependencies and upstream notices.
- Full local Mesa source replacements plus a hash-pinned download of upstream Mesa 26.2.4, and the cross-build recipe.
- Numeric preparation metadata: offsets, instruction lengths and hashes. These lists contain no original guest instructions and rebuild the supported red-zone site files byte-for-byte.
- Host and runtime build helpers, package staging, asset generation, dependency inventory, documentation, and provenance.

## Excluded

- Game executables and assets, prepared guest images, firmware/system libraries, game shader programs and compiled game caches.
- Bloodborne's game-specific patch payload. The staging helper accepts the locally supplied matching file; it is not uploaded.
- Saves, credentials, signing material, built runtime/driver DLLs and toolchain binaries.
- The generated PKG-reader header. It is generated locally from pinned upstream OrbisPkgTool source during a default host build, with a source hash check.

## Build boundaries

The software can be compiled using the documented Windows/MSYS2 and WSL toolchains. A build is not a game-content distribution. The upstream source and toolchain packages require network access or pre-populated download caches. Source-build validation is recorded separately from gameplay validation; rebuilt artifacts are not claimed byte-identical to the former PGO/profile-dependent development builds.

Required upstream build/generation helpers are retained with their source dependencies. Internal development notes, conversation exports and artwork-generation prompts are excluded from the published source and history.
