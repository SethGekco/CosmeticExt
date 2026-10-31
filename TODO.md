# CosmeticExt — TODO

Newest request first. Each item keeps Rex's words verbatim, followed by notes
for whoever builds it. The notes are leads and open questions. They decide
nothing that Rex hasn't.

---

## 1. Remap-colour and palette overrides (`ColorRules=`) — not started, two decisions open

Requested 2026-10-30. Verbatim:

> The ability to override the recolorable parts of graphics with another.
> There's a special remap color that is typically the player's chosen color. I
> want the ability to override it via TechnoType entry. I would like a tag that
> also overrides it via UnitDelivery or Paradrop Superweapon types. Make sure
> especially, but not exclusively, it works for civilian/neutrals.
> Create a complicated setting that looks like the following:

```ini
[Color_Cycle01]
RemapColors=DarkRed,LightBlue,Yellow ;Entries under [Colors], accept new ones being added.
RemapColors.Pattern=Increment ;Decrement, Random
RemapColors.Repeat=Beginning;End,Random,Center,First,Last (First and Last is most useful for "Random" Pattern for it might not be the same as Beginning and End)

[Color_RemapOwner]
RemapColors=Owner;default

[Color_RemapInvoker]
RemapColors=Invoker;If a unit is spawned by a specific player, that player's remaps are used instead of who the unit belongs to.

[AMCV]
ColorRules=Color_Cycle01;from now on, it uses a color cycle

[AmericanParadropSpecial]
ColorRules=Color_Cycle01;While E1 doesn't have the tag, E1 instantiating from this superweapon do. If they did have a color rule, this overrides it if they conflict

[GAWEAP]
Factory.ColorRules=Color_Cycle01;while units might not have any color rules, or maybe they have a different one, this enforces a color rule on them and may override their existing one if they have one when built from this factory if they conflict

[GAREFN]
FreeUnit=CMIN
FreeUnit.ColorRules=Color_Cycle01;CMIN doesn't have any color rules, but this applies it simply for coming from this GAREFN's instantiation. 

[ZombieSpawn]
....
Deliver.Owner=civilian
Deliver.Types=ZOMBIE01
ColorRules=Color_RemapInvoker
;This superweapon spawns uncontrollable zombies that have a player's color for activating the superweapon. So while they have the player's color, they don't register as losses to their non-owner and they have no devoted loyalty to their non-owner in spite sharing their colors. 
```

```ini
~~~~~~~~~~~~~

[Color_PaletteDesert]
PaletteSwitch=desert01;desert01.pal is used instead of whatever the unit or building usually uses. Instead of a .PAL with a lot of blues and grays, it's a bunch of tans and browns.

;Artmd.ini
[ABRAMS]
Palette=abrams ;Ares allows users to define custom palette; this unit uses a palette called abrams.pal

;Rulesmd.ini

[AmericanParadropSpecial]
ParaDrop.Types=ABRAMS
ColorRules=Color_PaletteDesert;ABRAMS dropped by this superweapon appear like Desert Abrams instead of normal metallic variants.
```

### Notes for the builder (2026-10-30, from a quick source search only)

**Why it belongs here.** It changes colours and palettes and nothing else.
In the zombie example the owner stays `civilian`, so losses, loyalty and AI
are untouched. Keep it that way: the rule must never change `Owner`.

**Engine and prior art**
- A house's remap is `HouseClass::ColorSchemeIndex` → `ColorScheme::Array` →
  that scheme's `LightConvert`.
- Antares already re-colours one drawable this way: an anim's `LightConvert`
  is swapped for its new owner's scheme in `Antares-src/src/Ext/AnimType/Body.cpp:40`.
  That is the closest model for a per-object override.
- Neither Phobos nor Antares has a per-TechnoType remap or palette override;
  a grep found nothing.
- The instantiation paths to cover:
  - Ares UnitDelivery, which reads `Deliver.Types` and `Deliver.Owner` in
    `Antares-src/src/Misc/SWTypes/UnitDelivery.cpp:29-31`;
  - ParaDrop (`ParaDrop.Types` is read in `Ext/Side` and `Ext/HouseType`);
  - factory exit, for `Factory.ColorRules`;
  - the vanilla `FreeUnit=`, for `FreeUnit.ColorRules`;
  - the type's own tag.
- A quick grep didn't find where Antares reads the artmd `Palette=` that
  `[ABRAMS]` uses. Find it before writing `PaletteSwitch`.

**Online games.** No randomizer for `Pattern=Random` or `Repeat=Random`:
take the colour from a deterministic hash, such as the object's unique ID
plus the cycle step. Then every player in an online game sees the same
colours, and the synced randomizer is never touched.

**State.** The rule an object was created under, and its cycle position, is
per-object data. It must be kept in an extension map and survive save/load.

