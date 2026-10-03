# Findings: mechanics of individual magic effects

Source: OpenMW `mwmechanics/spelleffects.cpp`, `activespells.cpp`, `spellcasting.cpp`, `spellresistance.cpp`,
`magiceffects.cpp`, `summoning.cpp`, `actors.cpp`. Full page: [../magic-effects.md](../magic-effects.md). Written from
OpenMW only; our `source/magic.cpp` was not read.

## Rules

### Same spell does not stack; different spells do
`activespells.cpp addToSpells`. Entries are keyed by (spell or item id, caster, item). A repeat cast of the same spell
ends the old copy and starts the new one (one magnitude, fresh timer). Potions and ingredients are stackable. Two
different spells add; two effects of one spell add. Vanilla bug stance: none, this is the rule.

### Magnitude and resistance are rolled once
`spelleffects.cpp applyMagicEffect`, `applyProtections`. Magnitude is `min + uniform(max - min)` at the first frame.
Reflect, Absorption and the resistance multiplier are applied at the first frame only; a per-second effect keeps its
scaling to the end. Resistance multiplies min and max (a no-magnitude effect is all or nothing); Weakness may push the
multiplier above 1. Self-cast effects are resisted too (only Reflect / Absorption need caster != target). The part of the
roll that does not come from Resist / Weakness is below 1 percent of the magnitude.

### Per-second effects deliver magnitude x duration in total
`applyMagicEffect`. For effects that are neither no-magnitude nor applied-once, with a duration: each frame applies
`magnitude x min(time left, dt)`; duration 0 applies the whole magnitude once. Fire / Frost / Shock, Poison, Damage and
Restore of Health / Magicka / Fatigue / Attribute / Skill, Absorb Health / Magicka / Fatigue, Sun Damage, Disintegrate.

### Drain and Fortify are reversible one-shots
`applyActorMagicEffect`, `removeMagicEffect`. Drain Health / Magicka / Fatigue lower the *current* value, may go below 0,
leave the maximum alone, and give the amount back at the end. Fortify raises current (past the old maximum) and, via the
totals, the maximum; at the end current falls by the amount and may go below 0: damage taken while fortified is not
forgiven. Drain / Fortify with duration 0 are undone at the next update.

### Attribute and skill values
Value = base + modifier - damage, floored at 0. Drain Attribute / Skill add to the damage with no cap, undone at the end.
Damage Attribute / Skill (per second) cap each tick at the current value and are **never** undone by expiry. Restore only
removes damage: it never raises a stat above its base. Fortify adds to the modifier, uncapped. Skills exist on NPCs only
(Drain Skill is dropped on a creature).

