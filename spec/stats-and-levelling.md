# Stats and Levelling

Mirrors the OpenMW wiki page *Research:Stats and Levelling*. Rules read from OpenMW's `npcstats.cpp` (`levelUp`,
`updateHealth`, `getSkillProgressRequirement`, `getLevelupAttributeMultiplier`), `creaturestats.cpp`
(`getFatigueTerm`, `recalculateMagicka`), `actors.cpp` (`restoreDynamicStats`, `calculateRestoration`,
`getRestorationPerHourOfSleep`), `mwclass/npc.cpp` (`getCapacity`, character creation), and the Lua
`omw/skillhandlers.lua` (skill use types). In our words.

## Character creation

- Attributes: the race's (by sex), +10 for each of the class's two favoured attributes.
- Skills: 5; +25 major, +10 minor; +5 in the class's specialization; the race's skill bonuses on top.
- Race and birthsign abilities apply as constant effects (Fortify / Drain attribute and skill, resistances ...).

## Health, magicka, fatigue

- **Base health** is fixed at creation: `floor(0.5 x (Strength + Endurance))` of the creation attributes. It grows
  only by level-up gains. Later raises, Fortify and Drain do not move it. Max health = base + Fortify Health.
- **Magicka:** `(fPCbaseMagickaMult [NPCs: fNPCbaseMagickaMult] + 0.1 x Fortify Maximum Magicka) x Intelligence`
  (modified). Race and birthsign multipliers are Fortify Maximum Magicka abilities. When the maximum changes the
  current value keeps its share of the maximum.
- **Fatigue:** Strength + Willpower + Agility + Endurance (modified).
- **Fatigue term:** `fFatigueBase - fFatigueMult x (1 - current / max)` (1.25 to 0.75; 1 when the max is 0). It
  scales nearly every skill roll.

## Level-up

- Level progress: `iLevelUpMajorMult` / `iLevelUpMinorMult` per major / minor increase; `iLevelUpTotal` (10) of it
  allows a level-up (excess carries over).
- Attribute increases counted for the multiplier: `iLevelUpMajorMultAttribute`, `iLevelUpMinorMultAttribute`,
  `iLevelUpMiscMultAttriubte` (sic) per increase of a skill governed by it.
- The attribute raise: `iLevelUpNNMult` with NN the count, clamped 0 to 10 (0: +1).
- **Health gain:** `fLevelUpHealthEndMult (0.1) x base Endurance`, including the raise just chosen; added to base
  and current health.

## Skill progress

- One increase needs `(1 + base skill) x type factor x specialization factor`: type factor `fMajorSkillBonus`
  (0.75), `fMinorSkillBonus` (1), `fMiscSkillBonus` (1.25); specialization factor `fSpecialSkillBonus` (0.8) when the
  skill is in the class's specialization. **Base skill**: Fortify / Drain / Damage do not change the requirement.
- Each use adds the skill record's gain for that use type (table in `spec/combat.md`), times a scale where given
  (athletics per second).
- No increase past base 100.

## Restoration

- **Resting (per hour of sleep):** health `0.1 x Endurance`, magicka `fRestMagicMult x Intelligence` (none while
  Stunted Magicka lasts). Waiting restores neither.
- **Fatigue per hour (resting or waiting):** `3600 x (fFatigueReturnBase + fFatigueReturnMult x (1 - load)) x
  fEndFatigueMult x Endurance`, load = encumbrance / capacity (at most 1). Nothing when fatigue is at or above its
  base.
- **Fatigue per second (awake, every actor):** `fFatigueReturnBase + fFatigueReturnMult x Endurance`, nothing when at
  or above the maximum.
- Health and magicka do not regenerate over time (only by rest, spells, potions).

## Encumbrance

`capacity = fEncumbranceStrMult x Strength (modified)`; `encumbrance = carried weight + Burden - Feather` (not below
0). Over capacity the player cannot move; under it movement slows by `fEncumberedMoveEffect x load`.

## Findings (2026-09-29)

1. **Max health followed the current attributes** (level-up raises counted twice, Fortify and Drain moved it).
   Fixed: `World::recomputeStats` (`PlayerStats::attrCreation`), `Session::applyLevelUp`.
2. **Skill need used the modified skill**, so a Fortify Skill made the skill slower to train and a Drain faster;
   and a fortified skill at 100 could not rise. Fixed: `Session::skillNeed`, `raiseSkill` use `Session::baseSkill`
   (`PlayerStats::skillCreation` + gains).
3. **Resting fatigue** ignored the load; **awake fatigue** used hardcoded numbers for the player and a flat 3 a second
   for NPCs. Fixed (`Session::restHour`, `combatUpdate`, the NPC loop); nothing is restored above the maximum.

Test: `openmw-spec-stats` (hand-written): base health unaffected by attribute changes; level-up gain; skill need
unaffected by Fortify Skill; an hour of sleep gives `fRestMagicMult x Intelligence` magicka.

## Open

- Old saves keep `healthBonus`; the double-counted half raises of earlier level-ups are dropped on load.
- Werewolf stats (Bloodmoon) out of scope.
