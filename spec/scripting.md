# Scripting and the console

How a script line becomes something the engine runs, the console's rules, and how a new game starts its scripts.
OpenMW's code: `components/compiler` (scanner, parsers, `extensions0.cpp`), `apps/openmw/mwgui/console.cpp`,
`apps/openmw/mwscript` (`interpretercontext.cpp`, `globalscripts.cpp`, `*extensions.cpp`), `mwworld/worldimp.cpp`.

## Two compilers, one tree

| Where | What | Code |
|---|---|---|
| PC, at conversion | every script and dialogue result of the load order | `tools/convert/mwscript.py` |
| device, at run time | one console line | `source/components/compiler/compiler.cpp` (a port of `mwscript.py`) |

Both build the same JSON tree (`mwscript.py`'s docstring has the shapes), which `mwworld/esmstore.cpp` turns into the interpreter's
nodes. The C++ function tables (`source/components/compiler/extensions0.hpp`) are generated from `mwscript.py`'s, and
`tools/test/script_compile_check.py` compiles every script and dialogue result with both and compares the trees
(26,230 compiles, 0 differences, 2026-10-15). Run it after changing either compiler.

`tools/test/script_gap.py` lists OpenMW's functions the engine does not run, and which master file uses each. None
that Morrowind.esm, Tribunal.esm or Bloodmoon.esm uses is missing (both add-ons are optional: [tribunal.md](tribunal.md),
[bloodmoon.md](bloodmoon.md)). With both add-ons in the load order: 32,911 compiles (1,207 scripts), 0 differences
(2026-10-26: `MW3DS_TRIBUNAL=1 MW3DS_BLOODMOON=1 script_compile_check.py`).

A run of comparison characters is one operator, read by its first two as OpenMW's scanner reads it (with a warning):
`=` then anything is `==`, `<` or `>` then `=` is `<=` / `>=`, else `<` / `>`, `!` then anything `!=`. Bloodmoon's
plantScript has `GetDistance Player <== 512` (it failed to compile: the strange seeds could not be planted). Both
compilers (`mwscript.py` `tokenize`, `compiler.cpp` `tokenize`); spaces inside a run (OpenMW takes `< =` as `<`) are not
read so: no script has one. Test: `bm-fn-misc`.

## Name arguments

An argument OpenMW reads as a string (signature S, c, x, z: an object, a script, a cell, a topic) is the word as
written, even where a local or global variable has the same name (OpenMW's StringParser): Calvus Horatius's
"StartScript Contract_Calvus" starts the script Contract_Calvus though his own script has a local of that name. Both
compilers (`mwscript.py` `arg(names_only)`, `compiler.cpp` `arg(namesOnly)`). Test: `trib-merc-contract`.

## Tribunal's functions

The 21 that Tribunal.esm's scripts and dialogue call and Morrowind.esm's don't, as OpenMW runs them:

| Function | Rule | Code | Test |
|---|---|---|---|
| GetInterior | 1 in an interior (Mournhold's under-the-sky cells too: they are interiors) | `mwscript/cellextensions.cpp` | `trib-fn-basic` |
| GetSquareRoot | the square root; a negative number reads 0 here (OpenMW stops the script) | `mwscript/miscextensions.cpp` | `trib-fn-basic` |
| GetStat | always 0: a placeholder in the original game (OpenMW's OpGetStat) | `mwscript/statsextensions.cpp` | `trib-fn-basic` |
| GetPCSneaking | the player holds the sneak stance | `mwscript/controlextensions.cpp` | `trib-fn-basic` |
| GetWeaponDrawn | the player's weapon (or fists) out; a person or creature fighting | `mwscript/miscextensions.cpp`, `Session::isWeaponDrawn` | `trib-fn-basic` |
| GetChameleon, GetParalysis | the effect's magnitude on the player or the actor | `mwscript/statsextensions.cpp` | `trib-fn-basic` |
| Enable / DisableLevitation | off: every Levitate ends at once (the player told `sLevitateDisabled`), a new one too; kept in a save | `mwscript/miscextensions.cpp`, `mwmechanics/activespells.cpp`, `mwworld/player.cpp` | `trib-fn-levitation` |
| GetCollidingPC, GetCollidingActor | the player / an actor touches the object (its box, within 2 units: from the side, or standing on it, as OpenMW's contact test) | `mwscript/miscextensions.cpp` | `trib-fn-objects` |
| HurtCollidingActor | whoever touches it so loses that much health a second (a negative number heals); nothing in a menu | `mwscript/miscextensions.cpp` | `trib-sotha-traps` |
| GetStandingActor | the player or an actor stands on its top | `mwscript/miscextensions.cpp` | `trib-fn-standing` |
| SetDelete | 1: gone (disabled, saved so), 0: back | `mwscript/miscextensions.cpp` | `trib-fn-objects` |
| ExplodeSpell | the player readies it; an object bursts the spell's area effects where it stands, always succeeding | `mwscript/miscextensions.cpp`, `Session::explodeSpell` | `trib-fn-objects` |
| Face | an actor turns to look at the point (3 degrees is close enough), then goes on (OpenMW's AiFace) | `mwscript/aiextensions.cpp`, `Session::npcFace` | `trib-fn-objects` |
| GetWaterLevel | outdoors 0 (the original game's oddity), an interior's water height, the lowest number with no water | `mwscript/cellextensions.cpp`, `mwworld/cellstore.cpp` | `trib-fn-waterlevel` |
| Set / ModWaterLevel | the player's interior's water moves (its surface, swimming, the view under it); kept for the cell and in a save; nothing outdoors | as GetWaterLevel; the converter gives such a cell a surface over its whole floor plan | `trib-fn-waterlevel` |
| PlaceItem, PlaceItemCell | a new object at the spot (the player's cell, or the one named): a person or creature as PlaceAtMe, an item lying there, any other object with its script running (drawn when it has a world visual); nothing for an unknown id or cell | `mwscript/transformationextensions.cpp`, `World::placeObject` | `trib-fn-placeitem` |

## Bloodmoon's functions

The 16 that Bloodmoon.esm's scripts and dialogue call and the other two masters don't (Bloodmoon is an optional add-on:
[bloodmoon.md](bloodmoon.md)), with the rest of the Stats loop they belong to, as OpenMW runs them:

| Function | Rule | Code | Test |
|---|---|---|---|
| Get / Set / Mod + ResistMagicka, ResistFire, ResistFrost, ResistShock, ResistDisease, ResistBlight, ResistCorprus, ResistPoison, ResistParalysis, ResistNormalWeapons, WaterBreathing, Chameleon, WaterWalking, SwimSpeed, SuperJump, Flying, ArmorBonus, CastPenalty, Silence, Blindness, Paralysis, Invisible, AttackBonus, DefendBonus (72; the spriggans' ModResist*) | Get: the effect's magnitude on the player or the actor less its opposite's (Resist Fire less Weakness to Fire ...; CastPenalty is Sound's, counted negative), Resist Fire / Frost / Shock with that element's shield added; a whole number. Mod adds to the part scripts set, which is counted into every effect total (resisting, Levitate, Chameleon ...), never runs out, is saved, and goes with Resurrect (OpenMW drops the actor's stats). Set moves that part so Get reads the value. Not an actor: 0. Both compilers know the 72 (`mwscript.py` `_EFFECTS`, `extensions0.hpp` `kScEffects`) | `mwscript/statsextensions.cpp` `effectStat`, `World::modScriptEffect`, `effectTotal` / `actorEffect` | `bm-fn-misc`, `bm-spriggan` |
| GetChameleon, GetParalysis | the same Get (Tribunal's: now through the same rule) | `mwscript/statsextensions.cpp` | `trib-fn-basic` |
| SetScale, GetScale, ModScale | the reference's size (OpenMW's scaleObject, unclamped). A person or creature is drawn that much bigger and its collision cylinder grows (28 x the change); a door is drawn so; an object baked into the cell's batches keeps its size (logged: no Bloodmoon script scales one). Kept in a save for a reference of a cell; Resurrect keeps it (the spriggan grows on her last life). The player: not scaled | `mwscript/transformationextensions.cpp`, `World::scaleObject`, `World::syncActor` | `bm-fn-misc`, `bm-spriggan` |
| GetWeaponType | the right hand's: a weapon's type (0 short blade .. 13 bolt), -2 a lockpick, -3 a probe, -1 empty or anything else; a person holds the weapon it fights with while it carries it, a creature nothing (ColonyBaro) | `mwscript/containerextensions.cpp` | `bm-fn-misc` |
| GetPCTraveling | 1 from a travel service's Go until the first scripts after the arrival have run (OpenMW: set on the click, cleared by the next world update) | `mwscript/miscextensions.cpp`, `Session::travelGo` | `bm-fn-misc` |
| GetPCInJail | 1 for the scripts running when a jail term is served and the next ones (OpenMW: while the jail window is up; ours is instant) | `mwscript/miscextensions.cpp`, `Session::goToJail` | `bm-fn-misc` |
| TurnMoonRed, TurnMoonWhite | Secunda's picture in [Moons] Script Color (255, 20, 20) until white again; kept in a save (OpenMW forgets it on a load, and bloodmoonScript turns it red only once), white in a new game | `mwscript/skyextensions.cpp`, `mwrender/sky.cpp` | `bm-moon-red` |
| BecomeWerewolf, UndoWerewolf | the player or a person (not a creature) changes; turning into one counts kills from nothing again (OpenMW's NpcStats::setWerewolf). The state only, saved: the form's stats, refusals, combat and look come with the werewolf work ([bloodmoon.md](bloodmoon.md)) | `mwscript/statsextensions.cpp`, `World::setWerewolf` | `bm-fn-misc` |
| IsWerewolf | 1 for a werewolf person or the player; a creature 0 | as above | `bm-fn-misc` |
| GetWerewolfKills | the player's kills since the change (counting them: the werewolf work) | as above | `bm-fn-misc` |
| SetWerewolfAcrobatics | marks the player's werewolf Acrobatics asked for (the stat itself: the werewolf work) | as above | `bm-fn-misc` |
| Dialogue function 72 (Werewolf) | the speaker is a werewolf person: Bloodmoon's 7 growls (Hello, Idle, Attack, Hit, Flee) | `mwdialogue/filter.cpp` | `bm-fn-misc` |
| Dialogue function 73 (PCWerewolfKills) | the player's werewolf kills (no response asks it) | `mwdialogue/filter.cpp` | none (nothing uses it) |

Not done: a creature placed by a script (PlaceAtMe) keeps only where it stands and its health in a save (as before),
so not its size or magic effect stats; the spriggans that matter are the cells' own.

## The console (Options > Console)

OpenMW's `Console::execute` and its `LineParser` with expressions allowed:

- One line at a time. It is compiled with the selected object's script locals, then run in that object's context: a
  call without `X->` acts on it, its locals are the line's variables.
- A line that starts with a number, `(`, `-`, `+`, a variable, `X.var`, or a function that returns a value is an
  expression: its value is printed, `%d` for a whole number and `%f` otherwise (`50`, `3.500000`). `X.var` prints by
  the type that object's script declares. Anything else is one statement (an instruction, `set ... to`).
- Block keywords (`if`, `while`, `begin`, `short` ...) are refused.
- Nothing selected: a call without `X->` to a function that needs an object is refused, "no implicit reference" (OpenMW
  throws the same). The functions that need none are those OpenMW registers without an explicit form, or whose opcode
  takes the reference as optional (Journal, StartScript, GetItemCount, PCJoinFaction ...): `NO_REF_NEEDED` in
  `mwscript.py`.
- Refused before anything runs: a name no table knows, and a function OpenMW has that the engine does not run
  ("not supported here"). OpenMW runs everything it compiles; this engine says so instead of half running a line.
- The object selected is the one under the crosshair when the menu was opened (OpenMW: a click on it).

Console commands the engine runs (OpenMW registers them for scripts too):

| Command | Does | Differs |
|---|---|---|
| `coc` / `CenterOnCell` | to a named exterior (a town's street) or an interior (where a door into it lands); "Cell X is not found" | |
| `coe` / `CenterOnExterior` | to the middle of that exterior cell, on the ground | |
| `tgm` / `ToggleGodMode` | "God Mode -> On / Off" | the player is kept alive; spells and fatigue still cost |
| `tai` / `ToggleAI` | actors stand still, packages and fights held; "AI -> On / Off" | |
| `sv` / `ShowVars` | the object's script variables; nothing selected: the globals, sorted, and running global scripts' | |
| `ori` / `OutputRefInfo` / `bc` | RefID, cell, coordinates, rotation, script | no content file, RefNum or model |
| `help` | the functions the engine runs | OpenMW lists every keyword |
| `FillMap` | every named exterior place on the map | |
| `FillJournal` | every quest's entries, every topic known | topic pages are not filled with every answer |

Not run (refused "not supported here"): collision, render and debug toggles (`tcl`, `tw`, `ts`, `tb`, `twf`, `tpg`,
`tcb`, `tcg`, `tm`, `tfow`, navmesh), `ssg`, `testcells`, `ra`, `fixme`, `user1`-`user4`, and the functions in
`script_gap.py`'s missing list.

The console screen itself is the 3DS's (bottom screen, the system keyboard): "differs (3DS UI, kept)".

## A new game

OpenMW's `World::startNewGame` (not skipping the menu): CharGenState 1, `main` and the SSCR start scripts added
(`GlobalScripts::addStartup`; Morrowind.esm has none), then the global scripts run five times before the first frame.
Main starts Startup, VampireCheck and CharGen; CharGen disables the controls, jumping, view switch, vanity mode,
fighting and magic, moves the player into the Imperial Prison Ship, sets the Bitter Coast cloudy
(`ChangeWeather "Bitter Coast Region" 1`), sets CharGenState to 10 and stops. The engine does the same
(`Session` new game); it used to set CharGen's results by hand, without the weather.

## GetTarget

`x->GetTarget <id>` (OpenMW `OpGetTarget`): true when the actor's combat target has that id; with no combat target,
`GetTarget player` is also true while the actor greets the player. Another actor fought by StartCombat counts (Falco
Galenus at Baslod: Bloodmoon's ColonyAssassin waits for it; case `script-gettarget-actor`).

## Findings

- 2026-10-15: CharGen's weather never ran (the engine copied CharGen's other effects by hand). Test
  `chargen-start-script` (failed before, passes after).
- 2026-10-15: both compilers type `X.var` as a float, so `X.var / 2` divides as floats; OpenMW asks the remote
  script's declared type (a short divides as whole numbers). Not fixed: no vanilla script found depending on it yet.
- 2026-10-15: `Equip` on an NPC only gives the item; OpenMW equips it. No vanilla script calls `Equip`; an NPC's
  clothes and armour are baked into its meshes at conversion and its weapon is a record field, so wearing a new item
  at run time is a larger change. Not fixed.