### Absorb
Absorb Health / Magicka / Fatigue (per second): the target loses (clamped at 0), the caster gains the **full** amount even
if the target had less (clamped at the caster's maximum). Absorb Attribute / Skill: the target is drained, the caster
fortified by the same amount, both undone at the end; a self-cast Absorb nets 0. Absorb is caster-linked: dropped without an
actor caster. Resisted by Resist Magicka.

### Spell Absorption and Reflect
`applyProtections`: only for temporary (spell-type) sources, target != caster. Per applied Reflect / Absorption entry on the
target, roll 0..99 below its magnitude. Reflect re-casts at the original caster (resisted by the caster); Absorption removes
the effect and gives the target magicka equal to the spell's cost (enchantment cast cost for an item), not clamped to the
maximum. Unreflectable effects skip Reflect.

### Cure, Remove Curse, Dispel
`applyActorMagicEffect`. Cure Common / Blight Disease and Remove Curse purge spells of that type from the target's list and
active spells. Cure Poison / Paralyzation purge the *applied* Poison / Paralyze effects. Cure Corprus purges Corprus effects
only (the disease spell stays). Dispel: per active temporary *spell* (not potion, enchantment, ability, power) a roll below the
magnitude removes the whole spell and runs each effect's undo.

### Fortify Maximum Magicka
`recalculateMagicka` (creaturestats.cpp, not on this list; from general OpenMW knowledge, verify). Max magicka =
`int((multiplier + 0.1 x Fortify Max Magicka) x Intelligence)`, multiplier fPCbaseMagickaMult (1.0) for the player, fNPCbaseMagickaMult
(2.0) for NPCs; current scales with the maximum. Stunted Magicka stops the rest-time magicka restoring (actors.cpp).

### Modifier-only effects
Shield, elemental shields, Burden, Feather, Jump, Levitate, Slowfall, Swift Swim, Water Breathing, Chameleon, Invisibility,
Light, Sanctuary, Night Eye, Sound, Blind, Silence, Paralyze, Detect *, Telekinesis, Resists, Weaknesses, Reflect, Absorption,
Fortify Attack: they add to the actor's per-effect total and nothing else in these files; other systems read the total. Totals sum over
all active copies. Levitate is removed (message) when levitation is disabled; Water Walking is refused where the target cannot stand on
water.

### AI effects
Calm / Frenzy / Demoralize / Rally (humanoid and creature variants), Turn Undead: add (or subtract) the magnitude to the Fight or Flee
setting and undo it. A humanoid effect on a creature, or the reverse, is invalid (dropped). Never on the player. Calm > 0 also stops combat
now. Turn Undead: only on undead creatures. Command: applies if the magnitude >= the target's level, never on the player, wrong kind invalid.

### Summons and bound items
`summoning.cpp`, `spelleffects.cpp`. The summoned creature (id from a game setting) is placed within 120 units of the summoner, follows and
helps it; removed when the effect ends; when it dies the summoning effect is purged so another can be cast. Bound items are added and
equipped; if equipping fails the effect ends at once; at the end the item is removed and the previous item re-equipped (player) or
autoequip (NPC). Recasting while active does nothing.

### Soul Trap, Disintegrate, Lock, Open, travel
Soul Trap: on a creature's death with soul > 0 and Soul Trap > 0, the caster (an actor) fills the smallest empty gem that fits (value x
fSoulgemMult). Disintegrate Armor: shield, cuirass, pauldrons, gauntlets, helmet, greaves, boots, the first with condition; Weapon: right hand;
per second with the fraction kept. Lock: raises the lock level to the magnitude if lower. Open: unlocks if lock <= magnitude (a crime attempt by the
player), else the fail sound. Mark / Recall / Divine / Almsivi: player only, once, refused when teleporting is disabled.

