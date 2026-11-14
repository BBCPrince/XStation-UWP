# Game compatibility and performance

Last reviewed: **2026-11-14**. These are recorded **Xbox Series X Developer Mode** results, not general emulator compatibility claims. Series S has not been verified. No complete campaign has been verified for any title.

**Build scope:** gameplay results below come from earlier installed development binaries. The source rebuild documented in [BUILD-VALIDATION.json](BUILD-VALIDATION.json) passed build/package checks, but has **not** been gameplay-tested. Rebuilding this repository does not guarantee the same results. God of War and Detroit's later development tests also differ from their packaged engine snapshots.

## Games tested

FPS describes the specified scene and settings. Loading, first-time shader compilation and heavier areas can be much slower. “In-game” means gameplay was reached; it does not mean the whole game is stable or runs at that rate.

| Game / title ID | Recorded status | Measured Xbox FPS | How far verified | Settings and limits |
| --- | --- | --- | --- | --- |
| **Bloodborne** — `CUSA00900`, 1.09 | In-game; limited route tested | **58–60** in the tested scene; latest warm-cache run **59–60** | Loaded a saved character, movement and menus; Hunter's Dream/lamp scene | **960×540**, dynamic shadows off, uncapped patch; FSR upscaling off for the default measurement. Loading/shader dips remain. Later areas, bosses and campaign completion unverified. |
| **The Last of Us Remastered** — `CUSA00552` | In-game; limited checkpoints tested | **25–30** in the recorded XStation gameplay checks; later default check **29–30** | Menus, Continue into gameplay, pause menu and controls from saved checkpoints | 30 FPS cap, compact-memory/reduced-texture setup. The test notes do not identify a completed chapter or campaign milestone. These results do **not** establish 30 FPS in the demanding courtyard or every later area. |
| **Uncharted 1: Drake's Fortune**, within The Nathan Drake Collection — `CUSA02343` | In-game; early areas tested | **30 capped** in later cache tests; early boat-deck test **58–59 average uncapped** | Early U1 jungle and boat-deck saves; movement and loading transitions | Memory/texture reductions and title-specific graphics workarounds. Uncapped 15-minute test averaged **58.4**, with slowest 5% around **53**: not a locked 60. Later chapters unverified. |
| **God of War (2018)** — `CUSA07408`, 1.35 | In-game in earlier standalone development tests; below target performance | **16–17.7** in the later standalone test | Opening gameplay, including the Leviathan Axe swing prompt | This later performance trial was **not integrated into XStation's packaged engine**. Earlier memory/startup failures and slowdowns occurred; sustained 30 FPS and later progress unverified. |
| **Detroit: Become Human** — `CUSA08344`, base 1.00 | In-game in development tests; limited first-chapter route | **27.9–28.3 average** on the tested walk; **29–30** in lighter stretches, slower in heavier sections | Chapter 1, **The Hostage** tutorial/hallway route; intro also reached | Later tuned runtime differs from the packaged snapshot. Streaming/shader hitches remain; Audio3d is implemented as a silent sink, so audio support is incomplete. Later chapters unverified. |

## Not verified

| Game | Status |
| --- | --- |
| **Uncharted 2: Among Thieves** | No confirmed gameplay/FPS test. Inclusion in the collection's game files does not establish compatibility. |
| **Uncharted 3: Drake's Deception** | No confirmed gameplay/FPS test. Inclusion in the collection's game files does not establish compatibility. |
| **Marvel's Spider-Man (PS4)** | No title runtime supplied in this repository; no XStation gameplay/FPS result. Separate launcher projects and dashboard artwork are not proof of support here. |
| Other PS4 games | Not established by these five title-runtime snapshots. |

## Reading the numbers

- **TLOU's optional “60 FPS (FSR)” setting uses frame generation.** One recorded run presented **55–59 FPS** while the game itself ran at **28–29.5 FPS**. That is not native 60 FPS simulation/input response; the table uses frame generation off.
- **Bloodborne's FSR option is upscaling**, from 540p to 1080p output in the recorded tests. The native scene remains 540p. The old 720p/1080p labels did not prove higher native rendering resolution and are not used here.
- **Uncharted's uncapped result is scene-specific.** Later 30-cap tests still recorded loading/streaming dips; one earlier same-route memory test sampled dips to 22–26 FPS.
- Warm caches improve repeat runs. For example, Bloodborne's cold cache-fill load briefly fell to 3–14 FPS, while the repeat cached load dipped to 42–47; gameplay in that measured scene remained 59–60.
- These are short development measurements, not a cross-title benchmark. Some captures/profiling introduced hitches. No FPS or memory figure here guarantees stability throughout a game.

## Test scope

This summary uses recorded development tests from November 7–13, 2026, including earlier standalone title builds where noted. Raw logs, screenshots, game files and saves are not distributed.

Historical snapshot hashes are recorded in [engines/manifest.json](../engines/manifest.json); fresh build hashes are in [BUILD-VALIDATION.json](BUILD-VALIDATION.json). Neither file claims binary equivalence between those versions.

For future reports, record the console model, runtime/driver hashes, game version, resolution/texture settings, frame cap/generation setting, checkpoint, cold/warm cache state, test duration, average/low FPS and furthest point actually reached. Update a title's progress only after that route is verified.
