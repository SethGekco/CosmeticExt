# CosmeticExt — design and reverse-engineering notes

CosmeticExt is a standalone Syringe DLL for Yuri's Revenge, built on the same
toolchain as the other Ext DLLs (YRpp + Phobos helpers, MSVC v142, co-loads
with Antares). It is a home for presentation-only improvements: things that
change what a player sees or hears and nothing else.

Rules for everything in it:

- **No simulation state.** No synced randomizer, no writes to objects,
  houses or rules. That is what makes it safe online, even if only some
  players run it.
- **INI-driven.** Every behaviour is opt-in through tags, and its defaults
  are vanilla behaviour.
- **Testable off the game.** The model code is plain C++ with host unit
  tests. The engine-facing code is a thin layer of hooks over it.

The first feature is distance-dependent sound. The user-facing reference is
[docs/soundmd-tags.md](docs/soundmd-tags.md).

## The engine's positional sound path (gamemd 1.001)

| Address | What |
|---|---|
| `0xB1D378` | `VocClass::Array`: a DynamicVector of **handles**. Each item is a 4-byte object whose only field points at the sound data (handle constructor 0x750380; LoadAll does `mov ecx,[Items+i*4]; mov ecx,[ecx]` at 0x75130F). YRpp's `VocClass` struct is the data, so YRpp's `VocClass::Find`, which reads `Name` through the handle, is wrong. Use `FindIndex` + dereference. |
| data `+0x10` | `Control` flags (loop = `SoundControl::Loop`) |
| data `+0x14` | `Type` flags (`0x10` = Global) |
| data `+0x50` | `Range`, in cells |
| data `+0x54` | `MinVolume` (read through 0x405140) |
| data `+0x6C` | `Name` |
| `0x7510D0` | `VocClass::LoadAll(CCINIClass*)`. Single exit, at 0x7513DD; EDI = the soundmd INI there. |
| `0x7509E0` | `VocClass::PlayAt(int, CoordStruct const&, AudioController*)`, `__fastcall`: ECX = index, EDX = &coords, `[esp+4]` = controller. Its first instruction reads `VoicesEnabled` (0x8464AC); `0x7509EF` is its `ret 4`. |
| `0x750AC0` | Positional volume/pan function. Called from PlayAt (returns to 0x750A48) and from the per-frame controller update 0x750D40 (returns to 0x750D7B). |
| `0x750D40` | Per-frame re-levelling of a sound held by an `AudioController` (units moving while they talk, burning buildings and so on). |
| `0x886B88` | `Randomizer::Global`, the **unsynced** randomizer. Audio uses only this one, which is why adding, delaying or changing PlayAt calls cannot desync. |
| `0x886FA0` | `DSurface::ViewBounds` (tactical view rectangle; Width `+8`, Height `+0xC`) |
| `0x6D2140` | `TacticalClass::CoordsToClient` |
| `0xA8ED84` | `Unsorted::CurrentFrame` |

### The distance metric (inside 0x750AC0)

1. The source's world coords become a client point.
2. The function measures how far that point is beyond each edge of the view:
   `[esp+0x38]` horizontally, `[esp+0x20]` vertically, doubled (isometric).
   Negative values are clamped to 0.
3. Range in pixels is `Range × 60`, at `[esp+0x14]`.
4. If both offsets are inside the range, volume = `(rangePx − max(dx, 2dy)) / rangePx`.
   This is the vanilla linear fade, stored at `[esp+0x18]`. Otherwise volume
   stays 0.
5. At **0x750C9B**: `test bl,0x10 / je 0x750CBD`. For `Type=global`,
   volume = max(volume, MinVolume) (0x750CA0..0x750CB9). That floor is all
   "global" means; vanilla still fades global sounds down to it.
6. At 0x750CBD the result is compared with 0.05 (`0x7E8AE8`). Below that, the
   function returns 0 and the sound is not played.
7. Later the pan is computed into EAX (0..0x4000, centre 0x2000). It is
   available at **0x750D29**, before being stored.

So "distance" = Chebyshev pixels beyond the view edge ÷ 60. CosmeticExt uses
exactly this unit (cells beyond the edge) everywhere, so `Range=` and every
`Distance.*` value mean the same thing.

## Hooks

