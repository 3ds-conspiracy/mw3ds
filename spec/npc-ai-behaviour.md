# NPC AI Behaviour

Mirrors the OpenMW wiki page *Research:NPC AI Behaviour*. Rules read from OpenMW's `mwmechanics/actors.cpp`
(`engageCombat`, `updateGreetingState`, `playIdleDialogue`, the crime-on-sight check), `mwmechanics/combat.cpp` (`getFightTerm`,
`isAggressive`, `isAggressionCapable`), `aicombat.cpp` / `aicombataction.cpp` (`vanillaRateFlee`,
`makeFleeDecision`), `aiwander.cpp`, `aifollow.cpp`, `aiescort.cpp`, `mechanicsmanagerimp.cpp` (`awarenessCheck`).
In our words.

## Perception

The awareness check (see `combat.md`): an observer notices the player when its roll (0..99, rerolled every 5 seconds)
is at least the player's sneak term minus the observer's. Not sneaking, only Chameleon or Invisibility can keep the
player unnoticed. Starting a fight, greeting and witnessing a crime also need line of sight.

## Attack on sight

    fight term = Fight + int(iFightDistanceBase - fFightDistanceMultiplier x distance + (50 - disposition) x fFightDispMult)

(creatures count disposition 50; distance ignores height for actors that fly or swim). The actor attacks when the
term reaches 100, it isn't calmed (Calm Humanoid / Creature active), and it sees and notices the player. Immobile
creatures are never aggressive. Guards also attack creatures that are fighting, within `fAlarmRadius`, when the
creature's own fight term against the player is 100.

## Fleeing

    Flee >= 100: always
    rating = (1 - health share) x fAIFleeHealthMult + Flee x fAIFleeFleeMult,
             plus iFightDistanceBase - fFightDistanceMultiplier x distance when the rating isn't 0

