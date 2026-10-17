# Performance trade-offs

Where the engine does less than OpenMW on purpose, so the New 3DS keeps its frame rate. Each one was a conscious
choice, made after measuring or comparing side by side. **Do not "fix" one of these toward OpenMW without asking the
maintainer first**: the gap is known and accepted. A change that keeps the cost and closes the gap is welcome; say so
in the change.

Each entry: what we do, what OpenMW does, why, and where it lives.

## Lighting

- **Lamps are baked, not live.** The converter lights the world's vertices with every lamp once (OpenMW's falloff,
  `convert_cell.py light_falloff`); OpenMW lights everything with up to 8 live lights an object, every frame. Live
  lights cost vertex-shader work on everything they touch, and the console's vertex work is its limit. Outdoors each
  batch keeps its lamps' share apart (one colour a batch, a brightness a vertex in the position's spare w) so the night
  darkens the rest only: one shader instruction a vertex. A lamp a script switches off keeps its baked light.
  (`convert_cell.py`, `terrain.py`, `cell.v.pica` lamp, `renderer.cpp setLamp`)
- **The player's torch is the one live light.** In first person it is lit per pixel through the fog's texture lookup
  (depth from the eye, not true distance: the screen's sides get a little more), no vertex work; in third person per
  vertex. (`renderer.cpp setLight`, `updateLightRamp`)
- **People's torches light people only.** A torch held out lights its holder (`Actor::heldLight`) and the people near
  it (`Session`, per person on the CPU), never walls or ground; OpenMW lights the world from them. Per-vertex torch
  lights on nearby batches cost the GPU a large part of a town at night (one batch is up to a 2048-unit chunk).
- **People take one light for the whole body.** A tint a person, from the lamps and torches near where they stand
  (`Session`, `renderer.cpp actorTint`); OpenMW lights each vertex of each person by its normal.
- **The night is a tint on the day's baked light.** Surfaces keep the daytime pattern of sun and shade, darkened;
  OpenMW's moonlight lights the side facing the moon. Splitting the sun's share out would need another channel in
  every vertex. (`World::daylight`)

## View distance and detail

- **Never farther than OpenMW's view (7168); shorter when frames run long.** The dynamic view closes in with the fog
  (Options > Video > Dynamic view). (`include/cell.h`, `Session::viewScale`)
- **Rain and storms shorten the view.** OpenMW draws the whole view and moves the fog's start in; drawing land hidden in
  thick fog cost both consoles their frame rate. (`World::weatherFogScale`)
- **Small clutter stops at a distance by its size.** Each size tier vanishes at about 25 pixels on screen.
  (`convert_cell.py detail_distance`)
- **People and creatures past 6000 (4000 on an Old 3DS) aren't drawn; small pieces of them stop sooner.**
  (`kActorViewDistance`, `actorViewLimit`, `kActorDetailDistance`)
- **People are re-posed on a cadence**, often up close, less often the smaller they are on screen. (`World::update`)

## Particles and memory

- **A particle budget a frame** (spell visuals, sprays): fewer sprites while frames run long, every visual keeping the
  same share. (`console.cpp spriteBudget`, `Session` frame pacing, `renderer.cpp`)
- **Far cells' objects are dropped from memory** past a reference budget, and read again when needed.
  (`console.cpp refBudget`, `World::evictCells`)

## Movement

- **No navigation mesh.** Actors walk on the path grids and climb up to 60 degree slopes; the player 46. A navmesh
  costs memory and CPU the console doesn't have.
