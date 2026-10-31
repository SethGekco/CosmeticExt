# CosmeticExt rulesmd.ini tags: `ColorRules=`

These tags change an object's **remap colour** (the player-colour pixels) and
its **palette**. They change nothing else. The object's owner stays the same,
so its losses, loyalty, AI and score all stay with the owner. A civilian
zombie can wear the colour of the player who summoned it and still be a
civilian.

Every client in an online game sees the same colours. The look is decided in
simulation order, and every "random" choice comes from a hash. The game's
synced randomizer is never used.

Everything goes in **rulesmd.ini**, or in a map, which can override it.
Nothing changes until some section names a rule.

## Test snippet

Add the `[Color_*]` sections at the end of rulesmd.ini. Add each other line
to the section that already exists (`[CIV1]`, `[AMCV]` and so on). Don't
start a second copy of those sections. All of them exist in the loaded mod,
and so do the colours.

```ini
; --- new sections ---------------------------------------------------------
[Color_Cycle01]
RemapColors=DarkRed,LightBlue,Yellow

[Color_Factory]
RemapColors=Yellow

[Color_DropTest]
RemapColors=Invoker
PaletteSwitch=iso        ; the terrain palette (isotem.pal and so on): easy to spot

; --- lines for existing sections -----------------------------------------
[CIV1]
ColorRules=Color_Cycle01 ; map civilians: dark red, light blue, yellow, dark red...

[AMCV]
ColorRules=Color_Cycle01 ; each player's MCVs cycle, each player starting at dark red

[GAWEAP]
Factory.ColorRules=Color_Factory ; everything from an Allied war factory is yellow

[GAREFN]
FreeUnit.ColorRules=Color_Cycle01 ; the free Chrono Miner cycles too

[AmericanParaDropSpecial]
ColorRules=Color_DropTest ; dropped troops: the caller's colour, terrain palette
```

What to look for:

- Map civilians (`CIV1`) in three colours.
- MCVs, war-factory vehicles and the miners from new refineries change colour.
- Paradropped troops look odd (terrain palette) but the plane does not.

`debug/debug.log` shows what happened. Look for these lines:

```text
[CosmeticExt] ColorRules: GetRemapColour wrapped in 4 of 4 techno vtables.
[CosmeticExt] ColorRules: rules pass 1: 3 rule sections, 4 types and 1 superweapons name one
[CosmeticExt] ColorRules: [Color_Cycle01] RemapColors=DarkRed,LightBlue,Yellow Pattern=0 Repeat=0 Scope=0 ...
[CosmeticExt] ColorRules: CIV1 #1000123 (type) -> colour DarkRed, palette own
[CosmeticExt] ColorRules: HTNK #1000456 (factory) -> colour Yellow, palette own
```

The first 40 decisions of each game are logged. Saving and loading log
`saved ...` and `restored ...` lines.

## Colour rule sections

A rule is a section of any name. The `Color_` prefix is a convention only.

| Key | Values | Default |
|---|---|---|
| `RemapColors=` | A comma list. Each entry is a name under `[Colors]` (new ones work), `Owner` or `Invoker`. One entry is a fixed colour. Several make a cycle. | none: the owner's colour |
| `RemapColors.Pattern=` | `Increment`, `Decrement`, `Random` | `Increment` |
| `RemapColors.Repeat=` | `Beginning`, `End`, `Random`, `Center` (or `Centre`), `First`, `Last` | `Beginning` |
| `RemapColors.Scope=` | `House` (or `Player`), `Game`, `Source` | `House` |
| `PaletteSwitch=` | A palette name, or `none` | none: the object's own palette |
| `Replace=` | `yes`, `no` | `no` |

- **`Owner`** is the owner's colour. It follows the owner: if a unit changes
  hands, it changes colour.
- **`Invoker`** is the colour of the player who spawned the object. The
  invoker is whoever fired the superweapon, owns the factory, or owns the
  building that handed out the free unit. An AI firing a superweapon is an
  invoker too. The colour is fixed at birth and stays if the unit changes
  hands. An object nobody spawned, such as one placed by the map or made by
  a trigger, takes the Neutral house's colour, as a map-placed civilian
  does. If there is no Neutral house, it takes the Civilian side's colour,
  then Special's. The first such use is logged with the house it chose.
  The loaded mod has `[Neutral] Color=Grey`.
