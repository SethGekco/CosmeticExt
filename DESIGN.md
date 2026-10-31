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
[docs/soundmd-tags.md](docs/soundmd-tags.md). The second is per-object remap
colours and palettes (`ColorRules=`), documented in
[docs/colorrules.md](docs/colorrules.md) and described at the end of this
file.

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

Two `Type` bits change the metric. Both were read from the disassembly on
2026-10-25 and are not tested in game.

- **`0x800`** (YRpp `Shroud`), tested at `0x750B2D`. The source's cell must be
  revealed (`MapClass::GetCellAt` 0x5657A0, cell flags `+0x12C & 0x18`). If it
  isn't, the function returns 0.0 before any distance is computed, so the
  sound is silent and the volume hook never runs.
- **`0x40`** (`Local`), tested at `0x750BFA`. The view's half-size is **not**
  subtracted. `[esp+0x38]` and `[esp+0x20]` then hold |dx| and 2|dy| from the
  **screen centre**, unclamped, and `Range` is measured from there.

`Screen` (`0x20`) is not tested here or in PlayAt.

**`Local` sounds follow the engine (fixed 2026-10-30).** The volume and pan
hooks read the engine's own locals, so they always agreed with vanilla. The
band pick, the delay and the echo threshold, however, used a distance the DLL
computed itself, and that was always measured beyond the edge. So a
`Type=LOCAL` sound would pick its band at a different distance from the one
it fades by. Vanilla has 31 LOCAL sounds, all ambients and loops.

Now `EngineDistance` passes the base sound's LOCAL bit to
`DistanceModel::ViewDistance`, which skips the half-size subtraction exactly
as the engine does. The host tests cover both metrics. The base's bit is used
because the decision is made for the base. `ReadINI` logs a warning when a
band's LOCAL bit differs from its base's, because then the pick and the fade
cannot agree.

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

## Remap colours and palettes (`ColorRules=`)

The spec, Rex's answers and the readings are in TODO.md #1. The user-facing
reference is [docs/colorrules.md](docs/colorrules.md).

### The engine's remap path (gamemd 1.001)

| Address | What |
|---|---|
| `0x705D70` | `TechnoClass::GetRemapColour`, virtual slot `0x1E4`. If the type has an art `Palette=` (`TechnoTypeClass+0xDF0`, a scheme vector), it returns `Palette->Items[Owner->ColorSchemeIndex]->LightConvert` and ignores disguises. Otherwise, when the local player can't see through a disguise (`IsClearlyVisibleTo`, slot `0x440`), it uses the disguise house's scheme (`GetDisguiseHouse(true)`, slot `0xD0`). Otherwise `ColorScheme::Array[Owner->ColorSchemeIndex]->LightConvert`. |
| `0x7F5E54`, `0x7EB23C`, `0x7E40A0`, `0x7E2488` | The slot in the Unit, Infantry, Building and Aircraft vtables (`AbsVTable + 0x1E4`). All four hold `0x705D70`. No deployed DLL references these addresses. The abstract classes' slots (`0x7E8E78`, `0x7F4B44`) are never called on a live object, so they are left alone. |
| `0xB054D0` | `ColorScheme::Array`. Each `[Colors]` entry appears twice: once with `ShadeCount` 1 and once with 53. A house's `ColorSchemeIndex` (`HouseClass+0x16054`) points at the 53-shade one. The engine's name lookup (`0x474A90`) takes the first match whose `ShadeCount` is not 1, and so do we. |
| `0x6263D0` | `ColorScheme::GeneratePalette(char*)`, what art `Palette=` uses. Its loader (`0x627590`) builds `<name>` + the theater's extension + `.PAL`, upper-cased, so `desert01` loads `DESERT01TEM.PAL` in temperate. It returns one scheme per colour, cached by name. A missing file gives no vector. |
| `0x534DBE` | The theater has rebuilt its palettes, which frees the generated schemes. We drop our cached pointers here. |

