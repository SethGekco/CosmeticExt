# CosmeticExt — TODO

Newest request first. Each item keeps Rex's words verbatim, followed by notes
for whoever builds it. The notes are leads and open questions. They decide
nothing that Rex hasn't.

---

## 1. Remap-colour and palette overrides (`ColorRules=`) — not started

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

**Open decisions for Rex** (do not guess these):
1. **Cycle scope.** Does `Color_Cycle01` advance once for the whole game, per
   house, or per source (each GAWEAP, each superweapon firing)?
2. **`Repeat=` meanings.** When the list runs out, what does each of these do:
   `Beginning` and `End` (the list's first and last entries?), `First` and
   `Last` (the first and last colours actually produced, which differ under
   `Random`?), and `Center`?
3. **What counts as a conflict.** A remap rule and a `PaletteSwitch` rule
   could both apply, as two separate channels. Or does one rule replace the
   other whole?
4. **Precedence when several sources apply.** Rex gave superweapon over the
   type, factory over the type, and `FreeUnit` over the type. What about a
   unit that gets two of these at once?
5. **`Invoker` for things nobody invoked.** What colour do those use, for
   example a map-placed civilian or a unit the AI's own superweapon delivers?
   Owner?
6. **Remap under a `PaletteSwitch` palette.** Do remap pixels still take the
   rule's colour, or the palette's own colours?

---

## 2. One sound short with the listening test installed — waiting on a game

The band pick, the delay and the echo were confirmed in game on 2026-10-26
(CHANGELOG). The engine counted 820 sounds against 821 distinct
`[SoundList]` names, though, and the offline checks found no cause. The
deployed DLL now logs `[SoundList] <key>=<name> did not become a sound` at
load. Read that line after the next game, then fix the cause (in the baker if
it is ours). Only then widen the test beyond Explosion14.
