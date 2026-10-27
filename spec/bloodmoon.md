# Bloodmoon (the optional add-on)

Bloodmoon.esm comes in only when the data is converted with the add-on (`level.py --bloodmoon`, or
`MW3DS_BLOODMOON=1` for every converter), on its own or with Tribunal (`--tribunal --bloodmoon`, the Game of the Year
set the tests run on: `out/tb`). Without it the data is the vanilla or Tribunal game's, byte for byte as before, and
the engine behaves as before: everything below reads what the data says (`game.json`'s `"addons"`), never a build
switch. This page is a skeleton: the sections below fill in as the work lands (the plan's phases).

OpenMW's code: `apps/openmw/mwmechanics/mechanicsmanagerimp.cpp` (setWerewolf), `mwworld/player.cpp`,
`mwmechanics/npcstats.cpp`, `mwmechanics/combat.cpp`, `mwworld/weather.cpp`, `mwrender/sky.cpp`, and the script
functions in [scripting.md](scripting.md).

## Loading Bloodmoon.esm

| Rule | Ours | Code |
|---|---|---|
| Bloodmoon.esm loads after Morrowind.esm, after Tribunal.esm when that is loaded too, and before any plugin; its records replace earlier ones by type and id, INFOs merge in their topic's chain, as OpenMW merges a load order | same | `tools/convert/mwfiles.py` `load_order`, `read_game` |
| Bloodmoon after Tribunal, never before: both issue the add-ons' 72 settings (the werewolf's, the companion share's, the summons'), 44 with different values, and Bloodmoon's are the ones the game was played with (Morrowind.ini's GameFile0-2 order) | fixed order; OpenMW takes whatever order its content list gives | `mwfiles.load_order` |
| A plugin needing Bloodmoon.esm is allowed only with the add-on | same | `mwfiles.out_of_scope` |
| Bloodmoon.bsa after Morrowind.bsa and Tribunal.bsa; its sounds, voices and dream videos are loose files in Data Files | same (the archive list was already all three) | `mwfiles.Archives` |
| The converted data says which add-ons it holds | `game.json` `"addons": ["tribunal", "bloodmoon"]` (load order) | `build_game.py`, `GameData::addons`, `GameData::bloodmoon()` |
| Converter caches | keyed by the load order's files (`content_key`): a Bloodmoon conversion has caches of its own | `mwfiles.content_key` |

## Start script (SSCR)