### Design

- **The draw side is a vtable wrap, not a hook.** DllMain swaps slot
  `0x1E4` in the four concrete techno vtables for a wrapper. The wrapper
  calls the original first, so Phobos' hook inside the function
  (`0x705D74`) still runs. Then, only for an object with a stored look, it
  picks the scheme itself. No stolen bytes, and no chain to truncate. The
  first rules read logs how many slots still hold the wrapper.
- **The look is decided once, at a simulation event.** The events are the
  first `Unlimbo`, and the `KickOutUnit` entry for factory products and
  clones. They happen in the same order on every client, so a cycle hands
  out the same colours everywhere. Hashes never take object IDs, which differ
  between clients (`ScenarioClass::UniqueID` starts from a salt). A Source
  scope keys on the order in which sources first appear instead.
- **Per-object state is a map keyed by `AbstractClass::UniqueID`.** It keeps
  only objects a rule touched, plus a `Seen` set of decided objects so that
  leaving a transport does not decide again. Both are pruned against
  `TechnoClass::Array` when `Seen` outgrows the live objects. IDs are never
  reused within a game and survive save/load.
- **Creation contexts.** The context stack is valid only on the frame it was
  opened, and a stale entry is dropped. A superweapon context collects what
  its launch unlimbos and decides it when the launch returns. Only then can
  a carrier be told apart from its cargo: an object with passengers is a
  paradrop plane. Its passengers get a pending entry that is consumed when
  they land. A Grand_Opening context marks free units. A deploy or undeploy
  records the parent against the new object until that object unlimbos.
  Ares `UnitDelivery` places objects after a deferment, outside any call we
  can see, so it is matched by type in a frame window around the due frame.
  The launch only counts when `IsReady` went from true to false, because
  Antares clears it only when the weapon really fired (`SuperClass_ClickFire`,
  read in Antares-src; classic Ares not checked).
- **The savegame block goes at the very end of the save,** not at Phobos'
  early pair (`0x67D32C`/`0x67E826`). CosmeticExt is last in the inject
  list, so its block is the last thing in the file. A save made without
  the block then simply ends, the read comes back short, and nothing else is
  disturbed. At Phobos' pair the same read would eat the next block's bytes
  and break the load. The block starts with a magic, a version and a byte
  length, and the payload is parsed with bounds checks.
- **Reading past the end of a save is safe.** The save stream is the
  game's own block-compressing COM stream (CLSID
  `b48fa168-646f-11d2-9b74-00104b972fe8`, vtable `0x7E5DAC`). Its `Read`
  (`0x4A2B60`) refills from an 8-byte block header. When the header read
  comes back short, it returns `E_FAIL` (`0x80004005`) with `*pcbRead = 0`,
  and it throws nothing (`_com_issue_error` only fires on a null inner
  stream). So an old save gives a failed 4-byte magic read, which the load
  hook reports as "no ColorRules block". Read from disassembly, not yet
  seen in game.
- **Rules are read idempotently**, on every pass, at the `Read_File` tail.
  A key absent from a pass leaves the earlier value alone. A rule first
  named by a later file is looked up in rulesmd.ini too. Type tags are
  keyed by type ID, not pointer, with a fallback from CloningExt's `.clone`
  and `.construct` IDs to the base type.

### Hooks

All return 0. None of the stolen windows holds a relative branch or call.

