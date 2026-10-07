# NPC AI Behaviour

Mirrors the OpenMW wiki page *Research:NPC AI Behaviour*. Rules read from OpenMW's `mwmechanics/actors.cpp`
(`engageCombat`, `updateGreetingState`, `playIdleDialogue`, the crime-on-sight check), `combat.cpp` (`getFightTerm`,
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

An actor flees when the rating reaches 100 and beats its best attack's rating; the choice is made again about every
second. A fleeing actor runs until it is `fFleeDistance` away out of sight. Demoralize raises Flee; Rally lowers it.

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
  wilds have none. So `npcMoveTo` (`combat.cpp`), stuck for half a second, first looks for an opening: the way nearest
  the goal, turned up to about 155 degrees, along which a body fits for 200 units (rays at its sides, knee and chest
  high), keeping to the side it took last; stuck again in the same place, it starts farther round and keeps to the
  way longer. Only with no opening does it use OpenMW's turn. `issue-20` (Drerel Indaren following the
  player down from the rock pillars west of Ald'ruhn) tests it.

## Findings (2026-09-29)

1. **Attack on sight** rounded differently (OpenMW adds the biases as a whole number), ignored Calm effects from spells
   and abilities, and didn't need line of sight.
2. **Fleeing** used our own health thresholds (`0.1 + Flee / 250` of health, 8-12 seconds); now OpenMW's rating with
   the distance bias, checked again every second or two.
3. **Greetings** repeated every 25-40 seconds within at most 440 units, whether or not the NPC could see the player or
   was busy with a package; now once until the player leaves `fGreetDistanceReset`, with sight, awareness, and only
   wandering / travelling / idle actors.
4. **Escorts** stopped and started at one distance (500), so they stuttered at the edge; now OpenMW's 450 / 250 wait.

Test: `openmw-spec-ai` (generated from the actor data): the fight term and the flee rating at several distances and
dispositions (`Session::fightTermOf`, `fleeRatingOf`). Needs `python tools/test/openmw_specgen.py` on `out/world`.

## Open

- The flee decision doesn't weigh the best attack's rating (OpenMW's anti-flee), so an actor with a strong weapon flees
  sooner here.
- Combat action choice (which weapon or spell, `rateWeapon` / `rateSpell`) is our own: melee or ranged by the record's
  weapon, spells by a timer.
- Wander idle animations don't use the package's idle chances (the converter doesn't write them).
- Guards don't attack fighting creatures on their own.
- Followers don't use OpenMW's follow distance by size; they keep 180 units.