| Address | Size | Name | What it does |
|---|---|---|---|
| `0x7513DD` | 9 | `VocClass_LoadAll_CosmeticExt` | Reads the `Distance.*` tags and `[CosmeticExt]` from the same INI, after vanilla has built every sound. Stolen bytes: `pop edi; pop esi; pop ebp; add esp,0x824`, no branch. Returns 0. |
| `0x7509E0` | 5 | `VocClass_PlayAt_CosmeticExt` | Picks the band (rewrites ECX) and queues delays/echoes. To defer: `EAX = 0`, then jump to `0x7509EF` (PlayAt's own `ret 4`, nothing pushed yet); the jump is marked `// CHAIN-OK` because nothing else hooks here. Stolen: `mov al,[0x8464AC]`. |
| `0x750C9B` | 5 | `VocClass_ComputeVolume_CosmeticExt` | Replaces vanilla's linear volume with the model's (falloff, scatter, echo gain). The stolen bytes are `test bl,0x10; je` (a **relative branch**), so it never returns 0 and takes both exits itself: `0x750CA0` for global, `0x750CBD` otherwise. The controller comes from the caller's frame: `[esp+0x8]` via PlayAt, `[esp+0xC]` via 0x750D40. |
| `0x750D29` | 5 | `VocClass_ComputePan_CosmeticExt` | Diffuse: pulls EAX toward centre. Stolen: `fld [esp+0x18]; pop edi`, ESP unchanged at replay. |
| `0x55D3DA` | 5 | `MainLoop_CosmeticExt_Tick` | Once per frame: dispatches due delayed sounds and echoes. Stolen: `mov [0xA8B55C],eax`. |

**A sixth hook comes with the toolchain, not from us.** The built DLL's
`.syhks00` also lists `0x7504C9 VocClass_ReadINI_LogMissingSamples`, and its
`.patch` section carries four byte patches (`0x40A55D`, `0x40A5BC` and two
debug.log strings). Both come from `Phobos/src/Utilities/Debug.cpp`, which the
project compiles for `Debug::Log`. Every sibling Ext DLL carries the same set:
WeatherExt, DossierExt, WarZoneExt, IntelExt, BunkerExt and CommandBarExt were
checked on 2026-10-25. Rex's deployed Phobos.dll, a June build, does not.

- **The hook** only logs `[Developer warning] VocClass [...] has missing
  sample` and returns 0, so the copies chain safely. Each DLL adds one
  duplicate warning line per missing sample; the live debug.log had none.
- **The patches** write identical bytes, so applying them more than once
  changes nothing.

`check_hook_overlap` does not see this hook because it scans `src/` only.
Parse the built DLL's `.syhks00` to see everything that is really installed.

Checks run on the seats:

- Disassembly shows no jump into the middle of any stolen span.
- `check_hook_bounds --exe` and `check_hook_overlap` (both in CI) are clean.
- The encyclopedia's `check_hook_chain` is clean, and so is a `.syhks00` sweep
  of every DLL in Rex's live game dir: nobody else hooks these addresses
  (2026-10-25).

### Why deferral is restricted

A delayed or echoed sound is replayed later through `PlayAt(index, coords,
nullptr)`, as a free one-shot. That is right for the main explosion path, an
anim's `Report` at 0x42211A, which gets a fresh controller per anim that is
often gone by the time the sound arrives. Two cases are wrong to defer, so
they always play immediately:

- **Loops.** Played free, they would never stop.
- **Sounds whose controller is busy** (`AudioController::GetEvent()` non-null).
  Vanilla means to keep or replace what is playing there.

### Scatter stability

0x750D40 re-levels a controller-held sound every frame. A fresh random draw
per call would make it flutter, so for controller-held sounds the draw is a
hash of (controller, sound). Free one-shots draw once, at PlayAt.

## Why frequency effects are baked, not live

The engine mixes in a callback on its own audio thread. The decoder is at
0x40AA70, and there are 8 critical sections around the channel state. A
channel has a volume and a pan, and nothing else: there is no filter stage
and no send. A live low-pass or reverb would mean owning that thread's
mixing, which is a large, risky piece of RE for an RTS where the far bands
are only a few discrete steps anyway.

So `tools/distance_bake.py` renders the physics into sample files, and the
DLL picks between them at play time. That's the same split a game audio
designer would use: filtering offline, selection and level at run time.

Leads, if someone wants live filtering later:

- The decoder at 0x40AA70 produces PCM blocks per channel. A per-channel
  one-pole low-pass there, with the coefficient chosen from the channel's
  current distance, would be the minimal version.
- The channel struct would need one float of filter state per channel.
- The 8 critical sections show which data is shared with the game thread.

## Audio bag format (as verified)

- `audio.idx`, "GABA" v2:
  - Header: magic `0x41424147`, version 2, count.
  - Then 36-byte entries: `Name[16]`, Offset, Size, SampleRate, Flags, ChunkSize.
  - Entries are sorted case-insensitively; the engine binary-searches them.
- Flags: `1` stereo, `2` PCM, `4` 16-bit, `8` IMA ADPCM. Vanilla YR has:
  - `12` (mono ADPCM, 512-byte chunks): 2192 samples
  - `6` (mono PCM16): 84
  - `13` (stereo ADPCM, 1024-byte chunks): 5
  - `7` (stereo PCM16): 4
  - every one at 22050 Hz
- **Mono ADPCM** block: int16 first sample, uint8 step index, 1 pad byte,
  then nibbles **low nibble first**. That gives 1017 samples per 512-byte
  block. Only this nibble order makes the block boundaries seamless.
- **Stereo ADPCM** block: one 4-byte header per channel, then the body's
  bytes alternate **L, R, L, R**. Measured on all five vanilla stereo ADPCM
  samples, this decodes 2–3× smoother than the MS-IMA 4-byte-group layout or
  a nibble-per-channel split.
- YR's bag lives in `langmd.mix` → `audiomd.mix` (`audio.idx` 82272 bytes,
  `audio.bag` 37625632 bytes).
