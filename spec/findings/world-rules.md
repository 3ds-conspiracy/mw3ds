# Findings: world rules

Spec: `spec/world-rules.md`. Oracle: `tools/test/specgen_world.py`. Written from OpenMW's source only; our
`source/world.cpp` and `source/session.cpp` were not read, and nothing here has been run (no build, no emulator).

## Rules

One heading per rule; OpenMW file:function, GMSTs, quirks. The prose is in `spec/world-rules.md`.

- **Leveled pick**: `levelledlist.cpp getLevelledItem`. Chance-none roll first; candidates are the entries at or below
  the player's level, only the top level unless the list's all-levels flag is set; recursion keeps the level and
  rerolls chance-none. Prior audit row I12 ("AddItem rolls a leveled list") only says it was fixed, not tested.
- **Each flag**: `containerstore.cpp addInitialItemImp`. Only on a top-level line with |count| > 1.
- **Fill / resolve / restock**: `containerstore.cpp fill, resolve, addItems`, `mwgui/containeritemmodel.cpp
  removeItem`, `mwgui/dialogue.cpp restock`. A negative count is the restock marker and sells without depleting; only gold
  has a timer (`fBarterGoldResetDelay`, on opening dialogue).
- **Respawn**: `cellstore.cpp respawn, clearCorpse`. `iMonthsToRespawn` x 720 hours for containers with the flag;
  `fCorpseClearDelay` (72 h, `<=`) for corpses; persistent actors are never cleared.
- **Journal**: `journalimp.cpp addEntry`, `quest.cpp addEntry, setIndex`. Index only goes up through `Journal`; down
  only through `SetJournalIndex`; finished / restart flags come from the entry; empty text -> no line, no message.
- **Calendar**: `datetimemanager.cpp`, `duration.hpp`. No leap years; set-day wraps; set-hour past 24 moves the day
  but not days passed; set-month clamps the day and carries years.
- **Timescale**: `engine.cpp` (frame seconds x timescale / 3600), default 30.
- **Soul trap**: `actors.cpp soulTrap`: creatures only, smallest empty gem with `value x fSoulgemMult >= soul`,
  one gem of the stack, restack.
- **Followers**: `actionteleport.cpp getFollowers`: within 800 units, `stayoutside` local honored, hostile ones included
  but their combat with the player is stopped and they stay.
- **Disposition**: `mechanicsmanagerimp.cpp getDerivedDisposition`: base + crime modifier + race + personality +
  faction + bounty + disease + weapon drawn + Charm, clamped 0..100 and truncated. GMSTs `fDisp*`.
- **Faction reaction**: `dialoguemanagerimp.cpp`: per-direction override, else the record, else 0.
- **Pick up / stacking / enable**: `actiontake.cpp`, `containerstore.cpp stacks, addImp`, `worldimp.cpp enable,
  disable, deleteObject`.
- Stance on vanilla behaviour: we follow OpenMW; where the setters wrap or clamp (`setDay`, `setMonth`, `setHour`)
  that is OpenMW's choice, not a measured vanilla behaviour.

## Tests written

- `tools/test/cases/openmw-spec-world.txt` (existing kinds and tokens only):
  - calendar: random date, `HOUR`, `SLEEP:h:2`, then `EXPECT:global:day|month|year|dayspassed` and `hour`; plus the
    set rules (day past month end, month 12+, month set with a day beyond its length, hour past 24 set directly).
  - timescale: `SETGLOBAL:timescale`, a 6 s wait, hour moved by `6 x timescale / 3600` within 25 % (wall clock).
  - disposition: `refdisp` as a delta against a snapshot, so the player's unknown race term cancels. Personality,
    `JOIN` of the NPC's own faction / one other / two others, `SETBOUNTY` (zero effect today, `fDispCrimeMod` is 0).
  - soul trap: random real creature (soul value from `game_actors_*.json`) and a random set of gems with stack
    sizes; expects the smallest fitting empty gem holds the soul, the others stay empty, counts unchanged.
  - followers: a placed actor `FOLLOW`s the player at 100..1500 units, `DOORTO` the Census Office; expects it is in
    that cell exactly when within 800.
  - journal index: `JOURNAL` sets, `EXPECT:journal` reads it back.
