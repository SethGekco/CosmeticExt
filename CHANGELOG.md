# Changelog — CosmeticExt

> Convention: **every change gets an entry here, in the same commit.** Several
> sessions work across this project and its siblings without seeing each
> other's context, so this file is the shared channel. Newest first, and always
> say *why*: the reason is what saves the next agent.

## [unreleased] — 2026-10-25

### Deployed — Explosion14 listening test
- Installed loose in the live game dir, with no file there replaced:
  `soundmd.ini` (sha256 `f4bfb36c…`), `audio97.idx` (`6a943ef0…`) and
  `audio97.bag` (`5e2e183d…`, 12 PCM samples, 0.7 MiB).
- The soundmd is the one the game was reading: vanilla `ra2md.mix/localmd.mix`,
  818 sounds, matching the logged count. It is not the mod folder's 871-sound
  soundmd.ini. Changes:
  - `[CosmeticExt] LogBudget=200`;
  - Explosion14 (the tank-shot impact, `Report` of S_CLSN58) gets Scatter 2,
    Diffuse 0.5, Delay 0.25, Echo 1 from 14 cells;
  - the baker's bands `Explosion14_D1..D3`, from 4, 14 and 32 cells, and
    `Distance.Falloff=inverse`.
- Every non-blank vanilla line was checked as kept, in order. `[SoundList]`
  has 821 unique names and no duplicate keys, and all 12 variant samples are
  present in the bag. The vanilla copy is saved beside it as
  `soundmd.ini.vanilla.bak-cosmeticext-20261025-225445`.
- To revert, delete the three files. The original never left its MIX.

### Found — `Type` bits in the volume function
- `0x800` returns silence for sources in unrevealed cells. `0x40` (`Local`)
  measures from the screen centre instead of beyond the edge. `Screen` is
  never tested. The DLL's `EdgeDistance` ignores `Local`, so a LOCAL sound's
  band, delay and echo distance would disagree with its fade. Explosion14 is
  not LOCAL. The fix is still open (DESIGN.md, "Known gap").

### Fixed — baker levels
- Far variants were too loud, then turned down as a whole. The ground comb and
  the scattered tail were energy-normalised against white noise. An explosion
  is mostly bass, where the comb adds in phase and the air-absorbed tail
  concentrates, so on vanilla `gexp14` the far bands came out up to 2.9× the
  energy air absorption leaves. The clip guard then scaled whole variants down
  1.5–9.3 dB, giving an audible level step at every band edge. It was found on
  the first Explosion14 listening-test bake, before anything was installed.
- Now each variant keeps exactly the energy air alone leaves of its own sample,
  and full scale is kept by a look-ahead peak limiter (`limit_peaks`, about
  1.5 ms around each peak) instead of whole-variant scaling. Measured on
  `gexp14a`–`d` and `snukexpl`: level within 0.16 dB of air-only, deepest
  limiter cut 1.9 dB. The bake report now notes cuts deeper than 3 dB.
- Two tests added: a bass-heavy sound's energy must equal air-only within 1%
  (the old code gave 1.74×), and the limiter must leave the body untouched,
  including peaks at the very first and last frame.

### Verified in game
- **VERIFIED, 2026-10-25, first launch through the CnCNet client.** In
  syringe.log CosmeticExt is recognised and its handshake answers 0. The
  hook count went from 3547 to 3553, i.e. our five plus the inherited
  0x7504C9. debug.log shows `[CosmeticExt] loaded (built Oct 26 2026
  04:40:14)` (the deployed CI build) and `soundmd.ini: 818 sound(s), 0 with
  distance behaviour`. That proves the LoadAll-exit hook fires and the
  handle-dereferencing sound walk reads the loaded mod's sounds. The game
  ran 32 minutes and Syringe exited with code 0. Its only exception was the
  same first-chance C++ one (0xE06D7363) that runs without CosmeticExt also
  log.
- Not yet exercised: the distance behaviour itself. No sound in the mod's
  soundmd.ini uses the tags, so the PlayAt, volume and pan hooks ran their
  inactive paths only.

### Deployed
- CI run 40270184852 (commit 1f300cf, 0 warnings) produced `CosmeticExt.dll`
  with sha256 `e93526f2…11aec72`. It was copied byte-identical into the live
  game dir, with no earlier copy there. `-i=CosmeticExt.dll` was appended after
  WarZoneExt in both `Resources/Compatibility/Unix/wine-game.sh` and
  `Resources/ClientDefinitions.ini`; backups are `*.bak-cosmeticext-20261025-214212`.
  It imports only KERNEL32, like WeatherExt. It has not yet been seen running:
  the game that was open at deploy time was launched before it was registered.
