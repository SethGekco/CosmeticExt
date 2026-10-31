# Changelog — CosmeticExt

> Convention: **every change gets an entry here, in the same commit.** Several
> sessions work across this project and its siblings without seeing each
> other's context, so this file is the shared channel. Newest first, and always
> say *why*: the reason is what saves the next agent.

## [unreleased] — 2026-10-30

### Decided — `ColorRules=` overlaps and owner changes (TODO.md #1)
- Rex answered the three overlap examples on 2026-10-30: by default a rule
  never passes along a chain.
  - A delivered refinery's free harvester follows the refinery's
    `FreeUnit.ColorRules`, not the superweapon's.
  - A clone doesn't get the barracks' `Factory.ColorRules`.
  - A deployed yard uses its own type's rule.
- Each case gets its own opt-in tag to carry the colour over instead. The
  names in TODO.md are suggestions only.
- By default a rule stays when the owner changes.
- Only the cycle's default scope is still open. Rex asked what the earlier
  reading meant, so it was explained with an example. Why: so the builder
  has every default before writing any code. Nothing is built yet.

### Decided — five of the six `ColorRules=` questions (TODO.md #1)
- Rex answered on 2026-10-30. His words are quoted verbatim in TODO.md, each
  followed by what the builder takes from it:
  - the cycle scope is an INI setting, and by default one sequence runs for
    the whole game;
  - `Repeat=` names where the cycle restarts;
  - remap and palette rules both apply, with a toggle to make one replace;
  - `Invoker` falls back to the neutral (civilian) colour;
  - remap still applies on a switched palette when it can.
- Still open: what happens when two rules reach one object along a chain (a
  delivered refinery's free harvester, clones, an MCV deploying), and whether
  a rule survives an owner change. TODO.md has an example of each and the
  recommendation sent to Rex. Why: Rex asked for examples before deciding.
  Nothing is built yet.

### Deployed — LOCAL fix and `[SoundList]` diagnostic
- CI run 41159376142 (commit fadc0fb; build and test green, 0 warnings)
  produced `CosmeticExt.dll` with sha256 `0c81c55a…`. It was copied
  byte-identical into the live game dir, replacing `e93526f2…`. Backups:
  `CosmeticExt.dll.bak-cosmeticext-20261030-192016` and the matching `.map`.
- Same six hooks at the same seats and sizes as before. It imports only
  KERNEL32. Registration is unchanged: it is already in both `wine-game.sh`
  and `ClientDefinitions.ini`. Not yet seen running.

### Verified in game — the Explosion14 listening test
- Two games on 2026-10-26 (00:17 and 00:29) ran with the listening test
  installed. debug.log listed Explosion14 and `Explosion14_D1..D3`, with
  `bandOf=` and the inherited model, and logged 20 runtime decisions. All of
  them match the design:
  - the bands switch past 4, 14 and 32 cells (12.6 → D1, 14.1 → D2,
    46.7 → D3);
  - the delay is 0.25 frames per cell, rounded and capped at 15 (22 cells →
    6 frames, 56.2 → 14);
  - the echo happens only from 14 cells (12.5 → none, 14.1 → one).
  So the PlayAt hook's band rewrite and deferral run in game. Volume, pan and
  the echo's actual playback are not logged; Rex's ears are the check there.
- **One sound short.** The engine reported 820 sounds. The installed
  soundmd.ini has 850 `[SoundList]` keys and 821 distinct names, and all
  three variants are among the 820, so one other sound is missing. The same
  count against the vanilla soundmd matched exactly (818). Offline checks
  ruled out duplicate keys, key and section CRC clashes, non-numeric keys,
  names over 31 characters and missing samples. Hence the new diagnostic
  below.
- Games launched from 2026-10-26 19:31 on had no Ext DLL injected at all.
  syringe.log recognised only Antares, CnCNet-Spawner, PhobosCompatExt and
  Phobos (2,973 hooks), so those logs say nothing about CosmeticExt. This is
  not a CosmeticExt fault: every sibling DLL was missing in the same way.

### Added — `[SoundList]` loss diagnostic
- At load, every distinct `[SoundList]` name is checked against the engine's
  sound list, the same way `LoadAll` and `FindIndex` compare names. Any name
  that did not become a sound is logged as
  `[CosmeticExt] [SoundList] <key>=<name> did not become a sound.`
  A totals line follows whenever names and sounds differ. So the next game
  names the sound missing from the 820, or shows that the engine's INI saw
  fewer keys than the file has.

### Fixed — `Type=LOCAL` sounds use the engine's distance
- The band pick, the travel delay and the echo threshold now measure a LOCAL
  sound from the screen centre, as the engine's volume function does
  (`test bl,0x40` at 0x750BFA). They used to measure every sound beyond the
  screen edge, so a LOCAL sound with `Distance.Bands` would have switched band
  at a different distance from the one it fades by. Vanilla has 31 LOCAL
  sounds (ambients and loops). The volume and pan hooks were already right,
  since they read the engine's own locals.
- The maths moved into `DistanceModel::ViewDistance`, with 14 new host checks
  for both metrics (74 in all). `ReadINI` now warns when only one of a sound
  and its band is LOCAL, and the per-sound debug.log line marks LOCAL sounds.
  Explosion14 is not LOCAL, so the installed listening test plays the same.

### Added — project
- `TODO.md`, starting with Rex's request for `ColorRules=` (verbatim):
  - remap-colour cycles, `Owner` and `Invoker` remaps, and `PaletteSwitch`;
  - set per TechnoType, superweapon (UnitDelivery, ParaDrop), factory and
    `FreeUnit`;
  - with civilian/neutral units as the key case.
- The notes there give the engine fields, the Antares prior art (anim
  `LightConvert` re-owning) and the open decisions. They decide nothing that
  Rex hasn't. It belongs in CosmeticExt because it changes colours only: the
  zombie example keeps `civilian` as the owner.

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
- Host tests: `tests/run_tests.sh`, with 60 C++ checks (74 since the LOCAL fix) of the distance model
  and 21 Python tests of the baker (23 since the level fix).
