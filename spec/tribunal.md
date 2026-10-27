# Tribunal (the optional add-on)

Tribunal.esm comes in only when the data is converted with the add-on (`level.py --tribunal`, or `MW3DS_TRIBUNAL=1`
for every converter). Without it the data is the vanilla game's, byte for byte as before, and the engine behaves as
before: everything below reads what the data says (`game.json`'s `"addons"`, cells' `"sky"`, `"start_scripts"`),
never a build switch. Bloodmoon is the other add-on ([bloodmoon.md](bloodmoon.md)); it loads after Tribunal.

OpenMW's code: `apps/openmw/mwworld/{worldimp,cellstore,scene}.cpp` (QuasiEx), `mwscript/globalscripts.cpp`
(addStartup), `mwgui/companionwindow.cpp`, `mwgui/companionitemmodel.cpp`, `mwgui/dialogue.cpp` (isCompanion),
`mwmechanics/summoning.cpp`, `mwmechanics/spelleffects.cpp` (shouldRemoveEffect), and the script functions in
[scripting.md](scripting.md).

## Loading Tribunal.esm

| Rule | Ours | Code |
|---|---|---|
| Tribunal.esm loads after Morrowind.esm and before any plugin; its records replace Morrowind's by type and id, INFOs merge in their topic's chain, as OpenMW merges a load order | same | `tools/convert/mwfiles.py` `load_order`, `read_game` |
| A plugin needing Tribunal.esm is allowed only with the add-on (Bloodmoon.esm likewise with its own) | same | `mwfiles.out_of_scope` |
| The converted data says which add-ons it holds | `game.json` `"addons": ["tribunal"]` | `build_game.py`, `GameData::addons` |

## Mournhold: interiors under the sky (CELL flag 0x80, OpenMW's QuasiEx)

Five cells: Godsreach, the Great Bazaar, the Plaza Brindisi Dorom, the Royal Palace's courtyard, the Temple
courtyard. They are interiors (GetInterior 1, their own map, no distant land, torches by the cell's ambient as
OpenMW's useTorches) that take the outdoors' sky:

| Rule | Ours | Code |
|---|---|---|
| The sky, sun and moons are drawn | the converter gives the cell the exteriors' sky dome; `LevelCell::underSky()` | `convert_cell.py` `interior(weather=)`, `mwrender/sky.cpp` |
| Lit by the weather and the hour, not the cell's ambient (it has none) | baked by the level's weather with the exteriors' sun direction, lamps kept apart so the night darkens only the sun's share | `convert_cell.py`, `engine.cpp` `rendererSetDaylight` |
| The region's weather (Mournhold Region), wind, storms, rain | `World::underSky()` in the weather | `mwworld/weather.cpp` |
| Region ambient sounds, sun damage, storm walking, the head-tracking reach | as outdoors | `mwsound/soundmanagerimp.cpp`, `mwmechanics/activespells.cpp`, `mwworld/player.cpp`, `mwmechanics/combat.cpp` |
| Dialogue's weather condition | asked there (OpenMW; the original game doesn't: OpenMW calls that a bug) | `mwdialogue/filter.cpp` |
| The weather's fog over the outdoor view, closing in with thick weather as outdoors (an ash storm draws 0.63 of it) | `objects.cpp` (the exteriors' fog range), `World::weatherFogScale`, `engine.cpp` | `bug-mournhold-weather-fog`, `bug-mournhold-storm-fog` |

## Start scripts (SSCR)