- Documented in DESIGN.md: the DLL also installs Phobos `Debug.cpp`'s
  `0x7504C9` hook and its four byte patches, as every sibling Ext DLL does.
  This is harmless (log-only and identical bytes), but the overlap checker
  cannot see it.

### Changed — build
- Silenced the one MSVC warning (C4996, `TacticalClass::CoordsToClient`
  deprecated) at its single call, with the reason in the code. YRpp prefers an
  inline twin that measures against different view bounds (0xB0CE28), but
  the engine's own volume function calls 0x6D2140 (at 0x750BB9). Switching
  would make the band distance disagree with the distance the engine
  attenuates by.

### Added — distance-dependent sound (soundmd.ini)
- `Distance.Bands=` / `Distance.Bands.From=`: play a different sound by
  distance. This was the first ask: a close, a nearby-off-screen and a far
  version of an explosion. The pick is made at `PlayAt` (hook 0x7509E0), by
  rewriting the sound index before vanilla reads it. Bands inherit their
  base's `Distance.*` tags, so a far variant falls off like its base unless it
  says otherwise.
- `Distance.Falloff=inverse` with `Distance.Rolloff` / `Reference` / `Fade`:
  spherical spreading (−6.02 dB per doubling) in place of vanilla's linear fade
  to `Range`. `Range` stays the audibility limit, eased out over `Fade`, so no
  sound starts reaching further than its author allowed.
- `Distance.Scatter=` (random ±dB growing with distance) and `Distance.Diffuse=`
  (stereo collapse toward centre): atmospheric scattering, the only parts of it
  a volume and pan can express. Scatter for controller-held sounds is a hash of
  (controller, sound), because 0x750D40 re-levels those every frame and a fresh
  draw would flutter.
- `Distance.Delay=` / `Delay.Max` and `Distance.Echo=` / `Echo.Delay` /
  `Echo.Volume` / `Echo.From` / `Echo.Sound`: travel time and multipath
  repeats, through a frame-driven queue (main-loop hook 0x55D3DA). Only
  non-looping sounds with an idle or absent controller are deferred: a loop
  played free would never stop, and a busy controller means vanilla wants to
  keep or replace what it plays.
- `[CosmeticExt]` in soundmd.ini: `Enabled`, `LogBudget`, `QueueMax`, and any
  `Distance.*` key as a default for every sound. Precedence: own > band base >
  `[CosmeticExt]` > vanilla.
- Everything uses the engine's own distance unit (Chebyshev pixels beyond the
  view edge, vertical doubled, ÷ 60), so `Range=` and the new tags mean the
  same thing. All of it is multiplayer-neutral: audio draws only from the
  unsynced randomizer.

### Added — `tools/distance_bake.py`
- Bakes the frequency side of distance, which the engine's mixer cannot do live
  (a volume and a pan per channel, no filter, no send): ISO 9613-1 air
  absorption, a ground-reflection comb, and a scattered tail. It writes an
  `audioNN.bag/.idx` that Antares loads on top of `audio.bag`, plus a
  soundmd.ini copy with the variant sections and `Distance.Bands` filled in.
- Air is applied over the path **beyond** the on-screen distance only, since
  the vanilla sample already is the on-screen sound. The first draft applied it
  over the full distance and double-counted.
- PCM16 output by default. IMA ADPCM's noise refilled band 3's top octave
  (−56 → −20 dB on vanilla `gexp14a`) and undid the muffling.
- Stereo IMA ADPCM in YR's bag is byte-interleaved L/R, not the MS-IMA
  4-byte-group layout. That was measured on all five vanilla stereo ADPCM
  samples (2–3× smoother); the decoder/encoder use the measured layout.
- It is idempotent (`;cebake` markers), reuses renders across repeated or
  shared samples, and refuses to overwrite an `audioNN` it did not write.

### Added — project
- Repo scaffold from WeatherExt: YRpp 3ba9495 / Phobos 4747562, MSVC v142 CI
  with the hook-overlap and hook-bounds checks against the YR Hook
  Encyclopedia registry, plus a Linux job running the host tests.
- `tools/mixread.py`, vendored verbatim from MapForge commit 647c8ac (header
  line only).
- Host tests: `tests/run_tests.sh`, with 60 C++ checks of the distance model
  and 21 Python tests of the baker (23 since the level fix).