- `tools/test/cases/openmw-spec-world-hooks.txt` (needs hooks below):
  - leveled lists: `levcandidates`, `levpick` over every real list, level 1..60 and the chance-none boundary,
    plus `levrolls` for the each flag.
  - `JOURNALADD` / `SETJOURNALINDEX` sequences against a model of Journal / SetJournalIndex (index, lines,
    finished).
  - respawn numbers (`respawninterval`, `corpsedelay`, `goldresetdelay`) and a corpse that must still be there at 71 h
    and gone at 72 and 77 h after a cell reload.
  - stack counts (a gem split by a soul, restack of the same soul, gold always one stack), enable / disable.

## Hooks needed in the engine

| Hook | Args | Returns |
|---|---|---|
| `EXPECT:levcandidates` | `list,level` | number of entries a roll can land on (after the level / all-levels filter) |
| `EXPECT:levpick` | `list,level,rollNone,idx` | position in the list of the idx-th candidate (modulo count), -1 when rollNone < chance-none or no candidates |
| `EXPECT:levrolls` | `list,count` | how many separate rolls a top-level line of that count makes (count when each flag and count > 1, else 1) |
| `JOURNALADD:quest:index` | setup token | the script `Journal` command |
| `SETJOURNALINDEX:quest:index` | setup token | the script `SetJournalIndex` |
| `EXPECT:journalentries`, `questfinished` | exist | check they count only non-empty lines / report the flag of `Journal` |
| `ADVANCE:hours` | setup token | the same as sleep with no rest effects, any size (for 2880 h) |
| `EXPECT:respawninterval`, `corpsedelay`, `goldresetdelay` | none | hours, from the GMSTs |
| `EXPECT:refexists` | `ref` | 1 if the reference still exists (not deleted) |
| `EXPECT:stackcount` | `item id` | the number of separate inventory stacks of that id |
| `EXPECT:soulcapacity` | `gem id` | `gem value x fSoulgemMult` |
| `DISABLE:ref`, `ENABLE:ref`, `EXPECT:refenabled:ref` | tokens / kind | the script functions and the flag |
| `EXPECT:refstock` (not generated) | `npc,item` | the merchant's count of an item, to test the negative-count restock: buy n, count unchanged |
| `EXPECT:merchantgold` (not generated) | `npc` | the merchant's gold pool, to test the 24 h reset on opening dialogue |
| `EXPECT:followset` (not generated) | `dist,stayoutside,toExterior` | 1 if the rule takes the follower (pure rule form of the 800-unit test) |

## Open questions

- Our converter exports no *finished* or *restart* flag for any journal entry (0 of 620 quests in
  `game_journal_*.json`); vanilla has many. Either the reader loses `QSTF` / `QSTR` or the export key is wrong. Until
  that is fixed `questfinished` is always 0 and the finished checks in the hook test are vacuous.
- `JOURNAL:quest:index` is `setJournal` (a set, raise-only in chain mode); whether it also writes a line / the
  finished flag is the engine's choice. The script command is what `JOURNALADD` is for.
- Creature list flag bit: `levelledlist.cpp` says the item and creature flags are swapped. The OpenMW record
  header was not in the checkout (`components/esm3` absent), so the exporter's `flags & 1` meaning "all levels"
  for *item* lists was not checked against it; creature lists in `leveled` carry only `all`.
- `getActorsFollowing` (what counts as a follower: follow only, or escort too) and `CellStore::respawn`'s caller
  (every cell load?) were outside the list and not read. `Creature::respawn` / `Npc::respawn` (actors with the
  Respawn flag coming back, `fCorpseRespawnDelay`) likewise.