An actor flees when the rating reaches 100 and beats its best attack's rating; the choice is made again every 3 s
(`ActionFlee`'s cooldown). Demoralize raises Flee; Rally lowers it.

How it runs (`AiCombat::updateFleeing`): it stands until the foe, in sight, is within the foe's own attack reach
(`getMaxAttackDistance` of the foe, less both half extents; any distance when that reach is 1000 or more: a bow, a
throwing weapon, a target spell in hand). Then it picks a path grid point at random among those the point nearest it
connects to in that cell, runs there, and stands again once there, or once it is `fFleeDistance` away out of sight. With
no path grid (or a fish) it runs straight away from the foe for 1 s, then stands again.

## Choosing the next action (healing)

`prepareNextAction` rates each option and takes the best, about once per blow: potions (`ratePotion`), on-use enchanted
items, the weapons carried (`rateWeapon`), and the spells known (`rateSpell`). A potion is drunk at once when chosen,
then 0.75 s to the next choice.

    rateWeapon = (mean damage (melee: (slash^2 + thrust^2 + chop^2) / (slash + thrust + chop); bow: chop; thrown: chop x 2)
                 x condition x (fDamageStrengthBase + Strength x fDamageStrengthMult / 10) x (1 - foe's Resist Normal
                 Weapons, for a plain weapon) + a bow's arrows' own rating (rateAmmo; none: 0) + an on-strike
                 enchantment's effects' rating) x hit chance (0.01 .. 1) x speed
                 x fAIMeleeWeaponMult (fAIRangeMeleeWeaponMult: a ranged one out of the foe's reach); a ranged one
                 rates 0 under water. Test `bug-archer-arrow-rating`
    restore effect on self (rateEffect): p = 4 health, 2 fatigue, magicka 2 when a known spell costs more than is left
                 (but no more than the maximum), else 0
                 heal = mean magnitude x duration; damage = maximum - current; if damage < heal and more than half is
                 left: over = heal - damage, heal = damage
                 rating = (p - 1) / 2 x (damage / maximum + 0.6)^(2p) + p x (heal - 2 over) / maximum - 0.5
    each effect x its cost (0.5 x (min + max) x base cost / 10 x duration + area x base cost / 20, x fEffectCostMult)
                 x fAIMagicSpellMult (fAIRangeMagicSpellMult on target); a spell's sum x its cast chance / 100

    every other effect (rateEffect): 1, made negative when harmful (then x 1 - the foe's resistance / 100) and negative
                 again when cast on someone else, x its cost as above. 0 for what the AI can't use (Soul Trap,
                 interventions, Calm / Frenzy / Demoralize / Rally, Charm, detections, Telekinesis, Mark, Recall, Jump,
                 the water effects, Slow Fall, Light, Lock, Open, Turn Undead, the disease effects, Invisibility,
                 Chameleon, Night Eye, Vampirism, Stunted Magicka, Remove Curse, Command), for the resistances, Spell
                 Absorption and Reflect, the fortifies, Levitate and Restore Attribute / Skill. Blind only at a foe with
                 a weapon out who can use it, Sound / Silence at one with a spell ready, Burden only to overburden (x 3),
                 Drain / Damage / Absorb Attribute x the attribute's weight (Strength 1, Intelligence 0.5, Willpower 0.6,
                 Agility 0.7, Speed 0.5, Endurance 0.8, Personality 0.7, Luck 0.3), Disintegrate only at armour / a
                 weapon, Cure Paralysis / Poison 1001 per such effect lasting over 3 s, Dispel by the effects it would
                 take off / 5, a summon 3 (0 while one of its own is about), target effects 0 under water, a
                 no-magnitude effect already on its target 0
    rateSpell = 0 for a power, short of magicka, or still on its target; else the effects' sum x chance / 100

Here: every option is weighed so against the weapon: potions (rated with no foe) about once a blow, and spells (summons,
shields, heals, harmful ones) when a cast comes up, every 3 - 6 s: the best spell above the weapon's rating is cast. A
touch spell waits for the foe within reach; a target one is skipped a third of the time (cast pacing, ours). Left out:
on-use enchanted items, NPCs' bound weapons and armour (rated 0: not done for NPCs), the weapon's condition in
rateWeapon (actors' weapons don't wear here), "spell still on its target" by caster (ours: by the spell's name on the player, by
its effects on the caster). Test `bug-npc-ward` (Ajira summons and never casts Spell Absorption: fails before, she put
it on twice).

## Greetings and idle chatter

- An actor wandering, travelling or with no package, not in combat, swimming or paralyzed, greets once when the player
  is within `AI Hello x iGreetDistanceMultiplier`, in sight, and noticed (after two 0.25 s updates); it turns to face
  the player for `iGreetDuration`. It greets again only after the player has gone `fGreetDistanceReset` away.
- Idle voice lines: while not in combat or following, within 3000 units and in sight, a chance of `fVoiceIdleOdds` per
  1/10000 per frame-at-60-fps.
- Attack voice: `iVoiceAttackOdds` percent per attack; hit voice `iVoiceHitOdds`.

## Packages

- **Wander:** idles, and now and then (a roll of 92+ out of 100, when not walking) walks to a random point
  `(0.2 .. 1) x distance` from where it started, on the path grid when there is one. Idle animations by the
  package's idle chances and `fIdleChanceMultiplier`.
- **Travel:** walks to the point; done within a short distance.
- **Follow:** keeps within `64 + the leader's and its own size` (more with other followers); must first be in range
  and in sight to start.
- **Escort:** leads to the point while the escorted one is within `half extent + 450`; past that it stops and waits
  until they are within `half extent + 250`.
- **Activate:** walks to the object and activates it.
- Walking speed for packages is the walk speed; chasing, fleeing and catching up run (`movement.md`).
- **Getting unstuck** (`obstacle.cpp`): an actor that has stood in the same spot for a while (1.5 s) evades for a
  second, each time the next way round, as (right, forward): right and forward, right, right and back, back, left and
  back, left, left and forward. OpenMW finds its way round with a navigation mesh; we have the path grid only, and the
  wilds have none. So `npcMoveTo` (`mwmechanics/aipackage.cpp`), stuck for half a second, first looks for an opening: the way nearest
  the goal, turned up to about 155 degrees, along which a body fits for 200 units (rays at its sides, knee and chest
  high), keeping to the side it took last; stuck again in the same place, it starts farther round and keeps to the
  way longer. Only with no opening does it use OpenMW's turn. `issue-20` (Drerel Indaren following the
  player down from the rock pillars west of Ald'ruhn) tests it.
- **Footsteps (follow, outdoors):** the player's position is noted every 120 units walked (48 kept, cleared by a
  jump over 700 units). A follower with no path grid route that has lost sight of the player for a second, or made no
  headway for a second, walks those footsteps for up to 10 s: the one nearest it first, then on in order, looking
  ahead to the next two it can see once it has reached one. Held up outdoors this way it may step down a ledge of up
  to 250 units (OpenMW's actors fall off a ledge they walk to; ours otherwise refuse a step down over 48). No
  OpenMW source rule: a stand-in for its navigation mesh. `bug-follow-rock-walk` tests it.

## Findings (2026-09-29)

1. **Attack on sight** rounded differently (OpenMW adds the biases as a whole number), ignored Calm effects from spells
   and abilities, and didn't need line of sight.
2. **Fleeing** used our own health thresholds (`0.1 + Flee / 250` of health, 8-12 seconds); now OpenMW's rating with
   the distance bias, checked again every second or two.
3. **Greetings** repeated every 25-40 seconds within at most 440 units, whether or not the NPC could see the player or
   was busy with a package; now once until the player leaves `fGreetDistanceReset`, with sight, awareness, and only
   wandering / travelling / idle actors.
4. **Escorts** stopped and started at one distance (500), so they stuttered at the edge; now OpenMW's 450 / 250 wait.

## Findings (2026-10-13, side by side)

5. **Fleeing** ran straight away from the player all the time (into the walls indoors) until 3000 away, choosing again
   every 1-2 s. Now OpenMW's flee states: standing out of the player's reach, running to a path grid point, 3 s
   choices. Test `bug-flee-stands` (the scamp Creeper, Caldera: fails before).
6. **Healing:** NPCs never drank potions, and cast a healing spell under half their health whatever else they had. Now
   rated as `prepareNextAction` does (above). Tests `bug-npc-drinks-potion` (Arrille at a fifth of his health drinks a
   Quality Restore Health) and `bug-npc-keeps-potion` (at 95% he keeps it); written with the fix (their set-up steps
   REFADDITEM / REFHEALTH are new), the build before had no drinking at all.

Test: `openmw-spec-ai` (generated from the actor data): the fight term and the flee rating at several distances and
dispositions (`Session::fightTermOf`, `fleeRatingOf`). Needs `python tools/test/openmw_specgen.py` on `out/world`.

## Open

- The flee decision doesn't weigh the best attack's rating (OpenMW's anti-flee), so an actor with a strong weapon flees
  sooner here.
- Combat action choice is OpenMW's only for healing (above): which weapon is our own (the record's), harmful spells by
  range and a timer, no fortify potions or enchanted items.
- A fleeing actor's trigger counts the player's weapon only (our player has no spell stance).
- Wander idle animations don't use the package's idle chances (the converter doesn't write them).
- Guards don't attack fighting creatures on their own.
- Followers don't use OpenMW's follow distance by size; they keep 180 units.
