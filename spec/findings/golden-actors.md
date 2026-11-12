# Golden run: actor stats (NPCs and creatures)

Found with `tools/test/golden/` (`golden_openmw.py actors`, then `golden_compare.py actors`): a real OpenMW 0.51 makes
every NPC and creature record of `Morrowind.esm` and dumps the numbers it works out; the comparison sets them beside
the converter's actors (`out/<data>/game_actors_*.json`, what the engine runs on). The answers are derived from the
game's data and stay under `build/golden/` (git-ignored). Exact match, no tolerance.

OpenMW's numbers are read a few frames after the actor exists, so a script on the actor (the vampires' clan bonuses,
the cattle's AI settings) may already have changed them. The report keeps actors with a script apart; "no script" is
the clean evidence.

## Mismatches

| # | What | Evidence | Status |
|---|---|---|---|
| G1 | Autocalculated NPCs (the Autocalc flag: stats from race, class and level) get the wrong attributes, skills, health, and so magicka and fatigue. `autocalc()` in `tools/convert/npcstats.py` is an approximation (attributes +2 a level, flat skill growth); OpenMW's rule (`mwclass/npc.cpp` autoCalculateAttributes / autoCalculateSkills): attribute growth per attribute from the class's major / minor skills tied to it (1.0 / 0.5 / 0.2 each), skill growth `(level - 1) * (1.0 for major or minor, else 0.1; +0.5 in the class's specialization)`, health `floor((str + end) / 2) + multiplier * (level - 1)` with the multiplier 3, +2 combat / +1 stealth specialization, +1 for a favored Endurance; rounding half to even | 1223 of 1266 without a script differ (1583 of 1737 in all). A rule written from OpenMW's code reproduces OpenMW's attributes, skills and health for all 1271 autocalc NPCs without a script | fixed in the converter (`bug-npc-autocalc-stats`; after it 1270 of 1271 match, `golden_compare.py actors --esm`) |
| G2 | NPCs with stored stats: the converter drops the stored magicka (`<3H` unpack keeps health and fatigue only) and uses intelligence x 2 | 114 of 589 without a script differ (185 of 878 in all). Ababael Timsar-Dadisun: file 45, OpenMW 45, ours 180 | fixed (`bug-npc-autocalc-stats`: Hrisskar Flat-Foot) |
| G3 | Fatigue: OpenMW sets an NPC's fatigue to strength + willpower + agility + endurance (`mwmechanics/creaturestats.cpp` setAttribute); the converter keeps the stored value for NPCs with stored stats | OpenMW's value is that sum for 2596 of 2615 NPCs; 8 stored-stats NPCs differ from ours (Assantushansar: OpenMW 203, ours 224) | fixed, then found too broad: only for autocalc NPCs and after an effect on one of the four. One with stored stats keeps its record's fatigue (`golden-addons.md` A2) |

