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

In the air (OpenMW's `CharacterController::updateState` and `MovementSolver`): the take-off's speed along the ground
is kept until landing (inertia), and the pad adds only `min(1, fJumpMoveBase + fJumpMoveMult x Acrobatics / 100)` of
the run speed on top. A standing jump has no inertia. Landing, swimming, levitating and doors / travel end it. This is
what makes the speedrunners' jump (Fortify Speed, Scroll of Icarian Flight, run and jump) cover whole cells.

## Levitation

Levitate is read while it lasts (`Player::levitate`): flying where the view points at `fMinFlySpeed` to
`fMaxFlySpeed` by Speed + Levitate, up with R / B, down with L, with collision like walking.

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

## Findings (2026-10-07, while planning the uber quest tests)

6. **Levitate went through walls.** The effect switched on the free fly mode (`Player::flying`): a fixed 300
   units/s, no collision, through any wall. Now only `Player::levitate` (the collision-checked path, the
   `fMinFlySpeed` / `fMaxFlySpeed` speed of finding 1). Test: `bug-levitate-noclip` (the player flew 1210 units
   backwards through the Balmora guild's entrance wall before; stops after 118 now).
7. **Jumps had no momentum.** A running jump went straight up at the full jump speed and the pad steered the whole
   run speed in the air; standing still in the air stopped dead. Now the 45-degree take-off, inertia until landing,
   and `fJumpMoveBase` / `fJumpMoveMult` air control. Test: `bug-jump-momentum` (29 units covered before, 3222 after).
   The "falling for over 3 s" monitor now leaves a jump's flight alone (an Icarian Flight jump is long).
8. **Levitating in the water** stayed at the surface: swimming won over levitation. OpenMW's flying actors fly in
   and out of the water. Now levitation wins (`Player::swimming` cleared while levitating). Test:
   `bug-levitate-water`.

Test: `openmw-spec-movement` (generated): run speed from Speed and Athletics; jump speed from Acrobatics, Jump, load,
running and fatigue (`Session::runSpeedFor`, `jumpSpeedFor`). EXPECT kinds `npcwalk:<id>` / `npcrun:<id>` exist for
hand-written checks.

## Open

- Slow Fall: OpenMW scales the fall and the inertia by `1 - 0.005 x magnitude` each physics step; ours quarters
  gravity and caps the fall at 200 units/s, and leaves the inertia alone.
- Slopes: OpenMW keeps the inertia while sliding on a steep slope; ours ends it on any ground.
- Past the edge of the map: OpenMW has empty exterior cells with water everywhere; ours has no cell there (no water,
  no floor). An Icarian jump west from Seyda Neen landed 115000 units out, in "Wilderness".

- The player moves by the Circle Pad's tilt (full tilt = run); OpenMW has a walk / run toggle. The swim speed uses the
  run speed.
- The strafing x 0.75 is not applied (analog movement mixes forward and sideways).
- NPC encumbrance is not tracked, so NPCs are never slowed by their load.
- NPC fall damage and movement animations are driven by our own animation speeds, not `mSpeedFactor`.