`BMStartUpScript` runs through the start-script path Tribunal's use ([tribunal.md](tribunal.md) "Start scripts"):
once in a game, recorded in the save (`start_scripts_done`). It starts WereCheckScript and bloodmoonScript (they run
from then on), starts dbFixScript while Tribunal's TR_DBAttack is under 30 (0 without Tribunal: harmless), disables
some forty people and things by journal state (the colony guards, the smugglers, the Wild Hunt's aspects, the stones'
parts, the steam vents, the Caenlorn wolves, Karstaag's open door), and stops itself. Its disables of objects in cells
not yet read wait for the cell (`World::deferEnable`) and are kept in a save. Test: `bm-start-script`.

## Werewolf

The state, from the scripts (OpenMW's NpcStats): the player's (`PlayerStats::werewolf`, `werewolfKills`,
`werewolfAcrobatics`) and a person's (`Ref::werewolf`), set by BecomeWerewolf / UndoWerewolf through
`World::setWerewolf` (`mwmechanics/mechanicsmanagerimp.cpp`: OpenMW's setWerewolf), all saved; Resurrect clears a
person's. The test driver's `WEREWOLF:1` is the same change.

The player's form (OpenMW's setWerewolf, Player::setWerewolfStats and the rules that read isWerewolf):

| Rule | Where |
|---|---|
| Every attribute and skill but Acrobatics at its `fWereWolf<name>` setting over whatever raised or lowered it; Acrobatics too once SetWerewolfAcrobatics asks; health x `fWereWolfHealth`, filled on the change; the human values come back from the base on UndoWerewolf | `mwworld/player.cpp recomputeStats` |
| Everything worn and held taken off on the change | `World::setWerewolf` |
| Using anything but a door is refused (`sWerewolfRefusal`) with a `Wolf*` sound by its kind (NPC, creature, container, activator, item: one of the numbered set at random); a script's OnActivate still runs first | `mwworld/action.cpp werewolfRefused`, `Session::playRandomSound` |
| No inventory or magic screen, no casting, no quick keys (differs, 3DS UI: OpenMW hides the two windows; our Items and Magic buttons say so) | `mwgui/windowmanagerimp.cpp openScreen`, `mwbase/session.cpp` |
| Waiting only, no sleep (`sWerewolfRestMessage`) | `mwgui/waitdialog.cpp` |
| The HUD's weapon box with nothing held shows the claw (`icons\k\tx_werewolf_hand.dds`, OpenMW's unsetSelectedWeapon); in their own form the 3DS's "Fist" stays (3DS UI, kept) | `Session::hudHandArt` (case `bug-werewolf-hud-hand`) |
| ForceGreeting ignored unless the speaker's script has a local `allowwerewolfforcegreeting` | `mwscript/dialogueextensions.cpp` |
| Claws: hand to hand as health damage x the `WerewolfClawMult` global (25 when the data lacks it) x `fHandtoHandHealthPer`, with WolfHit, for the player and for a werewolf person | `World::werewolfClaw`; `character.cpp`, `combat.cpp`, `actors.cpp` |
| A silver weapon on a werewolf: x `fWereWolfSilverWeaponDamageMult` (WEAP flag 2) | `World::silverOnWerewolf` (case `bm-werewolf-silver`) |
| A person killed in wolf form counts toward GetWerewolfKills (people only, as OpenMW's Npc::onHit) | `actors.cpp killNpc` |
| Running with claws put away x `fWereWolfRunMult`; encumbrance 0 | `mwworld/player.cpp` (case `bm-werewolf-run`) |
| A werewolf person swings from `fCombatDistanceWerewolfMod` farther (OpenMW's getMaxAttackDistance) | `actorMeleeReach` (case `bm-werewolf-reach`) |
| A werewolf person, aimed at, shows as `sWerewolfPopup` (OpenMW's Npc::getName) | `Session::targetLabel` (case `bm-werewolf-popup`) |
| People's fight term + `iWerewolfFightMod` against the player in wolf form or once `PCKnownWerewolf` is 1; people under `iWereWolfLevelToAttack` rate fleeing at `iWereWolfFleeMod` | `mwmechanics/formulas.cpp` (case `bm-werewolf-reactions`) |
| Seen changing (a person within `fAlarmRadius`, in sight, who notices): `sWerewolfAlarmMessage`, `PCKnownWerewolf` 1, bounty + `iWereWolfBounty` if any of them has Alarm above 0 | `Session::werewolfWitnesses` (case `bm-werewolf-reactions`) |
| Attacking or killing a werewolf person is no crime; guards don't come to arrest a werewolf | `character.cpp`, `combat.cpp`, `actors.cpp`, `guardCheck` |

Drawing: a werewolf person is drawn as the `bm_werewolf_default` creature's model (the converter makes it placeable:
`level.py`), their own model hidden and shown again on UndoWerewolf (`World::werewolfBody`, again when their cell loads);
the player's first-person view takes the werewolf's own skeleton and skin (`firstperson.py WEREWOLF_1ST`,
`ViewModel::rebuild`), the world seen at `General_Werewolf_FOV` (Morrowind.ini [General] Werewolf FOV, 100 degrees
up and down; the arms keep 60: case `bug-werewolf-fov`); the fur overlay over the top screen in first person, the HUD
hidden or not (`artassets.py`, `Session::drawWerewolfView`); in third person the player's body is the library's werewolf
(`PlayerBody::rebuildWolf`, case `bug-werewolf-body`). A person changing keeps their own light on the wolf's body
(`World::werewolfBody`, case `bug-spawned-light`), and anyone placed while playing (PlaceAtPC, summons, leveled spawns)
is lit as a placement there is: the cell's light without lamps, which the converter writes into its `.act` (version
5), the lamps near where it stands added at run time (case `bug-spawned-light`; the library's even light stood out
dark). Rieklings' skin (`tx_bm_iceminion_01`) is see-through almost everywhere, for their blended fur, the body drawn
solid with it: the texture converter keeps a see-through texel's colour (Pillow's resize and tex3ds blackened them;
case `bug-riekling-black`). Not done: the unseen WerewolfRobe OpenMW puts on (nothing shows it).

## Script functions

All 16 that Bloodmoon's scripts and dialogue call, the whole Get / Set / Mod magic effect family (72) and the dialogue
functions 72 / 73: [scripting.md](scripting.md) "Bloodmoon's functions". `script_compile_check.py` over Morrowind +
Tribunal + Bloodmoon: 0 differences; `script_gap.py`: nothing Bloodmoon uses is missing. plantScript's `<==` reads as
`<=` (OpenMW's scanner). The spriggans' three lives (Resurrect, SetScale, ModHealth, ModResist*): `bm-spriggan`.

Note: plantScript compiles only in data converted after this change (its `game.json`); the engine's console compiler
reads `<==` already.

## Weather and sky

| Rule | Ours | Code |
|---|---|---|
| Snow falls: snow.nif's flakes (Tx_BM_Snowflakes_01) at 60 a second (Weather_Precip_Gravity 575 x Snow Gravity Scale 0.1), hardly drifting; no ambient loop | sheets of the original's specks in the box round the eye, 240 on a New 3DS and 120 on an Old one ([performance-tradeoffs.md](performance-tradeoffs.md)); lit as snow.nif's material (emissive 0.3, the weather's ambient and sun), not by the baked light's tint | `mwrender/sky.cpp drawPrecipitation`, `artassets.py snow_sheet` |
| A blizzard: blizzard.nif's flakes blown along the wind (210 a second, tilted 70 degrees, gravity 180) and its clouds (Tx_BM_Blizzard_01, 300 a second); its loop "BM Blizzard"; Land Fog Depth 2.8 | the flakes at about 250 across the ground and 150 down, the ash storm's 24 dust clouds in its picture at half strength, the loop from `game.json`, the view at the fog scale's quarter | as above, `Session::updateWeatherEffects`, `World::weatherFogScale` |
| Which way a storm blows (OpenMW's calculateStormDirection): ash and blight out from Red Mountain, every other weather along the default direction (north); walking into a storm is slower (fStromWalkMult) | same: a blizzard blows north (it blew from Red Mountain, 500 km off) for the flakes and the walking | `World::stormDirection`, `mwworld/player.cpp` |
| The red moon: TurnMoonRed draws Secunda in [Moons] Script Color (255, 20, 20) until TurnMoonWhite | same, and kept in a save (OpenMW forgets it on a load: bloodmoonScript turns it red only once, so the moon went white for good); white in a new game; the colour from Morrowind.ini (`sky_bodies.script_color`, data converted after this change), else OpenMW's fallback 255, 20, 20 | `mwscript/skyextensions.cpp`, `mwrender/sky.cpp updateSky` |
| fMagicDetectRefreshRate (Bloodmoon: 0.5) | not read: OpenMW doesn't read it either; our Detect dots are found each time the map is drawn | `mwgui/mapwindow.cpp drawDetected` |

Tests: `bm-weather-snow`, `bm-weather-blizzard`, `bm-o3ds-snow` (`# console: old`), `bm-moon-red`. Side by side with
OpenMW in the Moesring Mountains (internal/SBS-PROCESS.md): snow close (flakes a little smaller and fewer in the sky:
the 240-line screen, kept); the blizzard's fog thicker than OpenMW's view (the agreed fog scale, kept).

## Not done (yet)

- The werewolf seen from outside as the player (third person, the face preview): still the human body.
- `Werewolf FOV` (100): the view keeps its angle (the stereo screen was tuned for it).
- Kills by damage spells don't count yet (OpenMW counts them); WolfSwing, WolfEquip and the WolfRun loop aren't played.
- Map notes are on with Tribunal only (`GameData::tribunal()` in `mwgui/mapwindow.cpp`); Bloodmoon alone does not turn
  them on.
