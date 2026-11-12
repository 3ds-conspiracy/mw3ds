# Golden runs with Tribunal and Bloodmoon

The golden tools of `golden-actors.md` and `golden-behaviour.md`, run with the add-ons loaded: `--tribunal --bloodmoon`
on `golden_openmw.py`, `golden_behave.py`, `golden_compare.py` and `golden_tour_compare.py` (OpenMW loads Tribunal.esm and
Bloodmoon.esm after Morrowind.esm, the converters' order; their files are named after the add-ons, so the base game's
results stay as they are). The engine's side runs on the Tribunal + Bloodmoon data (`out/data-tribunal-bloodmoon`).

```sh
python tools/test/golden/golden_openmw.py actors --tribunal --bloodmoon
python tools/test/golden/golden_compare.py <topic> --tribunal --bloodmoon
python tools/test/golden/golden_behave.py tour --tribunal --bloodmoon --addon-cells
python tools/test/golden/golden_behave.py case --tribunal --bloodmoon --addon-cells
python tools/test/run_test.py scratch/golden-tour-tribunal-bloodmoon --native --data out/data-tribunal-bloodmoon --sd build/native-golden-tb/sd
python tools/test/golden/golden_tour_compare.py --tribunal --bloodmoon --log build/native-golden-tb/sd/3ds/mw3ds/log.txt
```

`--addon-cells` tours only the cells the add-ons' own files hold: the 379 they add or change (Mournhold, Solstheim,
the docks and forts they touch). Without it the tour is all 2,884.

## First run (2026-11-11): records, and the 379 add-on cells

Clean: every object of the 379 cells (present, place, enabled, scale, count, lock, trap, owner), items, alchemy,
birthsigns, factions, enchantments, game settings, world records. OpenMW's tour reached all 379 cells, none stuck.

## Mismatches

| # | What | Evidence | Status |
|---|---|---|---|
| A1 | The add-ons' summons (Summon Fabricant 137, Bloodmoon's Call Wolf, Call Bear, Summon Bonewolf 138-140, and 141-142) had a magnitude and Touch / Target: OpenMW's HardcodedFlags give them 0x1048 as Summon Scamp's (applied once, Self, no magnitude) | `golden_compare.py effects`: 4 differ. Call Wolf's HUD tooltip shows "1 pts" | test `bug-addon-summon-magnitude` (fails on the old data); fix in `build_game.py` hardcoded_flags: the flags now 0x1048 as OpenMW's; passes natively on the rebuilt data |
| A2 | An NPC with stored stats keeps its record's fatigue: OpenMW's `Npc::ensureCustomData` sets it after the attributes (each of which works out the sum on the way). G3 of `golden-actors.md` made it the sum for every NPC; the base game's editor stored the sum for most, the add-ons' did not | Tharsten Heart-Fang 800, ours 391; Bels Uvenim 325 / 355; base game Crazy_Batou 290 / 311, Goris 255 / 243. With the fix 1,941 of 1,942 NPCs without a script agree (Galmis Dren left, as before) | tests `bug-npc-stored-fatigue`, `bug-addon-stored-fatigue` (fail on the old data); fix in `npcstats.py`, checked against OpenMW's numbers; `bug-addon-stored-fatigue` passes natively on the rebuilt add-on data (`bug-npc-stored-fatigue` too, on that data; `out/world` not yet rebuilt) |
| A3 | Those siding with an actor that fights another actor (StartCombat on it) take up the fight: OpenMW's `engageCombat` runs for every pair of actors in range, ours only against the player | Bamz-Amschend, Hearthfire Hall: the fabricants and centurions following their leaders, OpenMW Combat on the other leader, ours Follow | test `bug-followers-join-leader-fight` (failed before, passes natively after); fix `Session::sidesInActorsFight` |

## Leads, not yet looked into

- Bloodmoon's werewolf NPCs (`bm_werewolf_wildhunt*`, `bm_werewolf_maze1`) wear the unseen `werewolfrobe` in OpenMW, nothing
  in ours. OpenMW's robe is unseen; check on screen that they are drawn as werewolves.
- Two Dark Brotherhood assassins and Severa Magia hold darts in OpenMW, a dagger or tanto in ours. Our pick follows OpenMW's
  auto-equip; they attack on sight, so OpenMW's dump may catch them in combat, where `rateWeapon` chooses. Check with a fight.
- `colonyveresa` moveonce (Veresa Alver), `galmsscript` gamble, `thendastravel` walkstate: one script variable each.
- Tymvaul's magicka 700 in OpenMW, 500 here.

## Not differences (as in `golden-behaviour.md`)

Script timers, signs' and lanterns' swing, the character generation scripts by the prison ship, walk speeds within
about 1% (leveled gear), a body's health maximum of 1, rolled leveled gear (`golden_compare.py equipment`).