| Address | Size | What it does | Shared with (deployed) |
|---|---|---|---|
| `0x668F6A` | 5 | `Read_File` tail, ESI = this pass's INI: reads the tags, logs the vtable wrap once. Stolen: `mov eax,[0xA8B238]`. | Phobos, BuildQueueExt, WeatherExt, Country_Extension4, YRAggressiveStance |
| `0x6F6CBE` | 5 | `TechnoClass::Unlimbo` after `ObjectClass::Unlimbo` succeeded, ESI = this: the birth point. Stolen: `mov eax,[edi]; push 1; cdq`. | none |
| `0x443C60` | 6 | `BuildingClass::KickOutUnit` entry, ECX = building, `[esp+4]` = object: factory products (`Factory->Object == object`) and clones. Stolen: `sub esp,0x130`. | CloningExt, GiftBoxHost (both return 0) |
| `0x4FAE85` | 5 | `HouseClass::Fire_SW` before `ClickFire`, EBX = house, EDI = super: opens the superweapon context and records `IsReady`. Stolen: `mov eax,[0xA83D4C]`. | none |
| `0x4FAEED` | 5 | `Fire_SW`'s only epilogue, EDI = super: closes it. Stolen: `pop edi; pop esi; pop ebp; mov al,1`. | none |
| `0x446AA9` | 6 | `Grand_Opening` (0x445F80, EBP = this throughout), at the `FreeUnit=` block: opens the free-unit context. Stolen: `mov ecx,[ebp+0x520]`. | none |
| `0x446FB6` | 7 | `Grand_Opening` epilogue, before EBP is popped: closes it. Stolen: `pop edi; pop esi; pop ebp; pop ebx; add esp,0x58`. | none |
| `0x7396D2` | 5 | Unit deploying (0x7393C0, EBP = unit), after the `BuildingClass` constructor, EBX = new building. Stolen: `mov eax,[ebx]; push edi; push 0x12`. | Phobos (returns 0) |
| `0x449E4F` | 6 | `Mi_Selling` undeploying (0x449C30, EBP = building), after the `UnitClass` constructor, EBX = new unit or 0. Stolen: `mov ecx,[0xA8E7AC]`. | none |
| `0x534DBE` | 5 | Theater palettes rebuilt: drop the palette cache. Stolen: `mov ecx,0xA8B238`. | none |
| `0x67E42E` | 5 | SaveGame, end of stream, ESI = IStream: appends the block. Stolen: `test eax,eax; pop edi; pop esi; pop ebp`. Kratos declares 13 here; Antares 5, as we do. | Antares, GiftBoxHost |
| `0x67F7C8` | 5 | LoadGame, end of stream, ESI = IStream: reads it. Stolen: `pop edi; pop esi; pop ebp; mov al,1`. | Antares, GiftBoxHost |
| `0x685659` | 10 | `ClearClasses`: resets the runtime state (IntelExt's seat). | Antares, Phobos, IntelExt, DoctrineExt, DossierExt, WarZoneExt |

Checks run on the seats (2026-10-30): disassembly of every stolen window and
the registers each handler reads; `check_hook_bounds --exe`,
`check_hook_overlap` and the encyclopedia's `check_hook_chain` are clean; a
`.syhks00` sweep of every DLL in Rex's game dir gives the sharing column.

## Status

- Host-tested:
  - the distance model (74 checks)
  - the baker (23 tests: physics, level, limiter, codec, names, end-to-end,
    idempotency, foreign-bag refusal)
- Hook seats: verified statically, as listed above.
- Verified in game (2026-10-25): the DLL loads and reads every sound (818 in the
  loaded mod). A 32-minute game exited cleanly with all five hooks installed.
- Verified in game (2026-10-26, Explosion14 listening test): the band pick,
  the travel delay and the echo threshold. Each logged decision matches the
  model. Volume, pan, scatter and the echo's playback are not logged, so they
  rest on listening.
- Open: with the test installed the engine counted 820 sounds against 821
  distinct `[SoundList]` names. `CheckSoundList` logs which name was lost
  (see CHANGELOG).
- `ColorRules=` (2026-10-30): the cycle and precedence rules are host-tested
  (348 checks), and the CI build is clean. The hook seats are verified
  statically, as listed above. **Not yet seen in game.** The proof will be
  Rex's debug.log showing the vtable-wrap line and the per-object decisions
  for the test snippet in docs/colorrules.md.
