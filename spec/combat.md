# Combat

Mirrors the OpenMW wiki page *Research:Combat*. Rules read from OpenMW's `mwmechanics/combat.cpp`
(`getHitChance`, `blockMeleeAttack`, `adjustWeaponDamage`, `getHandToHandDamage`, `applyFatigueLoss`,
`applyElementalShields`, `resistNormalWeapon`, `reduceWeaponCondition`, `projectileHit`), `mwclass/npc.cpp` and
`creature.cpp` (`hit`), `mwmechanics/character.cpp` (`getFallDamage`), `mechanicsmanagerimp.cpp` (`awarenessCheck`),
`disease.hpp` (`diseaseContact`) and the Lua `data-mw/scripts/omw/combat/common.lua` / `local.lua` (armor rating,
armor, difficulty, stagger). In our words. Where this page and the code disagree, the code in `source/mwmechanics/formulas.cpp`
is what the tests read.

## Fatigue term

`fFatigueBase - fFatigueMult x (1 - current / max)`, the fraction taken as 1 when the maximum is 0.

## Hit chance

    attack  = (weapon skill + Agility / 5 + Luck / 10) x fatigue term + Fortify Attack - Blind
    evasion = (Agility / 5 + Luck / 10) x fatigue term + min(100, Sanctuary)
    defense = evasion (0 while the victim is knocked down, paralyzed, or unaware of the player)
              + min(100, fCombatInvisoMult x Chameleon) + min(100, fCombatInvisoMult x Invisibility)
    chance  = round(attack - defense);  the blow lands when a roll 0..99 is below it

A victim with negative fatigue has no defense at all. Creatures use their combat / magic / stealth skill for the
weapon skill.

## Reach (who a blow lands on)

    reach = fCombatDistance x the weapon's reach (people's fists: x fHandToHandReach; a creature's own attack: x 1)
    distance to bounds = distance between the two origins - each one's collision box half length (getHalfExtents().y:
                         a creature's "Bounding Box" node, people's base_anim.nif box, 28.5; x scale)

An attacker starts a blow when the distance to bounds is within its action's range (the same reach for melee), and
the blow lands (`getHitContact`) on a foe whose distance to bounds and height difference are both under the reach, in
front within the angle whose sine is `fCombatAngleXY / 90` (about 42 degrees). Ours measured from the origins: a
creature's at `fCombatDistance x 0.9` whatever its size, landing within 40 more and 57 degrees round, and the
player's fists at 0.7 (`bug-creature-reach`: the guar, 120 long each way of its middle, now fights from about 250
instead of 104; 2026-10-13). A touch spell's range is `fCombatDistance` from the edges too.

## Awareness (who counts as unaware)

