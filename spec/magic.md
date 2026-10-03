# Magic

Mirrors the OpenMW wiki page *Research:Magic*. Spell cost, cast chance and school below; then resistance, Reflect and
Spell Absorption, how each effect applies, and actors as targets. In our words.

Reference: OpenMW `apps/openmw/mwmechanics/spellutil.cpp` (`calcEffectCost`, `calcSpellBaseSuccessChance`,
`getSpellSuccessChance`). GMSTs: `fEffectCostMult` (0.5), `fFatigueBase` (1.25), `fFatigueMult` (0.5).

## Magicka cost of a spell being made (spellmaking)

Per effect, with `base` the magic effect's base cost:

- magnitude: 1 when the effect has no magnitude, else `max(1, min)` and `max(1, max)`;
- duration: 1 when the effect has no duration; otherwise the duration, at least 1 unless the effect is
  *applied once* (flag 0x1000);
- `x = 0.5 * (magMin + magMax) * 0.1 * base * (1 + duration) + 0.05 * max(1, area) * base`;
- `x *= fEffectCostMult`; a target-range effect costs 1.5 times that.

The spell's cost is the sum over its effects, rounded (`max(1, ...)`).

An existing spell record with the autocalc flag uses the same formula without the `1 +` duration offset and
without the minimum area; one without the flag keeps the cost stored in the record.

## Chance to cast

For each effect: `x = duration (at least 1 unless applied once) * 0.1 * base * 0.5 * (magMin + magMax) +
area * 0.05 * base`, times 1.5 for a target effect, times `fEffectCostMult`. Take `s = 2 * skill(school)` for
that effect's school, and the effect that gives the lowest `s - x`: its `s` is `lowestSkill`, and its school is
the spell's school (the skill a cast trains).

    chance = (lowestSkill - spellCost + 0.2 * Willpower + 0.1 * Luck - Sound) * fatigueTerm
    fatigueTerm = fFatigueBase - fFatigueMult * (1 - fatigue / maxFatigue)      (1 when max is 0)

clamped to 0..100. Powers always succeed (once a day); a spell with the "always succeeds" flag is 100.
Silence makes it 0. With less magicka than the cost the chance shown is 0.

## Findings (2026-09-28, all fixed, tested by `openmw-spec-magic`)

1. **Area and zero duration in the made-spell cost.** We ignored the area and let a duration of 0 count as 0
   (OpenMW: at least 1 unless applied once). `Session::effectCost`.
2. **School by cost, not by skill.** We took the school of the most expensive effect. OpenMW takes the effect
   that is weakest against the player's skill; this decides the chance and which skill a cast trains.
   `Session::spellSchool`.
3. **Sound applied after the fatigue term.** OpenMW subtracts Sound before multiplying. `Session::castChance`.

## Resistance (OpenMW: `spellresistance.cpp`, `MagicEffect::getResistanceEffect` / `getWeaknessEffect`)

An incoming harmful effect is resisted by: fire, frost and shock damage by Resist Fire / Frost / Shock (weakened by
Weakness to the same; the matching elemental shield counts as resistance too); poison by Resist / Weakness to Poison;
paralysis by Resist Paralysis; vampirism by Resist / Weakness to Common Disease; corprus by Resist / Weakness to Corprus;
and drain, damage (health, magicka, fatigue, attribute, skill), absorb, the Weakness effects, burden, charm, silence,
blind, sound, calm, frenzy, demoralize, rally and turn undead by **Resist / Weakness to Magicka**. Anything else can't
be resisted.

    resistance = resist effects - weakness effects (+ the shield for fire / frost / shock)
    x = (Willpower + 0.1 Luck) x fatigue term x 50 / castChance
        castChance: the caster's (uncapped) chance for the spell; 100 for potions, enchantments, traps
    roll = 0..100;  effects with no magnitude: roll -= resistance
    if x <= roll: x = 0   else x = 100 (no magnitude)  or  roll / min(x, 100)
    resisted percent = min(x + resistance, 100);   magnitude x (1 - that / 100); fully resisted: "sMagicPCResisted"

The target's numbers are its own: the player's, or an actor's (its record's Willpower, Luck, fatigue, and the effects
its abilities and the spells on it carry).

## Reflect and Spell Absorption (OpenMW: `spelleffects.cpp applyProtections`, `absorbSpell`)

