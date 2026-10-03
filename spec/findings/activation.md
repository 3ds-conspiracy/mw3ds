# Findings: activation

Written from OpenMW's source only (the engine's own code was not read). The rules are in `spec/activation.md`; this page
is the audit side: rules in short, tests, hooks, questions. Audit rows: `spec/audit.md` (`session.cpp` activation,
`screens_items.cpp` equipping, taking, pickpocket). Already covered elsewhere and not redone: lock and trap chance
(`openmw-spec-security`, `combat.md`), theft / trespass / pickpocket bounties (`crime.md`, `openmw-spec-crime`),
`audit-findings.md` M1 (Activate ran the default action unconditionally), M2 (OnActivate consumed on read), M4 (Lock /
Unlock state), A13 (AiActivate). This page extends them with what the player sees on activation.

## Rules

- **Door** (`mwclass/door.cpp Door::activate`). Locked and key in inventory: message "<key name> used to open lock"
  (`sKeyUsed`), unlock (level kept), trap removed with sound `Disarm Trap`, key kept. Not locked / key had: a trap fires
  instead of opening (`ActionTrap`, sound `Disarm Trap Fail`) and is cleared; else teleport (open sound) or the animated
  toggle (open / close by current state). Locked, no key: sound `LockedDoor` only, no message, no crime.
- **Container** (`container.cpp Container::activate`). Same key / unlock / disarm rule against the *player's* inventory;
  locked, no key: sound `LockedChest` only; trap fires on the first activation and the container does not open that
  time; organic containers are harvested instead of opened; opening may give a disease (`diseaseContact`).
- **NPC** (`npc.cpp Npc::activate`). Dead: loot window (OpenMW: at once by default). Knocked down, not fighting the
  player: loot / pickpocket window. Player sneaking, NPC not fighting the player: pickpocket window (checked before
  talking). Calm: talk. In combat with the player: message `sActorInCombat`.
- **Creature** (`creature.cpp`). Dead: loot. Alive, not knocked down: talk. Else nothing (a script variable `companion`
  makes it a container).
- **Items** (`class.cpp defaultItemActivate`, `actiontake.cpp`). Take whole stack; ownership check (`itemTaken`) first;
  non-carry lights: nothing; activators: nothing; werewolf refusal for everything but doors.
- **Book** (`book.cpp`, `actionread.cpp`). Opens the book / scroll, does not take it; first read of a skill book raises
  the skill by 1; reading from the inventory in combat is refused with `sInventoryMessage4`.
- **Ownership** (`mechanicsmanagerimp.cpp isOwned / isAllowedToUse / itemTaken`). Unlocked untrapped doors, tool-tip-less
  objects and non-bed activators are free; owned items, containers and beds are theft / trespass; faction-owned objects
  count unless the player's rank in that faction is at least the object's.
- **Pickpocket** (`pickpocket.cpp`). `t = 2 x thief - victim` (each `(add + 0.2 Agi + 0.1 Luck + Sneak) x fatigue term`,
  victim's `add = 10 x fPickPocketMod x value x count`, 0 for the final check); caught when `roll > int(min(75, t))`,
  bar floored at `int(Sneak / 5)` when `t < Sneak / 5`.
- **Bed** (`mechanicsmanagerimp.cpp sleepInBed`). Werewolf refused; enemies nearby refuses with `sNotifyMessage2`; owned
  bed is a trespass crime and a reported one refuses the sleep.
- **Equip** (`mwclass/armor.cpp`, `clothing.cpp`, `weapon.cpp`, `light.cpp`, `actionequip.cpp`). Broken armor / weapon
  refused (`sInventoryMessage1`); beast races refused full helmets, boots, shoes (`sNotifyMessage13/14/15`) by the item's
  *mesh parts*, not its name; two-handed weapon / shield: both flagged, neither refused; mid-attack weapon swap refused
  (`sCantEquipWeapWarning`); no slot, nothing.

Stance on quirks: nothing here is a known OpenMW bug. Two points where OpenMW and the vanilla game differ or are
undecided are under Open questions.

## Tests written

All in `tools/test/cases/`, written by what a player does (walk to the object, activate it through the crosshair, take items
through the pick-up / inventory); each uses existing tokens only. They have not been run (no emulator in the writing
step).

- `openmw-spec-activation-door.txt`: locked owned door with no key (Seyda Neen, Fargoth's door: `LockedDoor` sound, no
  entry, bounty 0); the real Warehouse key picked up in the Census Office and used on the Seyda Neen warehouse door
  (message, entry, key kept, bounty 0); Mudan's locked + trapped vault door with its key (`key_door_Mudan00`: no damage,
  entry); Mudan's unlocked trapped door (`trap_shock00`: health falls).
