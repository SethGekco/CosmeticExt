# CosmeticExt

A Syringe DLL for Yuri's Revenge that improves what you see and hear, and
nothing else. It never touches game state, so it is safe in online games.
Everything is opt-in through INI tags, and the defaults are vanilla.

## Distance-dependent sound

Vanilla sounds are either heard everywhere (`Type=global`) or fade linearly
to silence a few cells off screen. CosmeticExt adds soundmd.ini tags that
make distance sound like distance:

- **Different sounds by distance.** For example, an explosion uses one sound
  on screen, another when it's nearby but off screen, and a third far away:
  `Distance.Bands=` / `Distance.Bands.From=`.
- **Spreading loss.** The inverse-square law, −6 dB per doubling of distance,
  instead of a straight-line fade: `Distance.Falloff=inverse`.
- **Atmospheric scattering.** Distant sound swells and drops with wind
  (`Distance.Scatter=`), and its direction smears (`Distance.Diffuse=`).
- **Travel time and echoes.** Far sound arrives late (`Distance.Delay=`) and
  can come back off the terrain (`Distance.Echo=`).
- **Frequency effects, baked.** Air soaks up the high end, the ground
  reflection makes distant sound hollow, and scattering adds a rolling tail.
  The game's mixer can't filter live, so `tools/distance_bake.py` renders
  muffled variants of your sounds from the physics (ISO 9613-1 air absorption,
  ground-reflection comb, scattered tail), writes them to an extra
  `audio97.bag` and wires them up in soundmd.ini.

Full tag reference: [docs/soundmd-tags.md](docs/soundmd-tags.md).
Example: [docs/examples/soundmd-distance-example.ini](docs/examples/soundmd-distance-example.ini).

## Install

1. Put `CosmeticExt.dll` in the game folder, next to `gamemd.exe`.
2. Add `-i=CosmeticExt.dll` to the Syringe command line (or to your client's
   launch definitions). Order relative to Ares/Antares/Phobos does not matter.
3. Add tags to soundmd.ini. At startup `debug/debug.log` lists every sound you
   configured, with its resolved values.

Distance bands and the baked bags need **Ares or Antares**, which load
`audio01`..`audio99` bags. The runtime tags work on their own.

## Baking

```bash
pip install numpy
python3 tools/distance_bake.py --soundmd mymod/soundmd.ini --game "/path/to/RA2" --out bake-out --preview bake-out/preview
```

Mark sounds with `Distance.Bake=yes` first. The tool:

- reads samples from the game's MIX files and loose files,
- never modifies its input,
- can be re-run on its own output.

`bake-out/bake-report.txt` lists the physics applied to each band.

## Building

CI builds the DLL with MSVC (v142, x86, `DevBuild`) on every push; take the
artifact from the Actions run. YRpp needs MSVC inline assembly, so MinGW
cannot build it.

Host tests, which need no game:

```bash
tests/run_tests.sh
```

They build the distance model with g++ and run the baker's Python tests. The
exit status is the verdict.

## Layout

| Path | What |
|---|---|
| `src/Sound/DistanceModel.*` | The distance acoustics as plain C++, unit-tested on the host |
| `src/Sound/DistanceSound.*` | INI reading, the deferred-sound queue, the hooks |
| `tools/distance_bake.py` | Offline baker for the frequency effects |
| `tools/mixread.py` | MIX reader, vendored from MapForge |
| `tools/check_hook_*.py` | Hook overlap/bounds checks run by CI |
| `DESIGN.md` | Engine notes, hook table, design decisions |
