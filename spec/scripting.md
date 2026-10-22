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
that Morrowind.esm or Tribunal.esm uses is missing (Tribunal is an optional add-on: `internal`'s plan); the ones only
Bloodmoon uses are out of scope.

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

## Findings

- 2026-10-15: CharGen's weather never ran (the engine copied CharGen's other effects by hand). Test
  `chargen-start-script` (failed before, passes after).
- 2026-10-15: both compilers type `X.var` as a float, so `X.var / 2` divides as floats; OpenMW asks the remote
  script's declared type (a short divides as whole numbers). Not fixed: no vanilla script found depending on it yet.
- 2026-10-15: `Equip` on an NPC only gives the item; OpenMW equips it. No vanilla script calls `Equip`; an NPC's
  clothes and armour are baked into its meshes at conversion and its weapon is a record field, so wearing a new item
  at run time is a larger change. Not fixed.