A new game starts the data's start scripts, as OpenMW's `addStartup` (Tribunal's: TribunalMain, MarkTRStartScript,
TribFabAttackCheck). A save made before the add-on gets each once when it is first loaded with it; the scripts run
are kept in the save (`start_scripts_done`). TribunalMain turns levitation (and in Sotha Sil's, teleporting) off and
on by the cell; MarkTRStartScript starts dbAttackScript (the Dark Brotherhood's attacks when the player sleeps) and
stops itself. Code: `Session::startGameScripts`, `GameData::startScripts`.

## Companion share

| Rule | Ours | Code |
|---|---|---|
| Share (sCompanionShare) is offered by someone whose script's local `companion` is not 0; it is never refused | a dialogue button | `mwgui/dialogue.cpp`, `Session::isCompanion` |
| Their things and the player's, both ways, no price, no theft | the container screen in its share mode | `mwgui/container.cpp` |
| With a `minimumprofit` local: what is given adds its value x count, what is taken takes it off; shown as "Profit:" (sProfitValue) | same | `companionProfit` |
| Leaving with the profit below 0: the warning (sCompanionWarningMessage); "quit" closes the share and the conversation, the other button goes back | same | `Session::companionShareClose` |
| Layout | the 3DS's container screen | differs (3DS UI, kept) |

## Magic

| Rule | Ours | Code |
|---|---|---|
| Effect 137, Summon Fabricant: calls Fabricant_summon (sMagicFabricantID) | same | `mwmechanics/spelleffects.cpp` `summonCreature` |
| DisableLevitation ends every Levitate at once (the player told sLevitateDisabled); a new one too | same | [scripting.md](scripting.md) |
| An ability's or a worn constant enchantment's Restore Health / Magicka / Fatigue works every second, as a spell's (OpenMW keeps an ability as a spell that never ends): the Imperfect's Rebuild, Gedna Relvel's Refill, a constant Restore Health ring (vanilla too) | same | `mwmechanics/activespells.cpp` (the player), `mwmechanics/combat.cpp` (actors: their `const_effects`) |
| An ability's or a worn constant enchantment's Fortify Health / Magicka / Fatigue raises the maximum while it lasts: Her Reflected Glory, Maras's Skirt, the Hortator's Belt (vanilla too) | same | `mwworld/player.cpp` `recomputeStats` |

Every spell, ability, disease and power Tribunal adds, every enchantment on an item, every creature and every armour
and weapon has a check in `trib-spells`, `trib-enchantments`, `trib-creatures` and `trib-gear`, written by
`tools/test/specgen_tribunal.py` from Tribunal.esm and the converted data. Records nothing in the game reaches (no
item carries them, nothing places them: Master Fire / Shock Trap, Her Tamed Lightning, King's Oath (the guards'),
the Spite Dart, the Goblin Officer, two Centurion and Fabricant variants) get a note instead.

## The Clockwork City's doors

Sotha Sil's doors (act_sotha_pre_door*, act_sotha_door*, act_sotha_mach_door*) are activators a lever's script opens
and shuts with PlayGroup "idle2" / "idle3"; their collision is animated (keyframed nodes under the RootCollisionNode).
OpenMW moves those collision shapes with the animation every frame (`mwphysics/object.cpp animateCollisionShapes`).

| Rule | Ours | Code |
|---|---|---|
| Which objects | activators a script plays a group on (not a death group) whose model's collision is animated, found by the converter | `particles.py animated_collision_objects` |
| Drawn | animated, as their world piece (one piece a model), at rest until a group is played | `particles.py build_pieces`, `mwrender/effectmanager.cpp updateWorldFx` |
| Collision | the rest pose's, baked; the moving part kept apart. Gone once the opening has carried every moving part more than half its way (the pre-doors: 7.1 s of 9.3), back once the closing has brought them more than half their way home (8.0 s of 9.3). The frame stays solid. Saved (`colgone`); a door open in a save shows open | `particles.py collision_moves`, `convert_cell.py`, `Session::playObjectGroup`, `World::regainCollision` |

Differs on purpose: switched at two points instead of followed frame by frame (moving collision costs the console
more; see open question 5 about the traps, which this does not touch). Tests: `trib-sotha-doors`, `trib-sotha-doors-shut`.

Sotha Sil's steam vents (act_sotha_fabsteam, act_sotha_steam00 / 10) are animated activators too: their sprays run on
the group's clock, as OpenMW runs an animated object's particle controllers (`Animation::addAnimSource`), so they show
nothing at rest and steam once in the Idle2 a script plays (the Imperfect as it wakes, the Dome of Serlyn's machine
doors). Drawn as world particle pieces with their groups (`particles.py WORLD_FX`, `scripted_group_objects`). Tests:
`trib-imperfect-steam`, `trib-mournhold-scripted`.

## Globals

A short or long global's stored float is read as OpenMW's ESM::Variant reads it: cut to a whole number, and one out
of an int's range is the int's lowest (as a short: 0). Tribunal.esm stores leftover bytes in several (MuseumDonations
7.6e23, GobChiefDead, KarrodFightStart, KingHit, ShrineCleanse, MournWeather), Morrowind.esm a few (RedoranMurdered,
MT_WritDiscount, TGGlove, WearingHelmHHDa): all 0, as in OpenMW. Torasa Aram's shield now waits for two donations.
Code: `components/esm3/loadglob.hpp globalStoredValue` (the data and saves), `build_game.py`. Test: `trib-museum-donations`.

## The journal

Tribunal's quests show their titles (QSTN) and finished flags as Morrowind's do. The 3DS journal's questline filter
gains "Tribunal" (TR_ / TR0x_) and "Mournhold" (MS_, Merc_): `mwgui/journalwindow.cpp` (3DS UI).

## Map notes

The player's own notes on the map (OpenMW's custom markers; Tribunal's sEditNote / sDeleteNote), with the add-on
only. On the map screen (3DS UI): Y writes a note at the view's centre (its cross) on the system keyboard; a tap on a
note's red marker shows its text; X then edits it (emptied, it goes) and Y asks "Delete Note?" (Yes takes it away).
An interior's notes show on its local map, the outdoors' on the outdoor local map and the world map. Kept in the save
(`map_notes`). Code: `mwgui/mapwindow.cpp` `Session::mapNotesUi`, `World::mapNotes`. Test: `trib-map-notes`.

## Not done (yet)

- Barilzar's Heavy Eyes (Blind and Burden on a target): the combat AI casts harm, healing, summons and wards only
  (`npcstats.combat_spells`), OpenMW's spellpriority rates Blind and Burden too. A vanilla rule as much as Tribunal's.
- A creature's leveled weapon (the goblins' goblin_weapons_random) is its list's lowest entry for good
  (`build_game.resolve_leveled_item`), never rolled by the player's level: the goblins never carry the Goblin Club.
- Barter's "Seller Max" button (sMaxSale): the 3DS barter screen haggles item by item (3DS UI).