**Rex's answers (2026-10-30).** Each is quoted verbatim, then what the
builder takes from it. Where the second part is a reading of the words, it
says so; confirm a reading with Rex if building on it would be costly.

1. **Cycle scope.** "Color cycle how I described it is up to the user, but by
   default it should be continuous rather than limited."
   - The scope is an INI setting on the cycle section (a name such as
     `RemapColors.Scope=Game|House|Source` is only a suggestion).
   - Reading: by default there is one sequence for the whole game. It keeps
     going and never resets per player or per factory.
2. **`Repeat=`.** "Where in the cycle does it restart."
   - `Repeat=` names the entry the cycle restarts from after its last colour:
     `Beginning` and `End` are the list's first and last entries, `Center` the
     middle one, and `Random` a random entry at each restart. `First` and
     `Last` are the first and last colours the cycle actually produced, which
     differ from `Beginning` and `End` under `Pattern=Random` (Rex's note in
     the spec).
   - Builder's choices, to document: `Center` of an even count rounds down.
     `Increment` with `Repeat=End` keeps the last colour for good, and
     `Decrement` with `Repeat=Beginning` keeps the first. Both follow from the
     definition and are not errors.
   - Reading: under `Pattern=Random` the list only "runs out" if Random is a
     shuffle, each colour once per pass. Treat it as a shuffle.
3. **Palette and remap together.** "I want to say yes but we can make this
   toggle-able."
   - By default a remap rule and a `PaletteSwitch` rule are two channels and
     both apply, even when they come from different sources. A toggle makes a
     rule replace whatever the object had, in full.
   - So "if they conflict" in the spec means two rules on the same channel.
4. **Precedence.** "It should be impossible from how I'm seeing it. Can you
   give an example of when there's overlaps?" See *Overlaps* below. Open
   until Rex answers.
5. **`Invoker` with no invoker.** "Neutral/Civilian."
   - Objects nobody invoked, such as map-placed ones or ones a trigger
     creates, use the neutral (civilian) house's colour, the colour a
     map-placed civilian shows. Confirm which house that is in the loaded
     mod. An AI that fires a superweapon is its invoker.
6. **Remap on a switched palette.** "It should try."
   - Remap pixels on a `PaletteSwitch` palette still take the rule's colour
     when that palette can be remapped. When it can't, show the palette's own
     colours, log it once per palette, and never fail.

**Overlaps (sent to Rex 2026-10-30, for decision 4).** Rex is right for a
single creation: an object comes out of one factory, one superweapon or one
`FreeUnit=`, plus its own type's tag, and he has ranked each of those over the
type. Two rules reach one object only along a chain, when an object that
already has a rule makes, copies or turns into another:
- *A superweapon delivers a refinery.* `Deliver.Types=GAREFN` with
  `ColorRules=Color_RemapInvoker` gives the refinery the invoker's colour. It
  still hands out its free CMIN (`BuildingClass::Place`; Ares grants it once,
  `Ext/SWType/Hooks.Legacy.cpp:889`), and that CMIN gets GAREFN's
  `FreeUnit.ColorRules=Color_Cycle01`. It also came, one step back, from the
  superweapon. Cycle or invoker?
- *Cloning.* Infantry built at a barracks with `Factory.ColorRules` comes out
  yellow. A cloning facility copies it (`BuildingExt::KickOutClones`,
  `Ext/Building/Body.cpp:1194`), and the copy leaves the vats, not the
  barracks. Same yellow, the next colour in the cycle, or its own type's rule?
- *Deploying.* An AMCV with `ColorRules=Color_Cycle01` comes out yellow. When
  it deploys, the engine removes the unit and creates a new construction-yard
  object. Does the yard keep yellow or use its own type's rule? When it packs
  up again, does the new MCV get yellow back or the next colour? Crew leaving
  a destroyed vehicle and a carrier's aircraft raise the same question.

Recommendation given to Rex: an object that turns into another, or is copied
from one, keeps that colour. An object made by another uses its maker's tag,
then its own type's rule, and nothing is inherited from further back.

**Still open for Rex** (do not guess these):
- Decision 4, the overlap rule above.
- **Owner changes.** Does a rule stay on an object whose owner changes (an
  engineer capture, mind control, the Psychic Dominator)? Example: a zombie
  showing its invoker's colour is mind-controlled by a third player. An
  `Owner` rule follows the new owner by definition. Cycle, `Invoker` and
  `PaletteSwitch` rules might stay or drop.

---

## 2. One sound short with the listening test installed — waiting on a game

The band pick, the delay and the echo were confirmed in game on 2026-10-26
(CHANGELOG). The engine counted 820 sounds against 821 distinct
`[SoundList]` names, though, and the offline checks found no cause. The
deployed DLL now logs `[SoundList] <key>=<name> did not become a sound` at
load. Read that line after the next game, then fix the cause (in the baker if
it is ours). Only then widen the test beyond Explosion14.
