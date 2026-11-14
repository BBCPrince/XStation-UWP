# Third-party notices

XStation's software is licensed as a whole under the GNU General Public License v2.0 or later (see `LICENSE`), subject to the retained third-party notices below. This does not claim sole authorship of adapted code.
It includes or is based on the following works.

## OrbisPkgTool (MIT)

`source/pkg_extract.cpp` and `pkg_extract.h` are a C++ port of OrbisPkgTool's PKG reader
(`PkgReader.cs`, `Pfs/PfsReader.cs`, `Pfs/PFSCStream.cs`, `Crypto/PkgCrypto.cs`).

Upstream: https://github.com/pearlxcore/OrbisPkgTool
Local source revision inspected: `5cd42a515dc29a453c7abcb93318493bdb5b5e1f`.
The local MIT notice was compared with the notice below on 2026-11-14 and retained verbatim.

```
MIT License

Copyright (c) 2026 pearlxcore

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## miniz (MIT)

`source/third_party/miniz/` is miniz by Rich Geldreich, RAD Game Tools and Valve Software.
Its license is in `source/third_party/miniz/LICENSE`.

## Game preparation and UWP adaptation (GPL-2.0-or-later)

`source/game_prepare.cpp` ports preparation/linking/content-profile logic from locally modified game-port scripts, including Supermedo's Uncharted PC project (`scripts/prepare.py`, `link_modules.py`, and `content_profile.py`). Local source revision inspected: `8b66edc0192ea246ca63b360857402a0aa55626d`.

- Supermedo / Mohammed Albarghouthi and contributors: https://github.com/Supermedo/uncharted_pc and https://github.com/Supermedo/bloodborne_pc
- Earlier bbport work by deadinside28 and contributors: https://github.com/deadinside28/bloodborne_pc
- shadPS4 contributors: https://github.com/shadps4-emu/shadPS4

The cited port projects retain their own upstream attribution. XStation's local UWP host was adapted through the Bloodborne and TLOU host projects, and the native compiler-cache integration through the local Spider-Man project. These local adaptations are not represented as unmodified upstream code. See `provenance.json` for the verified local chain and its limits. XStation modifications were made in November 2026; Git history records the changes in this repository.

## Runtime source snapshots and graphics components

The game runtimes XStation loads (`*-uwp-runtime.dll`) are based on GPL-2.0-or-later port/renderer sources. Their modified local source snapshots are now included under `engines/<title>/`, with retained upstream license texts and notices. The relevant roots include Supermedo's Bloodborne/Uncharted projects, earlier bbport and shadPS4 contributions, and the local UWP adaptation chain recorded in `provenance.json`. Standard host/runtime interfaces remain in `source/uwp_host.h` and `source/runtime_text_input.h`.

`vulkan_dzn.dll` is built from Mesa Dozen and is a separate graphics component, not itself described here as a GPL-only game runtime. Mesa includes multiple licenses, predominantly MIT-style; retain the exact notices for the source revision and dependencies actually distributed. Mesa licensing reference: https://docs.mesa3d.org/license.html

The pinned Mesa archive and local full-source replacements are described in `graphics/mesa`. Source modifications include the shader/compiler, memory/resource and host native-cache integration used by the local graphics path. Original file notices are retained. Title source snapshots also retain the notices for their bundled FSR/AMD SDK source, ImGui, LibAtrac9, shadNet and other source dependencies.

A binary release must include the applicable dependency notices and satisfy the corresponding-source requirements for the exact components distributed, including dynamically linked MSYS2 libraries where required. The build records standard toolchain package versions; this does not certify complete binary-distribution compliance or transfer third-party trademark rights.

## Not included

- No game executables/assets, original guest instructions, game shader programs, built runtime DLLs, or driver DLLs. Numeric preparation lists contain offsets, instruction lengths and hashes only; they reconstruct metadata without publishing original instructions. Bloodborne's separate game-specific patch payload remains a local input.
- No generated key-bearing header is committed. The default host build fetches hash-pinned MIT-licensed OrbisPkgTool source and generates the needed header locally under `out/`, with its retained attribution.

## Trademarks

PlayStation, PS4, Xbox and the game titles named in the code and notes are trademarks of their
owners. XStation is not affiliated with or endorsed by Sony Interactive Entertainment, Microsoft or
any game publisher.
