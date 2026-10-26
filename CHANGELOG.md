# Changelog — CosmeticExt

> Convention: **every change gets an entry here, in the same commit.** Several
> sessions work across this project and its siblings without seeing each
> other's context, so this file is the shared channel. Newest first, and always
> say *why*: the reason is what saves the next agent.

## [unreleased] — 2026-10-25

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
  and 21 Python tests of the baker.
