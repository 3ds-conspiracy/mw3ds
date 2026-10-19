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
that Morrowind.esm uses is missing; the ones only Tribunal or Bloodmoon use are out of scope.

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