Unaware = not in combat and failing OpenMW's awareness check:

    sneak term = fSneakSkillMult x Sneak + 0.2 Agility + 0.1 Luck + boots' weight x fSneakBootMult   (0 unless sneaking)
    x = sneak term x (fSneakDistanceBase + fSneakDistanceMultiplier x distance) x fatigue term + Chameleon (+100 invisible)
    y = (observer's Sneak + 0.2 Agility + 0.1 Luck - Blind) x their fatigue term x (fSneakViewMult in front, fSneakNoViewMult behind)
    noticed when the observer's roll (0..99, rerolled every 5 seconds) >= x - y

Not sneaking, the sneak term is 0, so only Chameleon or Invisibility can keep the player unnoticed. Chameleon 200 is not proof against a sharp observer: with y above 100 (Berwen: Sneak 47, Agility 65, Luck 40, y = 120) a roll of 80 or more still notices, so a theft in her shop is seen about one roll in five; a weak observer (y under 101) can never notice it. The observer's Blind lowers y. Detection for
crime and the sneak indicator also needs line of sight. The Sneak skill trains every `fSneakUseDelay` seconds while
someone within `fSneakUseDist` has the player in sight and nobody has noticed them. Those siding with the player
(followers, summons) neither take the sneak indicator away nor train Sneak (OpenMW's `getActorsSidingWith`).

A head turns to the player only when its owner noticed them (OpenMW's `updateHeadTracking`: line of sight and the
awareness check), never to a player under Invisibility or Chameleon 75 or more (`isTargetMagicallyHidden`). Ours turned
every head in reach, so a sneaking player saw people look at them while the indicator said nobody had noticed
(2026-10-24, `bug-sneak-eye`).

Doors: a load door's two sides are two doors, each with its own lock in the data; unlocking one leaves the other as it
is (the Ministry of Truth's doors are locked on both sides). A door unlocked stays unlocked through cell changes and
saves (`bug-door-stays-unlocked`).

## Damage

- **Melee weapon:** `min + (max - min) x swing` of the chosen attack (chop, slash, thrust), x condition / max
  condition, x `(fDamageStrengthBase + 0.1 x fDamageStrengthMult x Strength)`.
- **Bow / crossbow:** launcher's chop + the missile's chop, each `min + (max - min) x draw`; a thrown weapon counts
  twice (it is both). Then condition and Strength as above.
- **Hand to hand:** `Hand-to-hand x (fMinHandToHandMult + (fMaxHandToHandMult - fMinHandToHandMult) x swing)` to
  fatigue. On a victim knocked down or paralyzed it goes to health instead, times `fHandtoHandHealthPer`.
- **Creatures:** their attack's `min + (max - min) x swing` (no Strength term). Only creatures with the CREA
  "Weapon and Shield" flag (skeletons, dremora, golden saints) carry a weapon: the best one they hold, chosen as
  for an NPC. With it, the weapon's chop / slash / thrust `min + (max - min) x swing`, still no Strength term
  (`Creature::hit`), with their combat skill as the weapon skill. (Their weapons do not wear here.) A weapon its
  own script gives it counts too (OpenMW's autoEquip once AddItem has run): Tribunal's sphere archers get their
  shock darts from replenishammoShock (`creature.py script_weapon`). Darts or arrows added to an actor count as
  ammunition, and each shot leaves what it carries (`World::refAddItem`, `npcFire`). With a weapon out an armed
  creature plays its model's ready-stance groups when it has them (Idle1h, RunForward1h: `actors.py STANCE_GROUPS`),
  the archers unfolding. Test: `trib-sphere-archer`.
- **Critical strike:** a melee blow by the player on an unaware victim, x `fCombatCriticalStrikeMult`. A missile on an
  unaware victim, and any blow on a knocked-down victim, x `fCombatKODamageMult` instead.
- **Resist Normal Weapons:** a weapon that is neither silver, magical nor enchanted does `x (1 - min(1, resist -
  weakness) / 100)`; fists are never resisted. A missile from a plain launcher (or thrown) is judged by the missile.
- **Armor** (health damage only): `damage x max(fCombatArmorMinMult, damage / (damage + armor rating))`, at least 1.
- **Difficulty:** damage to the player `x (1 + d x fDifficultyMult)` (d = difficulty / 100 > 0; `d / fDifficultyMult`
  when below 0); damage by the player the reverse.
- Weapon wear: `max(1, fWeaponDamageMult x damage)` per hit.

## Armor rating

Nine slots: `0.3 cuirass + 0.1 (shield, helmet, greaves, boots, each pauldron) + 0.05 (each hand)` + Shield effect.
A worn piece counts `armor x skill / iBaseArmorSkill` (weight 0: its armor as it is), times condition; an empty slot
counts `fUnarmoredBase1 x Unarmored x fUnarmoredBase2 x Unarmored`. The weight class of a piece is its weight against
`i<Slot>Weight` x `fLightMaxMod` / `fMedMaxMod` (+0.0005). A bracer is its hand's piece. Creatures: the Shield effect only.

## The piece that is hit

A roll 0..99: cuirass under 30, helmet 30s, greaves 40s, boots 50s, left pauldron 60s, right pauldron 70s, left hand
80-84, right hand 85-89, shield 90 and up. That piece (or Unarmored for an empty slot) gets the skill use, and loses
`-floor(armored damage - raw damage)` condition. A creature's own attacks (no weapon) wear no armor.

## Blocking

Only with a shield, not while knocked down, recovering from a hit or paralyzed, and only for an attacker between
`fCombatBlockLeftAngle` and `fCombatBlockRightAngle` of facing.

    block  = (Block + 0.2 Agility + 0.1 Luck) x (swing x fSwingBlockMult + fSwingBlockBase)
             x fBlockStillBonus (when not moving forward) x fatigue term
    attack = (attacker's weapon skill + 0.2 Agility + 0.1 Luck) x their fatigue term
    chance = clamp(int(block - attack), iBlockMinChance, iBlockMaxChance); blocked when a roll 0..99 is below it

A block wears the shield by the damage, costs `fFatigueBlockBase + load x fFatigueBlockMult + attacker's weapon
weight x swing x fWeaponFatigueBlockMult` fatigue, trains Block, and sounds by the shield's weight class.

## Knockdown (stagger)

After a blow with health damage: knocked down when the raw health damage (before armor) is at least `Agility x
fKnockDownMult` and a roll 0..99 is at least `Agility x iKnockDownOddsMult / 100 + iKnockDownOddsBase`; else a hit
recovery. Fatigue below 0 knocks out, whatever brought it there (`CharacterController::refreshHitRecoilAnims`): the
actor lies in its KnockOut group's loop for as long as the fatigue stays under 0, then plays the rest (getting up). A
knocked-down actor plays KnockDown through. Ours knocked out only on a melee hit and got up after 3.5 s whatever the
fatigue, and counted the time on the floor only in a fight (`bug-knockout-fatigue`, 2026-10-13).

## Fatigue spent

Each swing or shot: `fFatigueAttackBase + load x fFatigueAttackMult + weapon weight x swing x fWeaponFatigueMult`.

## Elemental shields

A melee striker of a bearer of Fire / Lightning / Frost Shield takes, for each:
`save = (Destruction + 0.2 Willpower + 0.1 Luck) x 1.25 x fatigue fraction`, `x = min(100, max(0, save - roll 0..99)
+ resistance to the element)`, damage `fElementalShieldMult x magnitude x (1 - x / 100)`.

## Disease on contact

A blow from a diseased creature: for each disease the victim lacks, a chance of `fDiseaseXferChance x (1 - (resist -
weakness) / 100)` percent (common, blight or corprus resistance by the disease).

## Falling

`x = max(0, height - fFallDamageDistanceMin - 1.5 Acrobatics - Jump)`, damage `(fFallDistanceBase + fFallDistanceMult
x x) x (fFallAcroBase + fFallAcroMult x (100 - Acrobatics))` when the height reaches the minimum; nothing in water.
Health lost is that `x (1 - 0.25 x fatigue term)`. Above `Acrobatics x fatigue term` it knocks down; otherwise the
landing trains Acrobatics (use 1).

## Skill uses

Armor hit 0, Block 0, weapon hit 0, spell cast 0, Alchemy 0 / ingredient 1, Enchant recharge 0 / use 1 / create 2 /
strike 3, Acrobatics jump 0 / fall 1, Mercantile 0 / bribe 1, Security trap 0 / lock 1, Sneak avoid notice 0 /
pickpocket 1, Speechcraft 0 / fail 1, Armorer 0, Athletics run 0 / swim 1.

## Fights between two actors (StartCombat)

`StartCombat <actor>` on a script's actor gives it a combat package with that actor as the target (the player is only
one possible target). It walks up to the target and swings; the target takes up the fight against it unless it is
already in one. A blow rolls the attack term less the target's evasion, the weapon's (or creature attack's) damage,
then armor with a floor of 1; a death ends the fight (it is nobody's murder). `StopCombat` clears it. Not covered:
blocking, critical hits, spells and flight between actors; the fight is not kept in a save.

The player's summons and followers fight whoever fights the player the same way (OpenMW gives them a combat package
against the player's foes): the nearest within 1500. Ours struck at once every 1.2 s from 120 away, never missing
(`bug-summon-fights`, 2026-10-13).

## Findings

2026-09-29 (earlier): armor rating ignored the armor skill and Unarmored (fixed for the player, `Session::playerArmor`,
and NPCs, `tools/convert/npcstats.py armor_rating`).

2026-09-29, this pass (all in `mwmechanics/combat.cpp`, `mwworld/projectilemanager.cpp`, `mwmechanics/formulas.cpp`, `mwbase/session.cpp`):

1. **Hit chance** was 100 against anyone unaware or knocked down; OpenMW only drops their evasion. The player's
   Chameleon and Invisibility didn't add to defense; the result wasn't rounded; NPC shots ignored Sanctuary.
2. **Critical strikes** needed the player to be sneaking; OpenMW only needs the victim unaware. Missiles on the unaware
   did x4 (melee's crit); OpenMW: `fCombatKODamageMult`. Blows on a knocked-down victim had no multiplier.
3. **Knockdown** judged the damage after armor and difficulty; OpenMW the raw damage.
4. **Armor** could take a blow to 0; OpenMW leaves at least 1. The armor piece's wear was the whole difference with a
   minimum of 1; OpenMW: `-floor(adjusted - raw)`, and none from a creature's own attacks.
5. **Bracers** were never hit (the slot roll only matched gauntlets), so a bracer hand trained Unarmored.
6. **Silver weapons** were resisted by ghosts (only the magical flag was checked); **fists** were resisted.
7. **Blocking** had fixed numbers (no GMSTs), always counted as standing still, always sounded heavy, cost no fatigue,
   and a shield blocked blows from 57 degrees either side instead of `fCombatBlockLeftAngle`..`RightAngle`.
8. **Hand to hand** on a knocked-down victim still went to fatigue; OpenMW: health x `fHandtoHandHealthPer`.
9. **Elemental shields** on the player didn't hurt melee attackers.
10. **Disease** transfer ignored weakness and corprus resistance and `fDiseaseXferChance`.
11. **NPCs' swings** cost them no fatigue; a bow shot cost the player a fixed 2 x (0.5 + draw).
12. **Falls** took the full damage (OpenMW: `x (1 - 0.25 x fatigue term)`), never knocked down, and trained Acrobatics
    on any hurt landing.
13. **Detection** used our own formula (distance / 500, facing x 1.5); now OpenMW's awareness check with its 5-second
    roll and line of sight. Line-of-sight casts for every actor within 2000 units four times a second: the cost on
    the 3DS is not measured.
14. Arrows stayed in bodies 25% of the time whatever the GMST, enchanted ones too.

15. **`OnPCHitMe`**: a blow of the player's that lands on an actor whose script declares the local sets it to 1 (the script clears it
    itself). We never set it, so Synette Jeline's ambush (The Lady's Ring, `MV_LostRing` stage 40) could not finish. Test:
    `side/the-ladys-ring`.

Test: `openmw-spec-combat` (generated): attack term, the player's defense, knockdown odds, fall damage, block chance,
elemental shield damage.

## Open

- Enchanted weapons count as magical (OpenMW's default setting); vanilla may differ.
- NPC armor ratings come from the converter (`tools/convert/npcstats.py`), fixed at their worn condition.
- The attack type an NPC picks (chop / slash / thrust) is random here; OpenMW picks by the weapon's best attack in AI.
