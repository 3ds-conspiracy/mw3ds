# Engine source

The folders and file names follow OpenMW's, so a rule is found in the file of the same name in both trees:
`source/<dir>/<file>` here is `apps/openmw/<dir>/<file>` in OpenMW, and `source/components/<dir>` is its
`components/<dir>`. A header sits beside its `.cpp` as `.hpp`, included from `source/`
(`#include "mwworld/worldimp.hpp"`).

| Folder | What is in it |
|---|---|
| `mwbase/` | `Session` (`session.hpp`): the game's state and services, OpenMW's `Environment` and managers in one |
| `mwworld/` | `World` (`worldimp`), references (`livecellref.hpp`: `Ref`), cells and streaming (`scene`, `cellstore`), items (`containerstore`, `inventorystore`), weather, time, activation (`action*.cpp`), projectiles; `esmstore.hpp` holds `GameData`, the loaded records |
| `mwmechanics/` | Rules: combat, AI packages (`ai*.cpp`), magic (`spell*.cpp`, `activespells`), crime and disposition (`mechanicsmanagerimp`), skills (`npcstats`), alchemy, enchanting, repair; `formulas.cpp` holds the numbers the spec tests read |
| `mwphysics/` | Collision meshes (`physicssystem`), moving the player and actors (`movementsolver`) |
| `mwrender/` | Drawing (`renderingmanager`), actors' meshes and animation (`animation`, `npcanimation`, `viewmodel`), sky, water, effects, camera |
| `mwgui/` | The screens, one file per OpenMW window (`inventorywindow`, `tradewindow`, `journalwindow` ...); `widgets.cpp` is the UI toolkit |
| `mwdialogue/`, `mwscript/`, `mwsound/`, `mwinput/`, `mwstate/` | Dialogue, script functions (`*extensions.cpp`), sound, input, saves |
| `components/` | The script compiler and interpreter (`scriptfunctions.hpp` is the function table), log, files, settings, misc; `esm3/load*.hpp` the game data's records as OpenMW names them (`loadnpc.hpp`: `ActorDef`), `fallback/` Morrowind.ini's sky and weathers |
| `platform/` | The 3DS itself (no OpenMW counterpart): console profile, heaps, linear memory, the light sensor |
| `testdrive/` | The test driver (no counterpart): step runner, movement, `EXPECT` values (`expect*.cpp`), tokens (`ops*.cpp`), monitors |
| `shaders/`, `extern/` | PICA vertex shaders; cJSON |

`engine.cpp` is the game loop (OpenMW's `engine.cpp`), shared by the console's `main.cpp` and the PC test build.

A `*helpers.hpp` is a folder's private header: what a few of its files share, not for use elsewhere. New code goes
in the file OpenMW keeps the same rule in; a new `EXPECT` value, token or script function goes in the part of its
subject (`testdrive/expectstats.cpp`, `testdrive/opsitems.cpp`, `mwscript/statsextensions.cpp`).
