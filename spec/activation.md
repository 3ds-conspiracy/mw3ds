# Activation: what the player's use key does

Read from OpenMW's `mwclass/*.cpp` (`activate`, `canBeEquipped`, `getEquipmentSlots`), `mwworld/action*.cpp`,
`mwworld/class.cpp`, `mwworld/refdata.cpp`, `mwmechanics/security.cpp` and `pickpocket.cpp`, and
`mwmechanics/mechanicsmanagerimp.cpp` (`isAllowedToUse`, `itemTaken`, `sleepInBed`, `unlockAttempted`), plus
`mwscript/miscextensions.cpp` / `interpretercontext.cpp` for the script hold. In our words. GMST values are those of the
converted data (`out/world/game_gmst.json`). The menu layout stays as on the 3DS; only the rules count here.

The engine asks the target's class what the activation would be (an *action*), then executes it. Every action may carry
a sound, played when it runs, including a failed one. Common checks, in this order, for every object type:

1. A script that asks `OnActivate` (see "Script hold") swallows the activation before the class is asked.
2. The class' own rule below.
3. Werewolf refusal: a player in werewolf form is refused by activators, containers, books, items, creatures and NPCs
   with the `sWerewolfRefusal` message and a random sound of a per-class family (`WolfActivator`, `WolfContainer`,
   `WolfCreature`, `WolfItem`). Doors are not refused. (`Class::getWerewolfRefusalAction`, `class.cpp`)

## Script hold

`mwworld/refdata.cpp` (`activate`, `onActivate`, `activateByScript`), `mwscript/miscextensions.cpp` (`OpOnActivate`,
`OpActivate`).

- An object whose script never reads `OnActivate` is activated normally.
- The first time a script evaluates `OnActivate` on a reference, activation of that reference is *held* from then on:
  a player activation does not run the class' action, it sets a one-shot flag the script reads as `OnActivate` = 1
  (reading it consumes the flag).
- The held activation runs only when the script itself executes `Activate` on the reference (or on a container, which
  always runs). Then the class' action below runs as if the player had done it, with the player as the actor.
- Practical result: the Census exit door and every scripted door, bed, button or quest object does nothing by itself
  until its script lets it.

## Doors (`Door::activate`, `door.cpp`)

In order:

1. Non-player actor on a teleport door: refused with the `LockedDoor` sound (a no-op).
2. A key is "had" when the door's key id is in the actor's inventory (searched by id).
3. Locked and key had: the message "<key name> used to open lock" (`sKeyUsed`), the door is unlocked (its lock level is
   kept, as a negative number), and a trap on it is removed with the `Disarm Trap` sound. The key stays in the inventory.
4. Not locked now, or key had:
   - still trapped (unlocked doors may carry a trap): the action is the trap, with the `Disarm Trap Fail` sound; it does
     not open the door that time (see Traps);
   - teleport door: the action is the teleport to the destination cell and position, with the door's open sound;
     a player using Telekinesis on a teleport door is refused (nothing happens);
   - animated door: toggles. It opens when closed or closing, closes when open or opening; the sound is the open or
     close sound, started at the door's current angle (doors turn 90 degrees a second).
5. Locked and no key: no message at all, only the `LockedDoor` sound. No crime, whoever owns the door; a lock pick or a
   spell is the only way (see Locks).

Telekinesis lets the player activate non-teleport doors from afar (the door glows for a second), and a teleport door
that is neither locked nor trapped cannot be used that way (`Door::allowTelekinesis`).

Tooltip: the destination for teleport doors, "Lock Level: N" while locked, "Unlocked" when a lock level exists but it is
open, "Trapped" with a trap. (Menus stay as on the 3DS; the data is what counts.)

## Containers and bodies (`Container::activate`, `container.cpp`; `Npc::activate`, `Creature::activate`)

Containers, in order:

1. Refused (nothing) while the inventory screen is not allowed (for example mid-dialogue).
2. Werewolf refusal.
3. Key search is in the *player's* inventory, with the same rule as doors: locked + key had: "used to open lock",
   unlock, trap removed with the `Disarm Trap` sound.
4. Not locked or key had: with no trap, an organic (plant) container is harvested, anything else opens the container
   screen; a trap fires first (below) and the container does *not* open that activation.
5. Locked, no key: the `LockedChest` sound only; no message.

Opening a container also runs the mechanics' open hook (contents respawn bookkeeping) and `diseaseContact` (a diseased
container may give the player a disease). Taking from it is checked by `itemTaken` (Ownership below).