- `openmw-spec-activation-container.txt`: Beshara `Com_Chest_11_alvur` without then with `key_alvur` (locked sound, then
  message + container screen, key kept); Bensamsi `chest_small_01_gold_50` (trap `flame`: first activation burns and
  does not open, second opens, no further damage); Sarys Ancestral Tomb `chest_tomb_large02` with `key_Sarys_chest`
  (key disarms `trap_shock00`: no damage, opens); Tel Vos, Aryon dead opens as a container.
- `openmw-spec-activation-npc.txt`: Arrille talks; the same NPC alarmed shows `sActorInCombat` and no screen.
- `openmw-spec-activation-item.txt`: loose ingredient taken (Census Office crab meat, no bounty); non-carry lamp in
  Arrille's not taken; `BookSkill_Alchemy2` in Ainat opens the book screen, is not taken, +1 Alchemy once only; the
  owned `misc_com_wood_cup_01` in Addammus's Yurt, taken in front of its owner: bounty 1.

Not written because a token is missing (see below): pickpocket numbers, sneaking activation, owned bed, beast races,
two-handed / shield, broken items, werewolf refusals, lock pick / probe use, telekinesis.

## Hooks needed in the engine

Names are proposals, in the existing style (`EXPECT:<kind>:<arg>:<op>:<value>`).

- `EXPECT:equipped:<item id>` -> 1 when that item is in an equipment slot, else 0. Needed for every equip rule.
- `EXPECT:canequip:<item id>` -> the code `canBeEquipped` would return (0 refused, 1 ok, 2 two-handed, 3 shield with a
  two-hander) with the message checked by `EXPECT:message`. Optional if `equipped` exists.
- `SETRACE:<race id>` (setup) so a Khajiit / Argonian / Redguard can try the same helmet, boots and shoes. Data cases:
  `iron boots`, `chitin boots`, `netch_leather_boots` (feet), `common_shoes_01` (shoes), `iron_helmet`, `steel_helm`,
  `nordic_iron_helm`, `imperial helmet armor`, `chitin helm` (check each one's parts: the converted objects lack the
  part list, so `EXPECT` cannot tell a full helm from an open one; an `armorparts:<id>` number, or the flag `fullhelm`,
  would do).
