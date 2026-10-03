# World rules

Rules read from OpenMW's `mwworld/containerstore.cpp`, `cellstore.cpp`, `datetimemanager.cpp`, `duration.hpp`,
`timestamp.cpp`, `actionteleport.cpp`, `actiontake.cpp`, `worldimp.cpp` (enable, disable, deleteObject,
advanceTime), `engine.cpp` (the per-frame clock), `mwmechanics/levelledlist.cpp`, `actors.cpp` (soulTrap),
`spelleffects.cpp`, `mechanicsmanagerimp.cpp` (getDerivedDisposition), `mwdialogue/journalimp.cpp`, `quest.cpp`,
`dialoguemanagerimp.cpp` (faction reaction) and `mwgui/dialogue.cpp`, `containeritemmodel.cpp` (restock). In our
words. The oracle is `tools/test/specgen_world.py` (tests `openmw-spec-world`, `openmw-spec-world-hooks`); what each
test needs from the engine and what is still unread is in `spec/findings/world-rules.md`.

## 1. Containers, NPC inventories, leveled lists, restock

**Leveled list pick** (`levelledlist.cpp getLevelledItem`). Inputs: the list, the player's level (not the
actor's), a random generator.
1. Roll 0..99; below the list's chance-none, nothing is given.
2. `highest` = the largest entry level that is at most the player's level (0 if there is none).
3. Candidates: every entry whose level is at most the player's, and, unless the list's "all levels" flag is set,
   whose level equals `highest`. For creature lists the same flag decides, but its bit has a different meaning in the
   record than for item lists (the file format swaps them); our converter already exports one `all` flag per list.
4. No candidates: nothing. Otherwise one candidate is chosen uniformly.
5. An id that does not exist is dropped with a warning. An id that is itself a leveled list recurses with the same
   level and rolls its own chance-none; a real item is returned.

**Filling** (`containerstore.cpp fill, addInitialItem`). Each inventory line has an id and a count; a count of 0 does
nothing. An item with a script is added one at a time so scripted items never share a stack. A leveled list line
is resolved with a generator: if the line is top level, its count is more than 1 (or less than -1), and the list has the *each*
flag, it is rolled once per unit (each roll gives 1 item); otherwise it is rolled once and the chosen item gets the
whole count (the nested pick is never top level, so *each* applies only to the line itself). A negative count keeps
its sign through the leveled pick. The new items take the container's owner (a plain container has none).

**When a container is filled.** A container is unresolved until something touches it (open, take, put, script
reads); then `resolve` fills it from the record, once. Content already there is zeroed first. The generator seed is
stored with the container, so a saved, unmodified container refills the same way. Anything the player changed
marks it modified, and from then on it is never refilled by this path.

**Restock** is not a timer on the items. A line with a *negative* count in a merchant's (or any actor's) record is a
restocking quantity: the stack is kept with a negative count, `|count|` is what it shows, stacking onto it adds to
the absolute value and keeps the sign (`addItems`), and when the player buys from it (`containeritemmodel.cpp
removeItem`, trading only) up to that quantity is not removed, so the merchant still has it. Gold is the only thing
with a clock: opening dialogue with a merchant resets their gold to the record's value when `fBarterGoldResetDelay`
(24) hours have passed since the last reset (`mwgui/dialogue.cpp restock`).

**Gold** into any container: the item is turned into `gold_001`, whatever the id (a `gold_100` added counts as
one gold, not 100), and joins the one gold stack.

GMSTs: `fBarterGoldResetDelay`. No other GMST is read by the pick.

## 2. Respawn of corpses and containers

**Cell respawn** (`cellstore.cpp respawn`), run on a loaded cell:
- Containers: when `iMonthsToRespawn` (4) x 30 x 24 hours (2880) have passed since the cell's last respawn, every
  container with the Respawn flag that has been touched drops its generated content; the next open refills it
  (`Container::respawn`: untouched ones need nothing). The cell's timer is set at that moment.
- Actors: every creature and NPC with custom data (was killed, looted, ...) is checked each time, not on the container
  schedule: a dead actor whose death animation has finished, which is not persistent (essential and quest-bound
  ones are), and with `time of death + fCorpseClearDelay` (72 hours) at or before now, is deleted. The test is
  `<=`: exactly 72 hours counts.
- Actors that were moved into the cell from elsewhere are checked the same way, and so are leveled creature
  spawn points (they respawn).
- The corpse delay is in game hours, and the clock is the global game time stamp (day and hour).

**Not read (outside the allowed files):** `Creature::respawn` / `Npc::respawn`, which bring a dead actor with the
Respawn flag back (they use `fCorpseRespawnDelay`), and where `CellStore::respawn` is called from (believed: each
time a cell is loaded).

