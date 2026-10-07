# Movement

Mirrors the OpenMW wiki page *Research:Movement*. Rules read from OpenMW's `mwclass/npc.cpp` (`getMaxSpeed`,
`getWalkSpeed`, `getRunSpeed`, `getSwimSpeed`, `getJump`), `mwclass/creature.cpp`, `mwclass/actor.hpp`
(`getSwimSpeedImpl`, `getCurrentSpeed`), `mwworld/class.cpp` (`getNormalizedEncumbrance`) and
`components/misc/constants.hpp`. In our words.

## Speeds

- **Walk (NPC / player):** `fMinWalkSpeed + 0.01 x Speed x (fMaxWalkSpeed - fMinWalkSpeed)`, times `(1 -
  fEncumberedMoveEffect x load)` (load = encumbrance / capacity), not below 0. Sneaking: x `fSneakSpeedMultiplier`.
- **Run:** walk x `(0.01 x Athletics x fAthleticsRunBonus + fBaseRunMultiplier)`. Sneaking never runs.
- **Swim:** (run or walk) x `(1 + 0.01 x Swift Swim)` x `(fSwimRunBase + 0.01 x Athletics x fSwimRunAthleticsMult)`.
- **Fly (Levitate, or a flying creature):** `fMinFlySpeed + 0.01 x (Speed + Levitate) x (fMaxFlySpeed -
  fMinFlySpeed)`, times the encumbrance term.
- **Creatures:** walk `fMinWalkSpeedCreature + 0.01 x Speed x (fMaxWalkSpeedCreature - fMinWalkSpeedCreature)`; they run
  at their walking speed; swim as above from walk.
- **Over the limit** (load > 1): speed 0, levitating or not, and no jump.
- **Paralyzed, knocked down or dead:** 0.
- Strafing only (sideways): x 0.75.
- Storm wind: walking into an ash storm or blizzard is slower by `fStromWalkMult` x the angle to the wind.

## Jump

    a = min(Acrobatics, 50), b = max(0, Acrobatics - 50)
    x = fJumpAcrobaticsBase + (a / 15) ^ fJumpAcroMultiplier + 3 b x fJumpAcroMultiplier + 64 x Jump
    x x= fJumpEncumbranceBase + fJumpEncumbranceMultiplier x (1 - load)
    x x= fJumpRunMultiplier while running;  x x= fatigue term
    vertical speed = (x + gravity) / 3,   gravity = 8.96 m/s^2 x 69.99 units/m = 627 units/s^2

Moving, the take-off goes along the move direction at 0.707 of that, up and forward.

## Fatigue spent moving

Per second: running `fFatigueRunBase + load x fFatigueRunMult`, swimming (running / walking) `fFatigueSwimRunBase /
fFatigueSwimWalkBase + load x ...Mult`, sneaking `fFatigueSneakBase + load x fFatigueSneakMult`; each jump
`fFatigueJumpBase + load x fFatigueJumpMult`. Athletics trains per second running (use 0) or swimming (use 1).

## Footsteps

Each step plays by where the walker is (`Npc::getSoundIdFromSndGen`): flying, none; swimming, `Swim Left` / `Swim Right`;
feet under the water's surface, `FootWaterLeft` / `FootWaterRight`; else on the ground `FootBareLeft` / `FootBareRight`
(OpenMW picks light, medium or heavy boots by the boots' armor skill; the 3DS always plays the bare ones). The player
steps every 0.42 s walking and 0.65 s swimming (OpenMW takes them from the animation's sound keys). Test: `issue-27`.

## Findings (2026-09-29)

1. **Levitation speed** was the run speed x `(0.3 + Levitate / 50)`, at most 2x; OpenMW: `fMinFlySpeed` to
   `fMaxFlySpeed` by Speed + Levitate. (`Player::flySpeed`, set in `Session`.)
2. **Over-encumbered while levitating** could still fly; OpenMW: speed 0. Jumping over the limit was allowed; OpenMW:
   no jump.
3. **`fJumpRunMultiplier`** was applied to every jump; OpenMW only while running.
4. **NPC and creature speeds** were constants (walk 110, chase 260, flee 280, fighting ally 250, guard 200); now by each
   actor's Speed and Athletics (`Session::actorWalkSpeed` / `actorRunSpeed`), creatures running at walking speed.
5. **NPC arrows and thrown weapons** flew at a fixed 2500 / 1000; now `fProjectileMin/MaxSpeed`,
   `fThrownWeaponMin/MaxSpeed` by the draw, as the player's.

Test: `openmw-spec-movement` (generated): run speed from Speed and Athletics; jump speed from Acrobatics, Jump, load,
running and fatigue (`Session::runSpeedFor`, `jumpSpeedFor`). EXPECT kinds `npcwalk:<id>` / `npcrun:<id>` exist for
hand-written checks.

## Open

- The player moves by the Circle Pad's tilt (full tilt = run); OpenMW has a walk / run toggle. The swim speed uses the
  run speed.
- The strafing x 0.75 is not applied (analog movement mixes forward and sideways).
- NPC encumbrance is not tracked, so NPCs are never slowed by their load.
- NPC fall damage and movement animations are driven by our own animation speeds, not `mSpeedFactor`.