- `SETITEMHEALTH:<item id>:<n>` (a broken piece, 0) to test `sInventoryMessage1`.
- `SNEAK:<0|1>` (setup, the player's stance) for sneaking activation (NPC window instead of talk) and the sneak term.
- `KNOCKDOWN:<npc>` (setup, the NPC is knocked down) for the loot window on a living NPC.
- `EXPECT:pickpocketchance:<thief sneak>,<victim value term>` (in the style of `lockchance`): the chance the victim
  does *not* notice, from the thief's Agility / Luck / Sneak / fatigue and the victim's Agility / Luck / Sneak /
  fatigue, taking the item value and count. Without a window to drive the menu, the formula test needs this.
- `WEREWOLF:<0|1>` to test `sWerewolfRefusal` on non-door activation.
- `EXPECT:reflock:<dest cell>` for doors (ids repeat: `ex_nord_door_01`, `door_dwrv_loadup00`), to see a key left the
  door unlocked.
- `USELOCKPICK:<ref>:<tool id>` / `USEPROBE:<ref>:<tool id>` driving the equipped tool at the crosshair with `ROLL`
  holding the die, so `Security::pickLock` / `probeTrap` outcome and the charge used can be tested (the chance alone is
  covered by `lockchance`).
- `EXPECT:screen:<name>` for the pickpocket window (the container screen opened for an NPC can be told only by `refitem`).
- `EXPECT:message` also for notices in the `SayText` / log style if used for `sKeyUsed`.

Data issue: the converted sounds (`out/world/game.json` `sounds`) have `LockedDoor`, `LockedChest`, `Open Lock` but no
`Disarm Trap`, `Disarm Trap Fail` or `Open Lock Fail`, so `EXPECT:soundstarted:Disarm_Trap` cannot be written yet.

## Open questions

1. **Two-handed weapon and shield.** In `apps/openmw` neither is unequipped: `Armor::canBeEquipped` returns 3 and
   `Weapon::canBeEquipped` returns 2, but `ActionEquip` and `InventoryWindow::useItem` treat every non-zero code
   alike and `InventoryStore` has no rule for it (the shield is only hidden while a two-hander is swung in
   `character.cpp`). The vanilla game unequips the other hand. Decision needed: which to follow (the existing
   engine behaviour is not checked here).
2. **Dead-actor looting.** OpenMW loots at once by default; vanilla waits until the death animation ends. A test that waits
   long enough passes under both; one that does not is a choice.
3. **Bed.** `Activator::activate` only does the werewolf refusal; sleeping is called by whoever owns the bed handling
   (`sleepInBed` is read; the caller is outside the list). Check before writing the owned-bed test: the crime needs a witness
   to be reported, and the refusal message `sNotifyMessage64` and the `iCrimeTresspass` bounty of 5 follow only then.
4. **Beast helmets.** The rule is by mesh part (a head part), not by the item name: open helmets (no head part) are fine
   for Khajiit / Argonians. The converted `game_objects` do not carry parts, so the engine must have some other
   source; the spec cannot say which items are full helms without that list.
5. **The driver and DOORTO on a locked door / trapped door.** The tests assume `DOORTO` retries the door until the
   cell changes (or gives up with a log line); for the trapped Mudan door, a second activation (after the trap) should
   enter the Right Tower, but the test checks only the health.
6. **Witnesses for theft.** The bounty tests assume the owner sees the pickup (same small room, awake, facing). If
   the engine's witness rule differs from `crime.md` ("Open" list), the bounty test fails for that reason, not for
   activation.

## Hooks added (source/testdrive.cpp)

Reused (already there): `EXPECT:reflock`, `locklevel`, `itemhealth`, `SEED`, `USE`, `ROLL`, `PCRACE`. New:
`SETRACE:<race>` (as PCRACE), `SETITEMHEALTH:<id>:<n>`, `SNEAK:<0|1>`, `WEREWOLF:<0|1>`, `KNOCKDOWN:<npc>`,
`USELOCKPICK:<ref>:<tool>` / `USEPROBE:<ref>:<tool>` (one try, `ROLL` holds the die), `EXPECT:equipped:<id>`,
`EXPECT:canequip:<id>` (0 / 1 / 2 / 3; a refusal also shows its message), `EXPECT:armorparts:<id>` (1 head part, 2 foot
part; needs the data rebuilt), `EXPECT:pickpocketchance:<npc>,<value term>` (percent not noticed: bar + 1),
`EXPECT:tooluses:<tool>`, `EXPECT:pickpocketing` (the open window is a pickpocket's), `EXPECT:reflock:<dest cell>` (the
door leading there). `ROLL` now also fixes the lock pick / probe die and the pickpocket roll.

New tests: `openmw-spec-activation-equip`, `-beast`, `-bed`, `-pickpocket`, `-knocked`.

## Open questions, resolved

1. Two-handed weapon and shield: followed OpenMW (both worn; the old engine unequipped the other).
2. Dead-actor looting: ours opens at once, as OpenMW's default.
3. Bed: sleeping runs from the bed's script (`ShowRestMenu` in `bed_standard`); owned-bed rules now in `Session::bedRefused`.
4. Beast helmets: no part list existed; the converter now writes `flags` on armor / clothing (1 head part, 2 foot part),
   read from the model's body part records. Needs `build_game.py` re-run.
5. `DOORTO` does not retry: it presses the door once. The trapped Mudan door therefore fires its trap and does not enter
   (the test checks the health only, as written). The locked + keyed one unlocks, disarms and enters on the one press.
6. Witnesses: a non-sneaking player is seen by any awake NPC within 2000 units with a line of sight (`crimeWitness`, `awarenessCheck`
   with no sneak term), facing not needed. The cup and bed tests rely on the owner standing in the same room.

## Mismatches

| Rule (OpenMW) | Our behaviour before | Cause | Status | File |
|---|---|---|---|---|
| Locked door / container with its key: unlock, disarm a trap (sound `Disarm Trap`) | A trap fired first, even for a locked door with no key and with the key | `springTrap` ran before the lock check | fixed (`lockGate`) | source/session.cpp |
| Locked, no key: sound only, no message | Extra "Locked" notice | `notify("Locked")` | fixed | source/session.cpp |
| Key use has no sound; the trap going off plays `Disarm Trap Fail` | `Open Lock` on the key, `Trap Trigger` on the trap | wrong sound ids | fixed | source/session.cpp, source/combat.cpp |
| Sounds `Disarm Trap`, `Disarm Trap Fail`, `Open Lock Fail` | Not in the converted data | not in `ENGINE_SOUNDS` | fixed in the converter, needs a data rebuild | tools/convert/build_game.py |
| NPC in combat with the player: `sActorInCombat` message | Nothing | silent return | fixed | source/session.cpp |
| Knocked-down NPC (not fighting): loot window, no rolls; taking is a theft from them | Talked, or the pickpocket rolls when sneaking | no rule | fixed (theft marks the NPC as owner) | source/session.cpp, source/screens_items.cpp, source/combat.cpp |
| Werewolf refused by everything but doors | No werewolf form in the engine | whole mechanic missing | partly: refusal hook only (`WEREWOLF`); no transformation or script functions | source/session.cpp |
| Owned bed: trespass, refused only if someone reports it; werewolf and enemies refused first | Refused at once, no crime, whoever watched | `F_SHOWRESTMENU` | fixed (`bedRefused`) | source/session.cpp, source/script.cpp |
| Equip broken armor / weapon: `sInventoryMessage1` | Worn | no check | fixed (`canEquip`) | source/screens_items.cpp |
| Beast races: no full helm, boots, shoes (by body parts) | No check | no part data | fixed, needs a data rebuild | tools/convert/build_game.py, source/screens_items.cpp |
| Two-hander and shield both worn | The other hand was unequipped | vanilla rule | fixed (user decision: follow OpenMW) | source/screens_items.cpp |
| Boots and shoes share one slot; shield and light share the left hand | Both worn together | slot table | fixed | source/screens_items.cpp |
| Script `Equip` is forced (no refusal) | n/a | | fixed (force flag) | source/screens_items.cpp |
| Reading a pack book in a fight: `sInventoryMessage4` | Opened | no check | fixed (uses `enemiesNear`, 3000 units, as the rest check) | source/screens_items.cpp |
| Non-carry lights do nothing when activated | Crosshair never finds them, but a direct activation took them | no check in `activate` | fixed | source/session.cpp |
| Pickpocket roll (`2x - y`, bar floor Sneak / 5, cap 75) | Same formula | | matches; the die can now be fixed with `ROLL` | source/combat.cpp |
| Lock pick / probe die | `rand` only | | `ROLL` hook | source/combat.cpp |
| Organic containers are harvested (taken whole, no window) | Opens the list | needs a harvest mesh state (graphic herbalism) | differs on purpose (3DS keeps the vanilla list) | source/session.cpp |
| Telekinesis on a trapped door / container: trap hits the object | Hits the player | no range rule | open | source/combat.cpp |
| Pickpocket window hides items by a Sneak roll each, hides equipped and bound items, no put-back | Shows everything | needs a filtered view of the container grid | open (drawContainer index mapping; no put-into-container action on the 3DS either) | source/screens_items.cpp |
| Mid-attack weapon swap refused (`sCantEquipWeapWarning`) | The inventory screen stops play, so no swing is in progress | unreachable | differs on purpose | |
| Dead actor / `diseaseContact` on opening a body | A diseased body gives nothing | not called from the loot path | open | source/formulas.cpp |
| Creatures: alive and not knocked down always start talk (no dialogue gives no window) | Only creatures with services or lines | equivalent outcome | differs on purpose | source/session.cpp |
| Sneaking pickpocket skips companions here | `!r.ally` skip | kept | open (OpenMW opens the window for followers too) | source/session.cpp |
| `OnPCEquip` / `PCSkipEquip` rules (books, ingredients, repair tools) | `onpcequip` set for every equip | | open | source/screens_items.cpp |