GMSTs: `iMonthsToRespawn`, `fCorpseClearDelay`.

## 3. Journal and quest state

**Quest** (`quest.cpp`): an index (starts 0), a finished flag (false), the heard entries, a name.
- *Name*: the response text of the topic's entry marked as the quest name.
- `setIndex` sets the index to any number, up and down, whether or not an entry exists for it.
- Adding an entry for a quest (`Quest::addEntry`): unknown index -> error. If the entry is marked *finished* or
  *restart*, the finished flag is set to true or false accordingly. Then, only if the entry's index is greater than
  the quest's, the index becomes it. An entry the quest already has is not added twice.

**Journal command** (`journalimp.cpp addEntry`), the script `Journal quest, index` and dialogue results:
1. The entry is found by the quest's info whose index equals `index` (the first). None: error.
2. If the journal (the shown list) already holds that entry: if the quest's index is lower than `index`, set it to
   `index` and show "journal updated"; stop. It never writes a second line.
3. Otherwise the quest takes the entry (rules above). If the entry was a *restart*, every other quest with the same
   name that is finished is reopened (finished false).
4. A line is added to the journal, and "journal updated" shown, only when the entry's text is not empty. An
   empty-text entry still moves the index and the finished flag, and, never being in the journal, is not
   "already heard" next time.
5. The line carries the date it was written (day of month, month) and the actor.

**SetJournalIndex** only sets the index (step `setIndex`): no line, no finished change, can lower it.
**GetJournalIndex** is the index, 0 for a quest never started. A quest started by `Journal` is created at index 0
first.

GMSTs: `sJournalEntry` (the message).

## 4. Date and time

State: hour (float, 0 up to just under 24), day of month (1-based), month (0-11), year, days passed, timescale.
`DateTimeManager`:
- Days per month: 31 28 31 30 31 30 31 31 30 31 30 31 (no leap years).
- **advance by h hours** (`advanceTime`): `t = hour + h`; the hour becomes `t mod 24` (capped just below 24); whole
  days `int(t / 24)` go to the day of month, through the month lengths, month 11 to month 0 adds a year; and `int(t / 24)`
  is added to *days passed* (so a plain advance and the calendar always move together).
- **set hour** (`setHour`, the script `SetGameHour` / global): negative -> 0; the hour is `x mod 24`; whole days in
  `x` move the calendar day only, *not* days passed.
- **set day**: below 1 -> 1; a day past the month's end wraps into the following months (carrying the year).
- **set month**: negative -> 0; `m / 12` is added to the year, month is `m mod 12`; a day of month beyond the new
  month's length is cut to it.
- **set year**, **set days passed**, **set timescale**: stored as is.
- **Per frame** (`engine.cpp`), when not paused: `hours = frame seconds x timescale / 3600` advance (incremental).
  The default timescale is 30 (a game minute per 2 real seconds). A pause is the console, a message box needing an
  answer, the post-processor HUD, or no game loaded; menus pause through the tags the game sets.
- A fast-forward (sleep, travel, training: 2 hours, jail: days) is the same advance, with magic items recharged
  for `hours x 3600 / timescale` seconds.
- Time stamp comparison: day first, then hour; `a - b` in hours is `24 x days + hours` taken the right way round.
- Month names are localised strings by month number.

GMSTs: none; the globals `gamehour`, `day`, `month`, `year`, `dayspassed`, `timescale`.
Not read: day of the week (`worldimp`), which we believe is `(days passed + offset) mod 7`.

## 5. Soul trap

Applying the effect (`spelleffects.cpp`) puts it on the creature; the player is told "invalid target" if it is a
creature with soul 0. The soul is taken when the creature dies (`actors.cpp soulTrap`), **creatures only** (an NPC
never gives a soul):
1. Needs a Soul Trap effect with magnitude above 0 on the victim, and the creature's record soul value above 0.
2. For each such effect, the caster (any actor with an inventory, not only the player) is looked at: among the caster's
   soul gems (miscellaneous items marked as gems) that are **empty**, the capacity of a gem is
   `gem value x fSoulgemMult` (3); it must be at least the creature's soul value; the gem with the **smallest**
   capacity wins (strictly smaller replaces, so the first of equal ones stays).