Dead actors (`Npc::activate`, `Creature::activate`): a dead NPC or creature opens as a container once its death
animation has finished; OpenMW's setting `can loot during death animation` (on by default) opens it the moment it is
dead. Nothing in the vanilla game decides this, so a test must wait out the animation to hold either way. Creatures that are alive and not knocked down start a conversation (their dialogue is the
creature's); a companion-scripted creature falls back to the container.

## NPCs (`Npc::activate`, `npc.cpp`)

Order of decisions for the player activating a living NPC:

1. Dead: container (above).
2. Knocked down and not in combat with the player: the container window (pickpocket-style, free of the sneak rule).
3. Player is sneaking and the NPC is not in combat with the player (or pursuing): the container window, which is the
   pickpocket window. This is checked before talking, so a sneaking player can never talk.
4. Not in combat with the player, not a werewolf: conversation.
5. In combat with the player or pursuing him: the message `sActorInCombat` ("This character is in combat."), no window.
6. Otherwise nothing.

A conversation started from an actor activating the player is a talk with that actor.

## Traps (`ActionTrap`, `actiontrap.cpp`)

The trapped object's trap spell is cast with the object as the caster. The target is the activating actor when the
player is within activation distance of the object, otherwise (Telekinesis from afar) the object itself. After the cast the
trap is cleared permanently, so a second activation finds an ordinary door or container. The `Disarm Trap Fail`
sound accompanies the action.

Triggering needs the object to be unlocked or its key had: a locked, trapped object with no key only gives the locked
sound and the trap does not fire. Having the key disarms silently (Disarm Trap sound).

## Locks and traps by tool (`Security::pickLock`, `Security::probeTrap`)

Used by the equipped lock pick / probe on a locked or trapped target (the tool's use action, not the plain activation).

    base = 0.2 Agility + 0.1 Luck + Security
    lock:  x = base x pick quality x fatigue term + fPickLockMult (-1) x lock level
    trap:  x = (base + fTrapCostMult (0) x trap spell cost) x probe quality x fatigue term

Nothing happens (no use spent) if the target's lock level is 0 or less, it has no tooltip (an unlocked prop), the trap
is empty, or the tool has no uses left. Otherwise: `x <= 0` is the "too complex" message (`sLockImpossible`,
`sTrapImpossible`, sound `Open Lock Fail` / `Disarm Trap Fail`); else a roll 0..99 succeeds when `roll <= x`: lock opens
(`sLockSuccess`, `Open Lock`) or trap disarmed (`sTrapSuccess`, `Disarm Trap`) and Security trains (pick lock / disarm);
a failure gives `sLockFail` / `sTrapFail`. Every attempt uses one charge of the tool; at none it is removed from the
inventory. The attempt is also a *trespass* crime if the target is owned and was ever locked or trapped (below).
Numbers in the engine: `openmw-spec-security` already compares `lockchance` and `trapchance`.

## Ownership and crime (`isOwned`, `isAllowedToUse`, `itemTaken`, `unlockAttempted`, `sleepInBed`)

Owned means: an owner id other than `Player`, or a faction owner the player does not belong to with enough rank
(`ofac` / `orank` in the converted refs); either is cancelled when the object's owner-global is non-zero.

Using something is allowed without question when: the target is the actor itself; it is an unlocked, untrapped
door; it has no tooltip; it is an activator that is not a bed script (`Bed*`); the NPC is dead or in combat. Pickpocketing
a living, calm NPC is not allowed (a sneaking player, or a knocked-down NPC): a theft. Anything else owned is not allowed,
and so is the `stolen_goods` chest even when unowned.

Taking an item (`itemTaken`, run by the take action and when moving things out of a container, the owner being the
container's when taking from one):

- Allowed: nothing happens.
- Not allowed: the thief is remembered against the owner, and a theft crime with value (count, times the item's value
  unless gold) is committed with the owner's faction. Bounty is in `spec/crime.md` (`max(1, int(value x
  fCrimeStealing))` with `fCrimeStealing` = 1). If nobody sees it, no bounty.
- Taking an item from a *living* NPC's own inventory uses the NPC's id as the owner (a pickpocket).

Trespass:

- Sleeping in an owned bed (`sleepInBed`): refused with `sWerewolfRefusal` for a werewolf, with `sNotifyMessage2`
  ("You can't rest here enemies are nearby.") when enemies are near, then if the bed is owned a trespass crime
  (`iCrimeTresspass` = 5), and if the crime is reported the message `sNotifyMessage64` and the sleep is refused.
- Trying to pick the lock of or disarm an owned object that has been locked or trapped (even if now open) is a
  trespass.

## Pickpocket (`Pickpocket`, `pickpocket.cpp`)

Opens from the NPC window (Sneaking activation, above). Two rolls decide whether the victim notices.

    mod(actor, add) = (add + 0.2 Agility + 0.1 Luck + Sneak) x fatigue term
    x = mod(thief, 0)
    y = mod(victim, valueTerm)
    t = 2x - y
    caught when roll(0..99) > int(clamp(t, floor, iPickMaxChance))     where the floor is  thief Sneak / iPickMinChance

Concretely: when `t` is below `Sneak / iPickMinChance` the bar is `int(Sneak / iPickMinChance)`; otherwise `t` capped at
`iPickMaxChance`. `iPickMinChance` = 5, `iPickMaxChance` = 75.
Taking one stack: `valueTerm = 10 x fPickPocketMod (0.3) x (item value x count)`. Putting items back, or the final check on
closing the window (`finish`): `valueTerm = 0`. A caught player is a pickpocket crime (`iCrimePickPocket` = 25, see
`spec/crime.md`), and the Sneak skill trains on success (use type 1).

## Items taken from the ground (`defaultItemActivate`, `class.cpp`; `actiontake.cpp`)

For ingredients, potions, misc items (including keys), apparatus, armor, weapons, clothing, lock picks, probes, repair
tools and carriable lights: refused while the inventory is not allowed; werewolf refusal; otherwise the take action
with the item's pick-up sound.

- Take: `itemTaken` runs first (ownership), then the whole stack goes into the inventory (gold is added as coins worth
  its value times count), then the world object is removed. In a GUI mode with the inventory / container open the take is a
  drag instead.
- Lights without the Carry flag cannot be taken: nothing happens (no message).
- Activators do nothing by themselves (only the werewolf refusal): their script decides.

## Books and scrolls (`Book::activate`, `ActionRead`)

Activating a book on the ground does *not* take it: it opens the book (or scroll) screen. The first time the player
reads a particular book id that teaches a skill, that skill rises by one level (the book id is remembered). A player in
combat who reads a book that is already in the inventory gets `sInventoryMessage4` ("You cannot read during battle.");
reading a book on the ground in combat is allowed (otherwise one could never pick it up). Taking the book by the take
button is the take above.

## Equipping (`canBeEquipped`, `getEquipmentSlots`, `ActionEquip`)

`canBeEquipped` returns a code and an optional message. 0: not equipped, message shown to the player if any; any other
value equips. The equip action then puts the item in the first free slot of its slot list, or, when all are full,
cycles them (the first slot's item is pushed out of the way) -- for rings the two ring slots.

- Armor and weapons with condition 0 (broken): `sInventoryMessage1` ("This object is broken and cannot be equipped until
  fixed."), not equipped. (Lock picks / probes with no uses can be equipped.)
- Armor with no slot (types that map to none): nothing.
- Beast races (Khajiit, Argonian; the race's Beast flag, NPCs only, not creatures): armor or clothing that has a Head
  part (a full helmet, the part that replaces the hair) gives `sNotifyMessage13` ("Beast races cannot wear full
  helmets."); armor with a foot part gives `sNotifyMessage14` ("... cannot wear boots."), clothing with a foot part
  `sNotifyMessage15` ("... cannot wear shoes."). Open helms, gloves, greaves and robes are fine.
- Slots by type: armor helmet, cuirass, left / right pauldron, greaves, boots, left / right gauntlet (bracers share
  the gauntlet slots), shield (left hand); clothing shirt, belt, robe, pants, shoes (the boots slot), left / right glove,
  skirt, amulet, ring (two slots: left ring, right ring); weapons: the right hand, except ammunition (the ammunition
  slot, stackable) and thrown weapons (right hand, stackable); a carriable light goes in the left hand (not carriable:
  refused, no message).
- Two-handed weapons return code 2, a shield with a two-handed weapon in the right hand code 3. Neither is refused
  in `apps/openmw`: the equip goes ahead and nothing in the reading list unequips the other hand; the code is a hint
  (see the findings: the shield / weapon exclusion is left to the vanilla rule we follow on purpose).
- Weapons: while the player is mid-attack or casting with the GUI open, equipping is refused with `sCantEquipWeapWarning`
  unless the new item is ammunition for the weapon in use (`Weapon::canBeEquipped`).
- Equipping an item whose script exists sets `OnPCEquip` to 1 on it (books `PCSkipEquip`; ingredients, books and
  repair tools never get `OnPCEquip`) -- the inventory window does it, not the action.

## Where the numbers are

| Number | Value | Source |
|---|---|---|
| Lock chance | see Locks | `security.cpp` |
| `fPickLockMult`, `fTrapCostMult` | -1.0, 0.0 | GMST |
| `iPickMinChance`, `iPickMaxChance` | 5, 75 | GMST |
| `fPickPocketMod` | 0.3 | GMST |
| `iCrimeTresspass`, `iCrimePickPocket` | 5, 25 | GMST |
| Disarming with a key | always | `door.cpp`, `container.cpp` |
| Trap range | maximum activation distance | `actiontrap.cpp` |

Messages and sounds are named by their GMST / sound id above; the converted data lacks the `Disarm Trap` and
`Disarm Trap Fail` sounds (see `spec/findings/activation.md`).