- **`PaletteSwitch=name`** loads `<name><theater>.pal`, as artmd.ini's
  `Palette=` does. So `PaletteSwitch=desert01` needs `desert01tem.pal`,
  `desert01sno.pal` and so on, one per theater you play. A trailing `.pal`
  is ignored. Remap colours still apply on the switched palette (Rex: "It
  should try"). When the palette has no remap for the colour, the owner's is
  used. When the file is missing, the object keeps its own palette. Both are
  logged once and never fail.
- **`Replace=yes`** makes the rule replace the whole look. By default a
  remap rule and a palette rule are two separate channels, and both apply
  even when they come from different places. A `Replace=yes` rule decides
  both channels. Where it sets nothing, it puts back the vanilla look. Rules
  ranked below it are not consulted. See *Which rule wins* below.

## Where rules attach

| Tag | Goes on | Applies to |
|---|---|---|
| `ColorRules=` | a TechnoType (unit, infantry, aircraft, building) | that type, however it is created |
| `Factory.ColorRules=` | a factory building | what it kicks out: vehicles, infantry, aircraft |
| `FreeUnit.ColorRules=` | a building with `FreeUnit=` | its free units |
| `ColorRules=` | a SuperWeaponType | what it creates: paradropped troops, Ares `UnitDelivery` objects (`Deliver.Types=`), anything else it spawns |
| `FreeUnit.ColorRules.Inherit=yes` | a building with `FreeUnit=` | free units copy the building's look |
| `Cloning.ColorRules.Inherit=yes` | a cloning facility | clones copy the original's look |
| `Deploy.ColorRules.Inherit=yes` | a type that deploys or packs up | the new object keeps the old one's look. On `AMCV` it covers MCV → yard, on `GACNST` yard → MCV |

- A value of `none` clears a tag set by an earlier file. Rules are read
  from rulesmd.ini, then the game-mode INI, then the map. A file only
  overrides the keys it sets.
- A tag naming a section that exists nowhere is ignored, and logged once.

## When the look is decided

An object gets its look once, the first time it enters the map. A factory
product and a clone get theirs as they leave the building. Leaving a
transport, a garrison or the paradrop plane later changes nothing.

What can reach the object depends on how it was born:

| Born as | Rules consulted, highest first |
|---|---|
| a factory product | the factory's `Factory.ColorRules`, then its own type's `ColorRules` |
| a free unit | the building's look if it has `FreeUnit.ColorRules.Inherit=yes`, else its `FreeUnit.ColorRules`, then its own type's |
| a superweapon's creation | the superweapon's `ColorRules`, then its own type's |
| a clone | the original's look if the facility has `Cloning.ColorRules.Inherit=yes`, then its own type's |
| a deployed or packed-up object | the old object's look if the old type has `Deploy.ColorRules.Inherit=yes`, then its own type's |
| anything else (map, trigger, crew, ...) | its own type's |

Only the direct source counts. A rule never passes along a chain unless an
`Inherit` tag says so (Rex, 2026-10-30):

- A superweapon delivers a refinery. The refinery gets the superweapon's
  rule. Its free miner gets the refinery's `FreeUnit.ColorRules` and never
  the superweapon's.
- A clone never gets the barracks' `Factory.ColorRules`.
- A deployed yard uses its own type's rule. An MCV that packs up again
  starts afresh, so under a cycle it takes the next colour.

### Which rule wins

The two channels, remap colour and palette, are decided separately. For each
channel, the highest rule that sets it wins. For example, a superweapon with
only `PaletteSwitch=` and a type with only `RemapColors=` both apply. A
`Replace=yes` rule settles both channels and stops the search. An inherited
look (from an `Inherit` tag) settles both channels.

### Owner changes

The look stays (Rex: "I would say by default, yes"). A cycle colour, an
`Invoker` colour and a `PaletteSwitch` stay. An `Owner` entry follows the
new owner, because that is what it means. A mind-controlled zombie keeps its
invoker's colour.

### Disguises

When the local player cannot see through a disguise, the object shows the
vanilla colour, which is the disguise's. A rule never gives a disguised spy
away.

## Cycles

`RemapColors=DarkRed,LightBlue,Yellow` hands out one colour per object.
`Pattern=` sets the order. `Repeat=` sets where the cycle restarts after its
last colour. It never stops.

| Pattern + Repeat | Colours handed out |
|---|---|
| `Increment` + `Beginning` | DarkRed, LightBlue, Yellow, DarkRed, LightBlue, ... |
| `Increment` + `End` | DarkRed, LightBlue, Yellow, Yellow, Yellow, ... |
| `Increment` + `Center` | DarkRed, LightBlue, Yellow, LightBlue, Yellow, LightBlue, ... |
| `Decrement` + `End` | Yellow, LightBlue, DarkRed, Yellow, LightBlue, ... |
| `Decrement` + `Beginning` | Yellow, LightBlue, DarkRed, DarkRed, DarkRed, ... |
| `Random` + any | a shuffle: each colour once per pass, a new order every pass |

- **`Beginning`** and **`End`** restart at the list's first and last entry.
  **`Center`** restarts at the middle one. **`Random`** restarts at a random
  entry each time.
- **`First`** and **`Last`** restart at the first and last colour the cycle
  actually produced. Under `Increment` and `Decrement` that is the same as
  `Beginning` and `End`, or `End` and `Beginning`. Under `Random` they
  differ, which is what they are for.
- A restart runs on from the restart entry. `Increment` goes to the end of
  the list, `Decrement` to the start. `Random` puts the restart entry first
  and shuffles the rest.

`Scope=` decides who shares a place in the cycle:

- **`House`**, the default. Each player has their own place, and each
  starts at the first colour (Rex, 2026-10-30). The player is the invoker
  when there is one, else the owner.
- **`Game`**. One place for the whole game: everyone's objects share one
  sequence.
- **`Source`**. Each factory, building, cloning facility and superweapon
  has its own place. A superweapon counts once per player. Objects with no
  source, such as map-placed ones, share one place.

## Readings and builder's choices

Rex answered every question about this feature on 2026-10-30. His words are
in TODO.md. These points are interpretations or choices made while building
it, not his words. Tell me if any should change.

**Readings of his answers:**

- "The first colour" is the first one the pattern yields: the list's first
  entry under `Increment`, its last under `Decrement`, and the shuffle's
  first under `Random`.
- `Pattern=Random` is a shuffle, each colour once per pass. Otherwise the
  list never "runs out" and `Repeat=` would mean nothing.
- Crew leaving a destroyed vehicle and a carrier's spawned aircraft follow
  the default: their own type's rule. They got no `Inherit` tag.
- An AI that fires a superweapon is its invoker.

**Builder's choices:**

- Tag names: `RemapColors.Scope=`, `Replace=` and the three `Inherit`
  tags. Rex's names were suggestions.
- `Center` of an even count rounds down: entry 2 of 4.
- `Increment` with `Repeat=End` keeps the last colour for good, and
  `Decrement` with `Repeat=Beginning` keeps the first. Both follow from the
  definition.
- "Random" is a hash of the rule's name, the cycle's place and the pass
  number. It is the same on every client and in every replay of the same
  game.
- A paradrop plane is a carrier, not part of the drop. It takes its own
  type's rule, with the firing player as its invoker. Its passengers take
  the superweapon's rule when they land. Anything else a superweapon spawns
  during its launch, a spy plane for example, takes the superweapon's rule.
- An Ares `UnitDelivery` places its objects `SW.Deferment=` frames after the
  launch (20 by default). Objects of the `Deliver.Types=` types that appear
  between 2 frames before and 8 frames after that get the superweapon's
  rule, each listed type once.
- An `Inherit` tag copies the parent's look even when the parent had no
  rule, so the new object shows the vanilla look.
- `Factory.ColorRules=` covers what a factory building kicks out. A
  building placed from a construction yard does not leave a factory
  building, so `Factory.ColorRules` on a yard does nothing.

## Limits

- An Ares `UnitDelivery` superweapon with `PreClick=` or `PostClick=` is not
  matched, because Ares keeps such weapons "ready" after firing.
- An object of a delivered type made by something else in the same few
  frames could take the delivery's look. That would need the same type, the
  same moment and no other source, so it should be rare.
- Saves made before this feature load normally, and their objects keep the
  vanilla look. The look is stored at the very end of the save, so
  `CosmeticExt.dll` must stay **last** in the `-i=` list, as it is now.