3. No such gem: try the next effect (another caster's); none: nothing.
4. The soul goes on **one** gem of a stack (the stack is split), which is then stacked with other gems that
   hold the same soul. The player sees "soul trapped"; the Soul Trap hit effect and the conjuration hit sound play
   at the creature.
5. One soul per death (the function returns after the first gem).

A gem with a soul does not stack with an empty one, nor with another creature's soul.

GMSTs: `fSoulgemMult`.

## 6. Followers taken through doors

`ActionTeleport` (doors and Teleport spells and scripts that carry followers), run for the player or any actor:
- Followers = actors with a follow package aimed at the actor (`getActorsFollowing`, not read: believed to be
  the follow AI package only, not escort).
- Taken with the actor: all of them *including hostile* ones (`includeHostiles` true from this path), except:
  - going indoors/not to an exterior: a follower whose script has the local `stayoutside` = 1 and who is in an
    exterior now stays.
  - farther than 800 units from the actor (squared distance over 800 x 800): stays.
- Each taken follower (and the actor) first lands (fall state reset) and gets a "teleported" flag. A follower that is
  in combat with the player gets its combat stopped and is *not* moved this time. Others are moved to the same
  position in the destination cell.
- A water-walking effect that cannot be used on the new spot is removed.

GMSTs: none.

## 7. Disposition and faction reaction

`getDerivedDisposition(npc)` (clamped to 0..100 and cut to a whole number by default):

    x  = base disposition (NPC record / script-set) + crime modifier (a stored per-NPC number)
       + fDispRaceMod                                            when the player and the NPC share a race
       + fDispPersonalityMult x (player's Personality - fDispPersonalityBase)
       + (fDispFactionRankMult x rank + fDispFactionRankBase) x fDispFactionMod x reaction
       - fDispCrimeMod x player's bounty
       + fDispDiseaseMod                                         when the player has a common or blight disease
       + fDispWeaponDrawn                                        when the player's weapon is drawn
       + the NPC's Charm magnitude

Faction part:
- NPC with no faction: rank 0, reaction 0 (the term vanishes).
- Player in the NPC's faction and not expelled: reaction = that faction's reaction to itself; rank = the player's
  rank in it (0-based). An expelled player in it gets reaction 0, rank 0.
- Otherwise, for an NPC with a faction: over the player's factions (skip expelled ones) take the one with the
  lowest reaction of the NPC's faction toward it (first one wins a tie, in the faction table's order); rank = the
  player's rank there. A player in no faction: 0 and 0.

**Faction reaction** (`dialoguemanagerimp.cpp`): reaction of A toward B is, in order, the value set or changed
by script for the pair (A, B) (only that direction), else the entry for B in A's record, else 0. `Mod` adds to the
current value; `Set` replaces; both need the factions to exist.

Creatures have no derived disposition. Disposition is used by barter (`spec/trading-and-services.md`) and
persuasion (`spec/combat.md`), both of which clamp as above. In our base data `fDispCrimeMod` is 0 (bounty has no
effect), `fDispDiseaseMod` -10, `fDispWeaponDrawn` -5, `fDispRaceMod` 5, `fDispPersonalityMult` 0.5 over
`fDispPersonalityBase` 50, `fDispFactionRankMult` 0.5, `fDispFactionRankBase` 1, `fDispFactionMod` 3.

GMSTs: the `fDisp*` ones above.

## 8. Enable, disable, pick up, add, remove, stacking

**Enable / disable** (`worldimp.cpp`): enabling needs the object to be in a cell; it is a no-op when already
enabled; the scene gets the object only if the cell is active and the count is not 0. Disabling an object that is
already disabled, or one inside a container, does nothing; the player cannot be disabled (error). **Delete**
(`deleteObject`) sets the count to 0 and removes it from the scene (not for something in a container, nor the
player); a count-0 reference is gone for good, even if enabled again.

**Pick up** (`actiontake.cpp`): in the inventory/container GUI mode the item is dragged instead. Otherwise the
count of the world item is taken (for gold, `count x value` gold, so a pile of `gold_100` is 100 gold), the theft
bookkeeping runs, the item is added to the actor's inventory and removed from the world. In the inventory the item
loses its world position, owner, owner global, faction and faction rank; scripts on it are re-attached to the
container's cell (the player's items stay on in any cell), and an `OnPCAdd` local set to 1 when the player holds it.

**Stacking** (`containerstore.cpp stacks, addImp`): two items stack when all hold:
- the same id;
- the same soul;
- the same remaining usage time (lights);
- neither has a script;
- not both the same object;
- if enchanted, both at full charge (any used charge keeps it separate);
- if it has condition (weapons, armor, tools), both at full condition.
An equipped item is never stacked onto (inventory stores). The stacked count is the sum of absolute values; either
being negative (restocking) makes the result negative. Gold always joins `gold_001`.

**Add** (`ContainerStore::add`): stacks as above, otherwise a new stack. **Remove** by id takes from stacks in order
until the count is met; a stack removed down to 0 is empty and disappears; the count removed is returned (can be
less than asked). The weight cache is dropped on any change. A selected enchanted item is deselected when removed.