### Corprus, Vampirism, death
Corprus: worsens on a clock (days), re-applying the spell's other applied-once effects; cure removes the Corprus effect and the worsenings are
undone once each. Vampirism total survives death; everything else in the active list is cleared when the death animation ends. A health-damage effect
that kills credits its caster (player or the player's followers).

## Tests written

- `tools/specgen_effects.py` (new, imports `openmw_specgen`) writes `tools/tests/openmw-spec-effects.txt` (about 70 cases, `--count N` scales, seed
  `--seed`). Each case reloads a saved battlemage, sets Willpower / Luck 5 (so the random part of the resist roll is minimal) and Intelligence 100, makes a
  one- or two-effect spell with `ADDEFFECT` / `MAKESPELL`, casts it (`CAST`, or `CASTAT:scamp`), and `EXPECT`s mostly relative to a `SNAP`:
  - drain / fortify attribute and skill (floor at 0, return after expiry);
  - damage then restore attribute / skill (stays, never above base), damage per second (total magnitude x duration);
  - two spells stacking on one stat, independent expiry, same spell not stacking; drain and fortify netting;
  - health: damage / poison per second, Drain Health (current only, returns), Fortify Health (max and current, damage not forgiven), damage then restore,
    restore per second capped by the loss; magicka: damage, restore, drain, fortify, Fortify Maximum Magicka;
  - resist / weakness by element and Resist Magicka against fire / frost / shock / poison / damage / drain, with Resist, Weakness and the matching shield
    set by `ACTIVE`; a scamp's own resists against the same;
  - Absorb Attribute / Skill (caster side) and Absorb Health / Magicka (target side) on a scamp;
  - Cure Poison, Dispel (magnitude 100) against a fortify and a poison;
  - modifier-only totals (Shield as `armor`, Burden, Feather, Chameleon, Sanctuary, Resists, Weaknesses...), two spells adding and expiring separately;
  - wrong-kind AI effects (Frenzy / Demoralize Humanoid) on a creature do nothing.
- The cost of a cast is taken from `openmw_specgen.spell_cost_made` (magicka cases only, one point of slack).

## Hooks needed in the engine

Everything below is what the spec describes but a test cannot see today. None is implemented.

| Name | Args | Returns |
|---|---|---|
| `EXPECT:refattr:<ref>,<attr>` | actor id, attribute name | the actor's modified attribute (to check Absorb / Drain / Damage on the target side, and NPC Fortify) |
| `EXPECT:refskill:<ref>,<skill>` | actor id, skill name | the NPC's modified skill |
| `EXPECT:refhealthmax:<ref>` | actor id | the actor's maximum health (Drain Health on an actor must not change it) |
| `EXPECT:refmagickamax:<ref>` | actor id | the actor's maximum magicka |
| `FATIGUEREGEN:0` | 0 or 1 | turns fatigue regeneration off for a test (it hides per-second Fatigue effects; needed for Drain / Damage / Restore / Absorb Fatigue, which `openmw-spec-effects` therefore skips) |
| `EXPECT:capacity` | none | the player's carrying capacity (Burden and Feather: strength x fEncumbranceStrMult + Feather - Burden, floor 0) |
| `EXPECT:effectarg:<id>,<attr-or-skill>` | effect id, arg | the per-key total (`effect` sums over the argument) |
| `EXPECT:hitchance` / `EXPECT:detectchance` | none | Sanctuary, Chameleon, Blind and Invisibility readers |
| `EXPECT:levitating`, `EXPECT:fallmult`, `EXPECT:swimspeed`, `EXPECT:jumpheight`, `EXPECT:waterwalking` | none | movement readers of Levitate, Slowfall, Swift Swim, Jump, Water Walking |
| `EXPECT:itemhealth:<id>` | item id (worn) | condition of a worn item (Disintegrate Armor order and Weapon) |
| `EXPECT:locklevel:<ref>` | ref id | a lock's level (Lock, Open) |
| `EXPECT:marked`, `EXPECT:cell` after Recall | | the marked cell / position (Mark, Recall, interventions) |
| `EXPECT:summoned:<effect id>` | effect id | number of live creatures summoned by the player under that effect; `refpkg` kind for Follow |
| `EXPECT:refcommanded:<ref>` | ref id | 1 if the actor has a commanded follow package (Command with magnitude >= level) |
| `EXPECT:refflee/refcombat` as numbers | ref id | the Fight / Flee setting modifier (Frenzy, Demoralize, Rally, Calm, Turn Undead amounts) |
| `EXPECT:refdisp` after Charm | ref id | exists; need the effect-derived disposition term |
| `EXPECT:spellmagicka:<spell id>` | spell id | the cost taken from the player's magicka by a cast (so magicka tests need no oracle cost) |
| `EXPECT:activespells` | none | count of active spell entries (Dispel, recast, stackable potions) |
| `EXPECT:diseases` | none | count of disease / blight / curse spells known (Cure Common / Blight, Remove Curse) |
| `EXPECT:corprus` | none | whether the Corprus effect is active (Cure Corprus must leave the disease on the list) |
| `GIVEPOTION:<id>` / `USE:<id>` | item id | to test stackable sources (potions stack per drink; spells do not) |
| `ROLL:` seeding for the magnitude and resist rolls | | to test min / max ranges and the exact resist term without margin |

## Open questions

- Which effects carry OpenMW's "applied once" flag? The ESM bit 0x1000 is not set on any effect in `out/world/game.json`
  (flags there stop at 0x7d8; Negative Light 0x800 is missing too), so either the converter masks high bits or OpenMW assigns the flag
  in code (a loader file outside the list). The spec derives the set from the apply / undo pairing in `spelleffects.cpp`; spec/magic.md's
  cost rule ("duration at least 1 unless applied once") is dead code if the flag never arrives in our data. Check which.
- Where is the duration of an effect clamped (to at least 1 for effects that are not applied-once)? It is in the Lua-side `applyMagicEffects`
  (`mwlua`), outside the list.
- Fortify Health / Magicka / Fatigue raising the *maximum*: the handler only raises the current value; the maximum comes from the totals read in
  `creaturestats.cpp` / `actors.cpp`. The tests assume the long-standing behaviour (maximum and current both +magnitude).
- Readers of the modifier-only totals (Burden, Feather, Sanctuary, Chameleon, Shield, Jump, Levitate, Slowfall, Swift Swim, Blind, Sound) are in
  `npc.cpp`, `character controller`, `combat`: their formulas need their own spec pass.
- magic.md says Drain Health on an *actor* lowers both current and maximum here, and that Cure Corprus removes the disease: OpenMW does neither
  (Drain touches only the current value; Cure Corprus ends the Corprus effect only). The player side and the actor side may differ in our engine.
- magic.md notes that actors' attributes and skills do not change under Drain / Fortify / Damage in our engine: OpenMW applies them to every actor
  (and drops skill effects on creatures). Needs the `refattr` / `refskill` hooks.
- Summon creature ids from the game settings for Wolf, Bear, Bonewolf, Creature04, Creature05: absent from our data (134 effects).

## Mismatches

Hooks added to `source/testdrive.cpp` (names as in the table above unless noted): `refattr`, `refskill`, `refhealthmax`, `refmagickamax`,
`refcommanded`, `effectarg`, `locklevel` (same as `reflock`), `summoned`, `itemhealth`, `spellmagicka` (the cost the last cast took),
`capacity`, `hitchance`, `detectchance` (Chameleon + 100 if invisible), `levitating`, `fallmult`, `swimspeed`, `jumpheight`, `waterwalking`,
`marked`, `corprus`, `activespells` (distinct sources), `diseases` (types 2..4), verbs `FATIGUEREGEN:0|1`, `GIVEPOTION:<id>[:n]`, `USE:<id>`,
and `SEED:<n>` (srand; named SEED, not ROLL: ROLL already sets the persuasion die). Not added: `refflee` / `refcombat` as the numeric Fight / Flee
modifier (the engine only has a fleeing flag), `refdisp` after Charm (exists), `EXPECT:cell` after Recall (exists).

| Rule | Our behaviour | Cause | Status | File |
|---|---|---|---|---|
| Applied-once flag 0x1000 on effects | never set, so the cost rule "duration at least 1 unless applied once" was dead | OpenMW sets it in code (`HardcodedFlags` in loadmgef.cpp), not in the ESM | fixed (needs data rebuild) | tools/build_game.py `APPLIED_ONCE` |
| Drain Health / Fatigue on actors lowers current only | lowered current and maximum | wrong rule | fixed | source/magic.cpp |
| Drain Magicka on actors given back at the end, may go below 0 | clamped at 0, never given back | missing undo | fixed | source/magic.cpp |
| Restore Magicka on actors capped at the maximum | uncapped | missing cap | fixed | source/magic.cpp |
| Drain / Fortify / Absorb Attribute and Skill on actors (skills not on creatures); Absorb fortifies the caster | none (actors' numbers never changed) | not modelled | fixed (kept as timed effects, read by hook; combat still uses record numbers) | source/magic.cpp, include/world.h |
| Fortify Health / Fatigue / Magicka on actors | not handled | missing | fixed | source/magic.cpp |
| Drain Magicka / Fatigue on the player may go below 0 | clamped at 0 | extra clamp | fixed | source/magic.cpp |
| Same spell does not stack (recast ends the old copy) | each cast added a copy | no source key check | fixed for the player (actors' spells carry no source) | source/magic.cpp `releaseSpell` |
| Dispel / Cure Poison / Paralyzation run each effect's undo | only Fortify undone (a Dispelled Drain stayed) | partial undo | fixed (`endPlayerEffect`) | source/magic.cpp |
| Reflect / Spell Absorption on any effect of another's spell | harmful effects only | extra condition | fixed | source/magic.cpp |
| Absorb needs an actor caster | self / potion Absorb drained the player alone | no caster check | fixed (dropped) | source/magic.cpp |
| Cure Corprus ends the Corprus effect only | removed the disease from the list | wrong rule | fixed (`corprusSince` = -2 means cured, saved with it) | source/magic.cpp, world.cpp, session.cpp |
| Unreflectable effects skip Reflect | not known | flag not exported | open | tools/build_game.py |
| Spells on actors keyed by spell (no stacking, Dispel of actors, Cure on actors) | effects on actors carry no source; Dispel / Cure do nothing there | memory (no per-effect source) | open | source/magic.cpp |
| Damage / Restore Attribute / Skill on actors; Fortify Maximum Magicka on actors | not kept | would need per-actor damage store | open | source/magic.cpp |
| Magnitude is an integer roll min + d(max-min+1) | player rolls a float | small | differs on purpose (under 1 point) | source/magic.cpp |
| Drain / Fortify with duration 0 undone at the next update | actors use 1 second | simplicity | differs on purpose | source/magic.cpp |
| Fortify Attack, Frenzy / Demoralize / Rally / Calm amounts as Fight / Flee numbers | flags and timers (r.fleeing, calmUntil) | AI is not number based here | open (AI area) | source/magic.cpp |
| Summon / bound-item rules (placement, purge when the summon dies, equip failure) | not compared | out of scope this pass | open | source/magic.cpp |

Next emulator batch: `openmw-spec-effects` (needs a data rebuild first for the applied-once flag: made-spell cost of Drain / Fortify with duration 0
changes), plus a quick re-run of `openmw-spec-magic` and `openmw-spec-actormagic` (cost and actor effects). The Fatigue cases need `FATIGUEREGEN:0`.
