# Golden tour: what every cell holds once its scripts have run

Tools: `tools/test/golden/golden_behave.py` (OpenMW's half, Lua in `mod_behave/`), the `DUMPTOUR` token (ours, native),
`tools/test/golden/golden_tour_compare.py`. The player goes to all 2,538 cells of Morrowind.esm in a fixed order, god mode
on, clock stopped. Only what involves no dice is compared.

## Setup both sides must share

- A run started with `--skip-menu` does not get the game's `Startup` script: `StartScript Startup` by `--script-run`.
  Without it 102 things that start disabled (Falura Llervu, Balyn Omavel's corpse) are enabled.
- That run's player is the `player` record's Dark Elf Acrobat with every attribute 30. `TOURSTART` gives ours the same.
  With that, disposition agrees for every NPC of the cells compared so far.
- A `.omwscripts` file takes one line per script: `NPC, CREATURE: x.lua`, not one line for each tag.
- The Imperial Prison Ship's character generation stops the game with a question box. The runner goes on from the next
  cell (`stuck` in its output).

## Not differences

Normalised by `golden_tour_compare.py`, each checked once by hand:

- A leveled spot's pick and whether it comes up empty: dice. Spawned objects are compared by their spot.
- Who has noticed the player: a roll, so a fight with the player says nothing about the package.
- OpenMW's `getRunSpeed` for a creature is its walk speed; ours follows the run animation on purpose (`spec/movement.md`,
  `bug-creature-run-speed`). `getWalkSpeed` leaves out scale and race weight, which OpenMW applies to the movement.
- A standing NPC has a Wander of distance 0 in OpenMW, no package in ours. OpenMW's Lua counts owner ranks from 1.
- `gamehour`, `random100`, and the globals OpenMW only works out for a talk (`pchascrimegold`, `pchasgolddiscount`,
  `pchasturnin`).

## Mismatches

| # | What | Evidence | Status |
|---|---|---|---|
| T1 | An NPC's load does not slow it: OpenMW's walk is Speed's x (1 - fEncumberedMoveEffect x carried / capacity) | About 600 NPCs. Imperial guard: OpenMW 156, ours 212. Athyn Sarethi 177.48 and 182 | fixed, `bug-npc-load-speed`. Small differences stay from leveled gear (trade-off) |
| T2 | A record's AI packages after the first are dropped, so someone with several Travel points finishes the first and stands | 79 records. Dan_Ru (Abebaal Egg Mine) has 3 repeating points: OpenMW Travel, ours none | engine and converter written, needs the data rebuilt, `bug-npc-travel-list` |
| T3 | `GetStartingAngle` x and y are 0 | Shipwreck crate running Float: OpenMW 11, ours 0 | fixed, `bug-float-start-angle` |
| T4 | A door or chest with a key and no lock level is locked in OpenMW; only the key, or an Open spell of any strength, opens it | 4 in the game: 2 doors in Assemanu's shrine, chests in the Ravel and Dralas tombs | fixed, `bug-key-only-lock` |
| T5 | A leveled pick runs what was placed at its spot, not its own script | Ashmelech's cattle keep flee 70 and fight 0 (OpenMW 100, 20); diseased scribs lack their disease script | fixed, `bug-levpick-script` |
| T6 | Abilities, diseases and curses on an actor did nothing to its attributes and skills, nor through them to its fatigue and magicka | 87 records and every scripted vampire. Vampire Attributes, Speed + 20: OpenMW's walk 169.58, ours 146.58. Dreamer with Wither: fatigue 140, ours 160. Dagoth Ur's Fortify Strength + 100: fatigue 500, ours 1000 | fixed, `bug-actor-standing-spells`. A rolled magnitude is taken at its middle, so an actor is the same each time: ash slaves' magicka 140 where OpenMW's run from 124 to 158 |
| T7 | AI Hello read as one byte | Galtis Serethi: OpenMW 300, ours 44 | fixed, `bug-npc-hello-two-bytes` |
| T8 | What a script does to an actor's attributes and skills (`ModSpeed`) counted only for `GetSpeed` | Mastrius, `ModSpeed -70`: OpenMW's walk 96.28, ours 168.79 | fixed, `bug-actor-script-stat` |

## Left over, not bugs

After the fixes 1,718 lines remain of 3,119. By group:

- **Timers and rolls:** script timers (0.606 against 0.7), banners' dice, `SignRotate`'s and `Float`'s `swingtime` (they do nothing beyond 2000 units from the player, and each side lands elsewhere in an exterior).
- **Leveled gear:** walk speed within about 3% for most (leveled items are picked at level 1 here, `spec/performance-tradeoffs.md`).
- **Rolled drains:** the pools above.
- **Dice in creature spots:** slaughterfish and mudcrab counts, wandering across a cell border.
- **Prison ship:** the run was stopped there, so its objects were not dumped.
- **Not explained:** Duldrar Saren's fight 100 in OpenMW and 60 here, with no script of his that sets 100.

## Side effects, in cases

Slower loaded NPCs and faster vampires moved people in some long runs. Reseeded (rule change): `uber-fighters-guild`,
`uber-bm-main-scroll`, `uber-trib-main-2`, `uber-house-redoran-seed17`, `bug-fight-shuffle`; a re-seed before the hunters in
`uber-bm-main` (that chase stays fragile: 2 of 6 seeds wedge the player among the rocks). A new spot for Ahnia's note
in the four Mournhold runs, her body now between the old spot and the note; a longer wait for Uryn Maren in `uber-bm-colony`.

Newly walking NPCs, in a 90 s visit to each of the 78 placed: 335 packages finished. One was stuck on its way (Dan_Ru, for a
moment, between two of his points) apart from a developer test cell.