| G4 | Magicka of Bretons and High Elves: OpenMW counts the race's Fortify Maximum Magicka (intelligence x (fNPCbaseMagickaMult + 0.1 x fortify), `CreatureStats::recalculateMagicka`); the engine applies no Fortify Maximum Magicka to NPCs, and the converter left it out. For a stored-stats NPC it replaces the stored magicka | found after G1 was fixed: 93 autocalc NPCs without a script (Ales Julalanie: OpenMW 210, ours 168) | fixed in the converter (Eldafire in `bug-npc-autocalc-stats`) |
| G5 | The spells an autocalc NPC picks (`mwmechanics/autocalcspell.cpp` against OpenMW's). Ours walked the candidate spells sorted by id, where Morrowind and OpenMW walk them in the data files' order (who is dropped when a school is full depends on it); replaced a full school's cheapest from that school only, where Morrowind (and OpenMW, on purpose) look among all the picks, a known vanilla bug kept; left out the area term `0.05 x max(1, area) x base cost` of the weakest-school cost; always raised a duration to 1, where applied-once effects keep 0; checked the iAutoSpellAttSkillMin floor on every effect's skill / attribute value, where only effects on a skill or an attribute (Drain / Damage / Restore / Fortify / Absorb) have one; and could pick the race's own spells | 294 of the 357 unscripted autocalc NPCs with any pick differed (`golden_compare.py spells`, `DUMPSPELLS`); after the fix 1 (Omesu Hlarys, below) | fixed in the engine and converter (`bug-npc-autocalc-spells`: Angaredhel, 7 of her 28 spells missing before) |
| G6 | Spells an NPC knows but neither sells nor casts: the converter keeps an NPC's own spell list only for spell merchants, and NPCs get none of their race's spells (Eye of Night, Argonian Breathing), where OpenMW gives every NPC its record's list and its race's | 201 of 589 stored-stats NPCs without a script know fewer spells than in OpenMW | fixed: every NPC and creature keeps its record's spell list (`build_game.py`), an NPC knows its race's (`World::actorHasSpell`), the Spells service follows the service flag (`mwgui/dialogue.cpp`; before, an autocalc NPC with picks offered it) and a merchant never sells its race's spells (`spellbuyingwindow.cpp`, as OpenMW). After: all unscripted NPCs' castable spells match; scripted ones miss only the vampire spells their scripts add (`bug-npc-race-spells`) |
| G7 | Magic effect flags (`build_game.py hardcoded_flags`, beside OpenMW's HardcodedFlags in `components/esm3/loadmgef.cpp`, as a running OpenMW reports them: `golden_compare.py effects`). 10 of 137 effects differ: Frenzy and Demoralize (Humanoid / Creature) harmful in ours, not in OpenMW (ours on purpose, for which spells fighters aim at a foe: `spellpriority.cpp`, `esmstore.cpp` combat spells); Turn Undead harmful in OpenMW, not ours; Night Eye castable on touch and target in OpenMW, self only in ours; Extra Spell touch / target in OpenMW, self in ours; Vampirism has no duration in OpenMW; Sun Damage self only and not harmful in OpenMW, harmful on touch / target in ours; Stunted Magicka no magnitude and not harmful in OpenMW | school and base cost match for all 137 | fixed: `hardcoded_flags` / HARMFUL_EFFECTS follow OpenMW's table; all 137 effects match (`golden_compare.py effects`) |
| G8 | Spells fighters keep: an actor's combat spells (`build_game.py` / `esmstore.cpp`) are only its harmful ones and heals, so creatures never cast their other spells: the Gothren guard dremora's and Fyr's dremora's Bound Shield / Bound Dagger, the Golden Saints' Dispel. OpenMW's AI casts those too (bound items with no weapon in hand, Dispel on a bad effect: `spellpriority.cpp` rateEffect) | 6 of 240 creatures (`golden_compare.py cspells`); diseases match for all | half fixed: Dispel and Cure Poison / Paralysis join the combat spells (the engine already rated them), every creature's spells match OpenMW's; still open: NPCs and creatures casting bound weapons / armour (the weapon is one mesh baked into the actor library: needs bound meshes baked as alternates, a per-actor weapon swap and its combat numbers) |
| G9 | NPC reputation: OpenMW sets every faction member's to `iAutoRepFacMod x (rank + 1) + iAutoRepLevMod x (level - 1)` (`mwclass/npc.cpp` ensureCustomData), whatever the record stores; the converter keeps the record's (`build_game.py` NPDT) | 84 of 2615 NPCs differ (Ababael Timsar-Dadisun: OpenMW 10, ours 0); faction and rank match for all (`golden_compare.py factions`) | fixed in the converter; all 2615 match (`bug-npc-reputation`, with the new `EXPECT:refrep`) |
| G10 | The weapon an NPC holds: OpenMW's auto-equip (`mwworld/inventorystore.cpp` autoEquipWeapon) takes the NPC's best weapon skill (long blade, axe, spear, short blade, marksman, blunt), its weapon of the highest max damage, then the next skill; a bow / crossbow only with ammunition, the arrow / bolt of the highest damage. Ours (`npcstats.py` combat_weapon) takes the melee weapon of the highest damage whatever the skill, a ranged one when Marksman is at least that weapon's skill, and the first ammunition found. In a fight OpenMW's AI may change weapon (`aicombataction.cpp` rateWeapon), so this is the weapon at rest | 23 of 1673 NPCs without a leveled list differ in weapon, 6 in ammunition (Edwinna Elbert: OpenMW silver dagger, ours ebony wizard's staff); worn clothes and armor match for all of them | fixed: `combat_weapon` is OpenMW's auto-equip pick; all 1673 NPCs without a leveled list match (`bug-npc-weapon-pick`) |
| G11 | A faction record listing a reaction twice: Census and Excise has Ashlanders -3 then -1. OpenMW keeps the first (-3), the converter's dict the last (-1) | 1 of 22 factions (`golden_compare.py world`) | fixed: a faction listed more than once keeps its lowest reaction (OpenMW's loadfact.cpp); was taken as the first, then found to be the lowest (Census and Excise toward itself: 0 and -1) |

After the fixes, without a script: 1270 of 1271 autocalc NPCs and 589 of 596 stored-stats NPCs match OpenMW in
everything compared.

Item records (`golden_compare.py items`): all 3972 weapons, armor, clothing, books, lights, tools, apparatus, misc
items, potions, ingredients and containers in the data match OpenMW's records (enchant capacity compared in the
record's units; a light that burns for ever is -1 there, 0 in the data).

Clean against OpenMW (`golden_compare.py`): `signs` (13 birthsigns' spells), `alchemy` (258 potions' and 95
ingredients' effects), `leveled` (116 creature leveled lists' records; at 14 player levels no creature OpenMW's own
picks gave that the engine's pick can't; the engine can give a few OpenMW's 150 samples per level didn't, all rare
entries of big lists that OpenMW gave at nearby levels).

Also clean: `world` (77 classes, 10 races, 9 regions' weather and sleep lists, 21 of 22 factions' favoured attributes / skills, reactions and rank requirements; journal: all 620 quests' stages but 4, and those 4 plus the finished / restart flags differ only because the converter takes Tribunal's and Bloodmoon's journal records for Morrowind.esm's quests, on purpose: `build_game.py` plugin_journal_infos), `magic` (all 879 spell and 573 enchantment records in the data: type, cost, charge, flags, effects), `gmst` (191 of the 192 numeric settings the engine reads match OpenMW's; fWerewolfHealth is Bloodmoon's,
absent with Morrowind.esm alone), `cells` (all 2323 cells hold the same objects but 17 near the start, where live
creatures had moved, and the divine / temple / prison markers OpenMW doesn't list; all 3127 of OpenMW's doors lead
where ours do). Shields: an idle NPC in OpenMW has none on; the converter bakes the one it takes up onto the body
(a look difference only, not compared).

## Not explained yet (do not treat as bugs)

- Omesu Hlarys (autocalc, level 17, no script) has no spells in OpenMW; ours picks five. Not looked into.

- Five stored-stats Bretons (Aditte Oges, Lanie Endre, Marielle Amedee, ...) keep their stored magicka 20 in OpenMW
  where G4 would give 102; Galmis Dren's fatigue (OpenMW 299, ours 269); two stored-stats NPCs' fatigue
  (Crazy_Batou, Goris the Maggot King).

- Creatures without a script: 25 differ in fatigue, 17 in magicka, 1 in strength (Vivec's god form 200, ours 100).
  OpenMW's creature numbers are the stored ones for 190 of 240, so these may be abilities or recalculations applied
  before the dump (Dagoth Ur's strength 200 is not in his record). Needs a dump taken before anything is applied.
- Scripted actors whose numbers a script changes (the vampires, the cattle, Galtis Serethi's greeting 300): expected.
- One NPC with a non-vampire script (Merta) differs in attributes and skills by the same 20 / 30 as the vampires.
- Heart of Akulakhan: health 5000 and no attacks in OpenMW, ours 1000 and one 1-3 attack; the Heart is scripted.

## Tests written

- `tools/test/cases/bug-npc-autocalc-stats.txt`: Eldafire's health and magicka (G1, G4), Hrisskar Flat-Foot's
  magicka (G2). Failed natively on the old data (62 / 116 / 72), written before the fix.
- `tools/test/golden/golden_compare.py actors` (`--esm`: the converter's numbers straight from the records, no
  conversion; without it, the converted data's). Needs `build/golden/openmw-actors.json`
  from `golden_openmw.py actors` (about 3 minutes, a window shows while OpenMW runs).

- `tools/test/cases/bug-npc-autocalc-spells.txt` (G5): failed on the old engine (7 of 28), passes after.
- `golden_compare.py spells`: OpenMW's spell lists beside a native run's DUMPSPELLS lines
  (`tools/test/cases/scratch/golden-spells.txt`, a step with DUMPSPELLS).

## Open questions

- Whether to take the numbers earlier (before scripts run) or to compare only actors without a script.
- Next topics: barter gold, the player's own derived pools at character creation, creatures' spells.