- Antares (`src/Misc/Audio.cpp`, hook 0x4011C0) loads `audio`, `ares`,
  `audio01`..`audio99`, then loose `<sample>.wav` files as a fallback. It strips
  `$`/`#` from sample names and allows 32 samples per sound.

## The baker

See the module docstring of `tools/distance_bake.py` and the tag reference.

### Decisions, each from a measurement

- **Air absorption is ISO 9613-1.** It matches the ISO 9613-2 Table 2 values
  to their printed precision for six temperature/humidity rows, which a test
  checks. Measured pure-tone loss through the actual FIR is within 0.2 dB of
  the formula at 250 Hz – 4 kHz, which a test also checks.
- **Air over the extra path, ground geometry over the full path.** The
  vanilla samples already are the on-screen sound, so a band should only lose
  what the additional d × CellMeters metres cost. The first version applied
  air over the whole distance, which double-counted the on-screen part. The
  ground reflection, by contrast, is absent from the close-recorded originals,
  so its geometry is at the full distance.
- **PCM16 output by default.** IMA ADPCM's ~20 dB-SNR white noise refilled
  band 3's top octave on vanilla `gexp14a` from −56 dB to −20 dB, which
  undoes the muffling.
- **Level is what air alone leaves of each sample.** The comb and the tail
  were first normalised against white noise. An explosion's energy is mostly
  bass, where the comb adds in phase (+2.55 dB at g = 0.5) and where the
  air-absorbed tail concentrates, so vanilla `gexp14`'s far bands came out up
  to 2.9× too loud, and scaling the whole variant back under full scale then
  cost 1.5–9.3 dB: a level step at every band edge. Now both are normalised
  against the sample itself, and peaks are handled by a 1.5 ms look-ahead
  limiter. Measured on `gexp14a`–`d` and `snukexpl`: within 0.16 dB of
  air-only, with the limiter cutting at most 1.9 dB, around single peaks.
- **Spreading is not baked.** The DLL applies it continuously
  (`Distance.Falloff=inverse` is added for baked sounds that set no falloff).
  Baking it too would count it twice, and a baked level could not follow the
  exact distance.
- **Renders are reused.** A sound that lists a sample twice, or two sounds
  sharing samples under the same physics, get one render. On vanilla
  explosions that cut 48 renders to 27.
- **Reruns are idempotent.** Every generated line carries `;cebake`. A rerun
  strips them first, and the tests check that baking its own output
  reproduces the same soundmd.ini and bag byte for byte.

## Status

- Host-tested:
  - the distance model (60 checks)
  - the baker (23 tests: physics, level, limiter, codec, names, end-to-end,
    idempotency, foreign-bag refusal)
- Hook seats: verified statically, as listed above.
- Verified in game (2026-10-25): the DLL loads and reads every sound (818 in the
  loaded mod). A 32-minute game exited cleanly with all five hooks installed.
- Not yet verified in game: the distance behaviour itself (bands, falloff,
  scatter, diffuse, delay, echo). It needs a soundmd.ini that uses the tags.
