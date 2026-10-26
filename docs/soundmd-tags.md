# CosmeticExt soundmd.ini tags

Everything here goes in **soundmd.ini**. Nothing touches rules, AI or
simulation state: it only changes what you hear, so it cannot desync an online
game (the delayed and echoed sounds draw from the engine's unsynced audio
randomizer, never the game's synced one).

There are two halves:

- **Runtime tags** (`Distance.*`), read by `CosmeticExt.dll` every game. They
  cover what can change *while the game runs*: which sound plays at a given
  distance, how loud it is, where it sits in stereo, and when it arrives.
- **Bake tags** (`Distance.Bake.*`), read by the offline tool
  `tools/distance_bake.py`. They cover what the game's mixer cannot do live,
  the *frequency* side of distance (muffling, ground-reflection hollowness,
  scattered tail). The tool renders new sample files for that.

## What "distance" means

Every distance here is in **cells beyond the edge of the screen**. This is the
engine's own sound metric (positional volume function 0x750AC0): the
Chebyshev distance in pixels from the source to the tactical view's edge, with
the vertical part doubled for the isometric view, divided by 60 px per cell.

- `0` means the source is on screen.
- `Range=` in vanilla soundmd.ini uses the same unit. A vanilla sound fades
  linearly from full volume at the screen edge to silence `Range` cells
  beyond it.
- Sounds with `Type=global` get a distance too, so bands and the other tags
  work for them. Vanilla fades them with distance like any other sound, but
  never below their `MinVolume=` (that floor is what "global" means in the
  engine). The floor is applied after CosmeticExt's adjustment, so a global
  sound stays audible everywhere.

## Runtime tags

### Distance bands: a different sound by distance

```ini
[ExplosionLarge]
Sounds= $bigboom1 $bigboom2
Distance.Bands=ExplosionLarge_Near,ExplosionLarge_Far
Distance.Bands.From=4,20
```

| Tag | Default | Meaning |
|---|---|---|
| `Distance.Bands=` | none | Sounds from `[SoundList]` to play instead of this one when the source is far away, nearest first. Comma or space separated. |
| `Distance.Bands.From=` | none | Where each band starts, in cells beyond the screen edge, one value per band. A band applies once the source is **more than** its value away, so `0` means "any off-screen source", and an on-screen source always plays the sound itself. |

- The choice is made once, when the sound starts.
- A band sound is a normal soundmd.ini sound with its own `Sounds=`,
  `Volume=`, `Range=`, `Control=` and so on. It needs its own entry in
  `[SoundList]`.
- Give far bands a larger `Range=` than the base. Otherwise they go silent
  where the base would have.
- A band **inherits every `Distance.*` tag of the sound that lists it**, unless
  it sets its own. A far variant therefore falls off like its base without
  repeating the tags.
- The two lists are checked against each other, and every problem goes to
  `debug.log`. If they differ in length, the band is dropped. Unknown sound
  names and a sound listing itself are dropped too, and an unsorted `From`
  list is sorted.

### Spreading loss: how loudness falls with distance

| Tag | Default | Meaning |
|---|---|---|
| `Distance.Falloff=` | `linear` | `linear` (alias `vanilla`) is the engine's straight-line fade to silence at `Range`. `inverse` (alias `physical`) is spherical spreading, the inverse-square law: a fixed loss per doubling of distance. |
| `Distance.Rolloff=` | `6.0206` | Inverse only: dB lost per doubling of distance. 6.02 is a point source such as an explosion or a gunshot. 3.01 is a line source such as a convoy, a train or a long burning front. |
| `Distance.Reference=` | `8` | Inverse only: the listener's distance to the screen edge, in cells. A source on screen is heard at full level, and `Reference` cells beyond the edge it is twice as far away, so 6 dB down. |
| `Distance.Fade=` | `0.25` | Inverse only: the fraction of `Range` over which the level is eased to silence (smoothstep), so `Range` stays a hard limit without the sound cutting off abruptly. `0` cuts at `Range`. |

With `inverse`, level = (Reference / (Reference + d))^(Rolloff / 6.02),
eased out over the last `Fade` of `Range`. Inverse is louder than vanilla
near the screen and keeps a long tail, which is how distant artillery
actually sounds.

### Atmospheric scattering

| Tag | Default | Meaning |
|---|---|---|
| `Distance.Scatter=` | `0` | Random level variation, in ±dB, drawn once per play. It grows from 0 on screen to the full value at `Range`. Wind and temperature layers make distant sound swell and drop, and this is that effect. A sound that stays attached to a unit keeps its draw for as long as it plays, so it does not flutter frame to frame. |
| `Distance.Diffuse=` | `0` | 0..1: how far the stereo position collapses to centre at `Range`, growing from 0 on screen. A distant source's direction is smeared by reflections, so at `1` a sound at `Range` is dead centre. |

### Travel time and echoes

| Tag | Default | Meaning |
|---|---|---|
| `Distance.Delay=` | `0` | Sound travel time in **frames per cell**. Applied when the sound starts. |
| `Distance.Delay.Max=` | `45` | Cap on the delay, in frames. |
| `Distance.Echo=` | `0` | Number of echoes (0..8): delayed, quieter repeats. |
| `Distance.Echo.Delay=` | `8` | Frames between repeats (minimum 1). |
| `Distance.Echo.Volume=` | `0.35` | Level of each repeat relative to the previous one (0..1). |
| `Distance.Echo.From=` | `0` | Echo only when the source is at least this many cells away. |
| `Distance.Echo.Sound=` | the sound | A different sound for the echoes, such as a darker or shorter tail. Set it per sound. It is not inherited and has no `[CosmeticExt]` default. |

Delays and echoes are in game frames, so they scale with game speed exactly
like projectiles and animations do. For scale: real sound covers 343 m/s. At
25 m per cell (the baker's default) that is about 0.073 s per cell, roughly
2.2 frames per cell at 30 frames/s. That is realistic, but a 20-cell delay of
1.5 s feels like lag in an RTS. Most mods will want a stylised `0.25`–`0.5`
with `Delay.Max` around 15–30.

Only **one-shot** sounds are delayed or echoed. A sound plays now, untouched,
if it loops (`Control=loop`) or if the unit or anim playing it already has a
sound running on that channel (vanilla is keeping or replacing it). A delayed
sound plays later from where it started, as a free one-shot. That is right
for explosions, since the anim that made the sound is often gone by then. The
queue is cleared when a new game starts or a save is loaded.

### Inheritance and `[CosmeticExt]`

A `Distance.*` tag is resolved in this order:

1. the sound's own section,
2. the sound that lists it in `Distance.Bands` (and that sound's base, up to 8 levels),
3. `[CosmeticExt]` in soundmd.ini,
4. the defaults above, which are vanilla behaviour.

`Distance.Bands`, `Distance.Bands.From` and `Distance.Echo.Sound` are never
inherited.

```ini
[CosmeticExt]
Enabled=yes            ; master switch; no = everything here is off
LogBudget=30           ; max runtime lines in debug.log per launch (band picks, delays)
QueueMax=128           ; max delayed sounds/echoes waiting at once (0..4096)
Distance.Falloff=inverse  ; any Distance.* key here is the default for every sound
```

At startup `debug.log` gets a `[CosmeticExt]` summary: how many sounds have
distance behaviour, then one line per sound you configured showing the
resolved values. Check it after editing.

## Bake tags (offline: `tools/distance_bake.py`)

gamemd mixes each sound with a volume and a pan and nothing else. There is no
filter stage and no reverb send, so muffling can't be applied live. The baker
applies it ahead of time, from the physics, and writes three things:

- a new **audioNN.bag/.idx** holding the variants (default `audio97`). Ares
  and Antares load `audio01`..`audio99` on top of `audio.bag`, so no game file
  is replaced.
- a copy of **soundmd.ini** with one generated section per variant, and the
  base sound's `Distance.Bands` / `Distance.Bands.From` filled in.
- **bake-report.txt**: the physics per band, in plain numbers.

Mark sounds, then run it:

```ini
[ExplosionLarge]
Sounds= $bigboom1 $bigboom2
Distance.Bake=yes
```

```bash
python3 tools/distance_bake.py --soundmd mymod/soundmd.ini --game "C:/Westwood/RA2" --out bake-out --preview bake-out/preview
```

Copy `bake-out/audio97.idx`, `audio97.bag` and `soundmd.ini` into the mod.
The input soundmd.ini is never modified. Re-running on a baked soundmd.ini
first removes everything a previous run generated (every generated line ends
in, or every generated section starts with, `;cebake`), so you can tweak the
tags and bake again. Python 3 and numpy are needed.

| Tag | Default | Meaning |
|---|---|---|
| `Distance.Bake=` | `no` | Bake distance variants of this sound. |
| `Distance.Bake.Bands=` | `4,14,32` | Where the bands start (becomes `Distance.Bands.From`), cells beyond the edge. Up to 9. |
| `Distance.Bake.CellMeters=` | `25` | World scale for the physics: metres per cell. |
| `Distance.Bake.Temperature=` | `20` | Air temperature, °C. |
| `Distance.Bake.Humidity=` | `70` | Relative humidity, %. Dry air muffles much faster: at 20 °C, 4 kHz is lost about three times as fast at 20 % RH as at 70 %. |
| `Distance.Bake.Air=` | `1` | Absorption multiplier, an artistic scale on the physics (2 = twice the muffling). |
| `Distance.Bake.Ground=` | `0.5` | Ground reflection strength (0..0.95). `0` turns off the hollow comb filter. Hard ground, water and concrete reflect more (0.7–0.9); grass and snow reflect less (0.2–0.4). |
| `Distance.Bake.SourceHeight=` | `2` | Height of the sound source, metres. |
| `Distance.Bake.ListenerHeight=` | `30` | Height of the listener (the camera), metres. Together with the source height it sets where the comb's notches fall. |
| `Distance.Bake.Tail=` | `0.35` | Level of the scattered tail at the farthest band. It scales down linearly for nearer bands. |
| `Distance.Bake.TailTime=` | `1.4` | Tail decay time (T60, seconds) at the farthest band, scaled down the same way. |
| `Distance.Bake.Range=` | `auto` | `Range=` of the variants. `auto` = the larger of the base's Range and 2 × last band + 10. |

Every `Distance.Bake.*` key in `[CosmeticExt]` is the default for all baked
sounds. Command-line options:

- `--only A,B`: bake just those sounds.
- `--bag N`: write `audioN` instead of `audio97`. The tool refuses to
  overwrite an audioNN it did not write unless you pass `--force-bag`.
- `--format adpcm`: a quarter of the size, but see below.
- `--search DIR`: extra folders of loose samples.
- `--dry-run`: print the report and write nothing.

### What the baker does, per band

For a band starting at `d` cells, its representative distance is midway to the
next band, or 1.5 d + 2 past the start for the last band. The source is then
(`Reference` + d) × CellMeters metres away. Of that path, d × CellMeters is
extra: the on-screen sound doesn't already have it.

1. **Air absorption.** ISO 9613-1 atmospheric absorption over the extra path.
   High frequencies go first: at 20 °C and 70 % RH, about 5 dB/km at 1 kHz but
   77 dB/km at 8 kHz. This is the muffled, boomy far explosion. The tool's
   values match the ISO 9613-2 tables, and its tests check that.
2. **Ground reflection.** A second path off the ground, delayed by the path
   difference, forms a comb filter, which is the hollow, phasey quality of
   distant sound. It reshapes the tone without making the sound louder or
   quieter.
3. **Scattered tail.** A decaying noise reverberation, itself air-absorbed,
   that grows with each band. This is the rolling of far thunder. It spreads
   the sound out in time without adding energy.

**Level.** Each variant keeps exactly the energy that air absorption alone
leaves of its own sample, so only air makes a band quieter. If a peak would
go above full scale, a limiter turns down just the few milliseconds around
it, not the whole variant. The bake report notes any cut deeper than 3 dB.

**Spreading loss is not baked.** CosmeticExt applies it at run time, smoothly
and at the exact distance. If a baked sound doesn't set a `Distance.Falloff`
(and neither does `[CosmeticExt]`), the baker adds `Distance.Falloff=inverse`
to it, so the level is continuous across the band edges.

Variants are 16-bit PCM by default. The game's own codec, IMA ADPCM
(`--format adpcm`), adds hiss about 20 dB under the signal. A full-range sample
masks that, but a muffled far band doesn't: on vanilla `gexp14a`, band 3's top
octave goes from −56 dB in PCM to −20 dB in ADPCM, so the muffling would be
undone.

## Full example

See [examples/soundmd-distance-example.ini](examples/soundmd-distance-example.ini).