- Day-of-week rule and `setDisposition` / `modDisposition` script semantics (base vs temporary, clamping) are unread.
- The harness's `SLEEP:h:2` may add effects (healing, a time cap per rest); if so use `ADVANCE`.
- Prior audit rows touched: I10 (AddItem stacks onto an equipped stack; OpenMW never does), I11 (scripted items stack),
  I12 (leveled id in AddItem), I15 / M11 (HasSoulGem, RemoveSoulGem always the player's), S18 (GetDisposition lacks
  the weapon-drawn and crime terms; disease and Charm), J1-J3 (journal dedup on index, "journal updated" with empty
  text, finished flag), J7. These are all covered by the rules above; none were re-derived.

## Mismatches

Fix pass (read OpenMW's files named in the Rules section against ours; nothing run, it compiles). "Data" = needs a rebuild
of out\world before the emulator shows it.

| Rule | Our behaviour | Cause | Status | File |
|---|---|---|---|---|
| Journal finished / restart / quest names | 0 of 620 quests had a finished or restart flag, and no title | Morrowind.esm has no QSTN / QSTF / QSTR at all: Tribunal.esm and Bloodmoon.esm carry them, replacing the vanilla entries by INAM (725 finished, 3 restart, 558 titles) | fixed (data) | tools/convert/build_game.py `plugin_journal_infos`, `build_journals` |
| Timescale | the TimeScale global was ignored (read as a GMST, always 30) | `gmstf("timescale")` | fixed | source/world.cpp `timescale()`, rest.cpp, magic.cpp |
| Calendar | Day / Month / Year / DaysPassed were recomputed from the start date each frame, so a script's `Set day / month / year / dayspassed` was overwritten; `GameHour` past 24 dropped the days | `World::date()` derived, `syncTime` rewrote | fixed: the globals are the calendar, time moving on advances them, sets follow OpenMW's setters (day wraps, month clamps and carries years, hour past 24 moves the day not DaysPassed) | source/world.cpp `setGlobal`, `syncTime`, script.cpp, main.cpp, save.cpp |
| Gold of other sizes | `AddItem gold_100` made a gold_100 stack (the script path mapped it, the harness / direct adds did not) | mapping lived in script.cpp only | fixed: `addItem` maps gold_005/010/025/100 to gold_001 | source/world.cpp |
| Soul trap restack | a filled gem never joined a stack of the same soul (stack counts grew with each soul) | `trapSoul` / `pickUp` pushed a new item | fixed (`addStack`) | source/world.cpp |
| Soul gem capacity | Azura's Star held any soul | special case | fixed: value x fSoulgemMult for every gem | source/world.cpp |
| Corpse clearing | corpses lay for ever | only respawning actors were looked at | fixed: dead, non-persistent, 72 h (`>=`, as `<=`) -> deleted on the next cell load; respawners come back after min(respawn, clear) delay | source/world.cpp `respawnCell`, `deleteRef` |
| Respawn timing | actors were only looked at inside the 2880 h container gate; the gate used `>=` and began at the first load | one block | fixed: actors every load; containers when `now - last > 2880` with the stamp starting at day 1, hour 0 | source/world.cpp |
| Followers through doors | everything within 2048 (2D) came, escorts too, hostile ones too, `stayoutside` ignored | simple radius | fixed: follow package / summons only, 800 (3D), `stayoutside`, a follower fighting the player stops and stays | source/session.cpp `finishTravel`, include/world.h `followerTaken` |
| Disposition | no weapon-drawn term; Charm raised the base disposition for good | missing term; effect applied as a change | fixed: fDispWeaponDrawn when the weapon is out; Charm is a timed effect read by `disposition()` | source/world.cpp, magic.cpp, session.cpp |
| Disposition crime term | `modCrimeDispositionModifier` (set by witnessed crime) not modelled | crime area | open (crime reaction, outside this pass) | - |
| Restock quantity | negative counts lost their sign (fill took abs; the converter did `abs` for NPC lines): a merchant ran out | `fillContents`, `npc_items` | fixed: sign kept, shown as `|count|`, a sale never depletes it, stacking keeps the sign, loot takes `|count|`; Data for NPC inventories | source/world.cpp, screens_dialogue.cpp, screens_items.cpp, script.cpp, tools/convert/npcstats.py |
| Merchant gold reset | reset on opening Barter, and every trade re-armed the 24 h timer (a busy merchant never refilled) | `lastBarter` set on each trade | fixed: on opening dialogue, timer set only by a reset | source/dialogue.cpp, world.cpp `restockGold`, screens_dialogue.cpp |
| Leveled pick / each flag | same rule | - | matches (candidate rule shared by `pickLeveled` and the `levpick` hook; item `all` = flag 1, `each` = flag 2, creature `all` = flag 1 as the exporter does) | source/game.cpp |
| Journal / SetJournalIndex | same rules (dedup on heard lines, index only up, finished / restart, empty text writes nothing) | - | matches | source/world.cpp |
| Journal with an index the quest has no entry for | OpenMW throws (script error); ours moves the index up and goes on | kept running | differs on purpose: a vanilla script naming a missing stage must not abort | source/world.cpp `journalAdd` |
| Scripted items stack | scripted items stack (one script per item id) | per-id script instances, memory | differs on purpose | source/world.cpp `addItem` |
| NPC leveled inventory lines | resolved once when the data is built (seeded), not rolled at fill time | converter `npcstats.bind` | differs on purpose: data size; open if exact rolls are wanted | tools/convert/npcstats.py |
| Persistent actors | corpse clearing spares essential and scripted actors | the record's Persistent flag is not exported | differs on purpose (needs the flag in the actor export to be exact) | source/world.cpp `respawnCell` |
| Respawn on cell load | runs for every cell that loads, OpenMW only when the player enters (not for neighbours streaming in) | no such distinction | differs on purpose, harmless | source/world.cpp |
| Followers of followers | not taken | one level | open (OpenMW recurses) | source/session.cpp |
| Day of week | not read: OpenMW's `apps/openmw` has no such command | - | n/a | - |
| Quest list shows finished quests | the journal's quest list does not hide finished ones | UI | open (3DS layout kept) | - |

Resolved questions: the level-list flag bits (items: 1 all levels, 2 each; creatures: 1 all levels, as the exporter has
them); `getActorsFollowing` is the follow package only (`followTargetThroughDoors` is set by AiFollow alone) and
recurses; `CellStore::respawn` runs from `Scene::loadCell` when the cell is entered; `Creature::respawn` uses
min(fCorpseRespawnDelay, fCorpseClearDelay) (or the clear delay for a deleted one), `<=`.

## Hooks added (source/testdrive.cpp; JOURNALADD ... DISABLE also in the verb list of source/main.cpp)

`EXPECT:levcandidates | levpick | levrolls | respawninterval | corpsedelay | goldresetdelay | soulcapacity | stackcount |
refexists | refenabled | refstock | merchantgold | followset`, setup `JOURNALADD`, `SETJOURNALINDEX`, `ADVANCE`, `ENABLE`,
`DISABLE`. `refexists` looks at the creature the test placed (`testPlaced`, still known after it was cleared), because
the level has hundreds of mudcrabs. The generator now writes ADVANCE for the calendar test (a SLEEP of 72 h needs
288 frames) and skips lists whose id holds `+ : , %`.

## Next emulator batch

Rebuild out\world (journal flags and titles, NPC restock signs), then `python tools/test/specgen_world.py` (the journal checks
read the flags from game_journal_*.json), then run `openmw-spec-world` and `openmw-spec-world-hooks`.