Only for an effect someone else cast (not traps, not the caster's own). Each Reflect and each Spell Absorption on the
target rolls 0..99 below its magnitude, per effect of the spell:

- **Reflect** sends the effect back to the caster (not reflected again; the caster's resistance applies). Not for
  effects flagged unreflectable.
- **Spell Absorption** drops the effect and gives the target magicka worth the spell's cost (the enchantment's cast
  cost for an item).

## How effects apply (OpenMW: `spelleffects.cpp applyMagicEffect`)

- **Damage** health / magicka / fatigue, fire / frost / shock, poison, **restore** health / magicka / fatigue, **absorb**
  health / magicka / fatigue: magnitude per second for the duration; all at once when there is no duration. Absorb gives
  the caster what it takes.
- **Drain** health / magicka / fatigue: lowers the current value by the magnitude (it may go below 0) and gives it back
  when it ends. Drain / Fortify attribute and skill: a modifier while it lasts. Damage attribute / skill: lasting damage
  until restored. Absorb attribute / skill: the target is damaged, the caster fortified.
- **Fortify** health / magicka / fatigue: raise the maximum and current while it lasts. Fortify Maximum Magicka: 0.1 x
  magnitude added to the magicka multiplier.
- **Cure** Common / Blight disease and **Remove Curse** purge those spell types; Cure Corprus ends the Corprus effect only (the disease stays on the list); Cure
  Poison / Paralyzation end those effects.
- **Dispel:** each active *spell* (not potions, enchantments or abilities) goes whole with magnitude percent chance, one
  roll per spell.
- **Open:** unlocks when the lock level is at most the magnitude, else "Open Lock Fail"; on someone else's lock it counts
  as an unlock attempt (a crime where seen). **Lock:** raises the lock to the magnitude unless it is locked higher.
- **Sun Damage:** outdoors, magnitude x clamp(max(sun visibility, fMagicSunBlockedMult) x sun height, 0, 1) a second.
- **Disintegrate Armor:** the first worn piece in the order shield, cuirass, pauldrons, gauntlets, helmet, greaves,
  boots loses the magnitude in condition; **Disintegrate Weapon:** the wielded weapon.
- Calm, Frenzy, Demoralize, Rally, Command, Charm, Paralyze, Silence, Sound, Blind, Chameleon, Invisibility,
  Sanctuary, Shield, the elemental shields: read while they last (combat, casting, detection).

## Findings

2026-09-28 (spell cost, chance, school): see above.

2026-09-29, first pass:
1. Resist Magicka was added to every harmful effect (fire, frost, shock, poison, paralysis included).
2. Effects with no magnitude weren't resisted except at 100 percent; they use the roll.
3. Reflect only turned the effect aside and Absorption gave nothing; both fired on traps and the player's own spells.
   Now Reflect lands on the caster and Absorption pays the spell's cost, only for someone else's spell.

2026-09-29, this pass:
4. **Actors had no resistances at all**: an atronach's Resist Fire, a Dunmer's 75 % fire resistance, an Altmer's
   weaknesses, and whatever Weakness or Resist a spell put on them. The converter now writes each actor's constant
   effects from its own and its race's abilities, diseases and curses (`tools/convert/build_game.py constant_effects`,
   `const_effects`), each actor keeps the spells on it (`Ref::effects`), and `applyEffectToActor` rolls Reflect,
   Absorption and resistance with the actor's own Willpower, Luck and fatigue (`resistRoll`, shared with the player).
5. **Damage on actors landed all at once** (magnitude x duration); now per second, as for the player. Drain Health on
   an actor was a plain hit; now, as in OpenMW, it lowers the current value only (the maximum stays) and gives it back.
6. **The resistance roll ignored the caster's cast chance** (always 100); now `x 50 / castChance` for spells (NPC
   spells at the player too).
7. **Actors' Shield, Sanctuary, Chameleon, Invisibility, Blind, Fortify Attack, Silence, Sound and elemental shields**
   did nothing; they now count in combat, casting, and shields burn the player's blows.
8. **Every enchanted weapon counted as ordinary** (a misplaced statement in `game.cpp` read "magic" only for lights),
   so ghosts resisted enchanted blades, and the magic icon background was missing on enchanted weapons and armor.
9. **Dispel** removed effects one by one from any source; OpenMW removes whole spells, one roll each, and leaves
   potions and enchantments. **Remove Curse** did nothing; it removes curses.
10. **Open** made no sound on failure and was no crime on an owned lock; **Lock** gave no feedback.

Tests: `openmw-spec-magic` (cost, chance), `openmw-spec-resist` (resistance terms), `openmw-spec-actormagic`
(hand-written: a Dunmer's Resist Fire 75 and an Altmer's weaknesses read from `const_effects`; the NPC ids
`ranis_athrys` and `estirdalin` and their races are from memory, check them). EXPECT kind `refeffect:<id>,<effect>`.

## Open

- Actors' attributes and skills under Drain / Fortify / Absorb are kept (`Ref::TimedEffect::key`, read by `EXPECT:refattr` / `refskill`), but combat still uses the record's numbers; Damage / Restore Attribute / Skill on actors is not kept.
- The spells on actors are not saved; a reload clears them.
- The cast chance passed to the resistance roll is capped at 0..100 here (OpenMW's is uncapped).
- Cure Corprus now ends the Corprus effect only, as OpenMW does: `World::corprusSince` = -2, the disease stays on the list and its other effects go on.
- Summoning and bound items: summons follow and fight for the player for the duration; bound items replace what is
  worn and go when the effect ends. Their details (fSummonDist, a summon despawning when the caster dies) are unchecked.
