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
- **The sun's light from the side is one number a vertex; the rest is baked.** Opaque outdoor geometry bakes the noon
  sun (overhead, to the south) and keeps in its colours' alpha how much it faces east or west; the low sun of morning
  and evening and the night light (both along OpenMW's path) add their colour to the side facing them: four vertex
  instructions, no memory. Terrain and see-through or alpha-tested things (leaves, fences) keep the noon pattern all
  day, tinted by the hour; OpenMW lights every vertex by its normal. (`convert_cell.py side_facing`, `World::sunSide`,
  `cell.v.pica sideLight`)
- **NPCs are drawn from a library the converter dressed** (their race's body, head, hair and what they wear, merged:
  about 29 meshes each, against about 35 put together from parts). One whose clothes leave its pack (a stripped
  corpse) is put together from the shared third-person pieces at run time instead (`npcDress`): about 6 more draws
  for that one. OpenMW builds every NPC from parts.

## View distance and detail

- **Never farther than OpenMW's view (7168); shorter when frames run long.** The dynamic view closes in with the fog
  (Options > Video > Dynamic view). An Old 3DS or 2DS runs a steady 20 fps outdoors (30 indoors) and
  keeps it with the view: 0.8 of OpenMW's at most, closing in as far as 0.3 (2150) whenever frames come slower than
  20 a second, growing back only with room to spare. The view is drawn in steps of a twentieth, moved only once the
  adapted scale is a whole step away: drawn as adapted, the fog's reach followed the scale's saw of a few thousandths
  a frame at its threshold, and on an Old 3DS the walkways' edges in the fog flashed. (`include/cell.h`,
  `Session::viewScale`, `Session::viewDrawn`, `Session::adaptView`, `console.cpp`)
- **Rain and storms shorten the view.** OpenMW draws the whole view and moves the fog's start in; drawing land hidden in
  thick fog cost both consoles their frame rate. (`World::weatherFogScale`)
- **Small clutter stops at a distance by its size.** Each size tier vanishes at about 25 pixels on screen.
  (`convert_cell.py detail_distance`)
- **People and creatures past 6000 (4000 on an Old 3DS) aren't drawn; small pieces of them stop sooner.**
  (`kActorViewDistance`, `actorViewLimit`, `kActorDetailDistance`)
- **People are re-posed on a cadence**, often up close, less often the smaller they are on screen. (`World::update`)

## Rain and storms

- **Rain, ash and blight are particles in a box round the eye, fewer than OpenMW's.** Up to 450 rain streaks (OpenMW
  450 to 650 raindrop batches), 240 ash or blight flakes, 24 dust clouds (OpenMW: ashcloud.nif's 100 flakes and 72
  clouds), each wrapped to the box's far side as the eye leaves it behind (OpenMW's WrapAroundOperator), moved and
  built on the CPU in one pass a frame and drawn in a few draws (about 0.5 ms of an Old 3DS's CPU). The dust clouds
  fade out within 400 units of the eye: a 400-unit cloud near the eye is a see-through layer over most of the screen,
  and blending costs the GPU per pixel covered. (`Session::drawPrecipitation`)
- **On an Old 3DS, the streaks alone; a New 3DS adds a far-rain ring and ripples.** The ring is one band of falling
  drops round the horizon, 450 out, about 0.4 of the screen blended (a whole ring covered the screen, two of them about
  2 ms of GPU in 3D); the ripples are up to 40 small quads and about two floor checks a frame. An Old 3DS has the least
  to spare in a busy town, in 3D most of all. Neither is in OpenMW (it has no distant rain; its ripples are on water,
  with its shaders). Full rings, sheets stacked across the view, or a screen-wide rain picture were each tried or
  weighed and turned down for the blending cost. (`console.cpp rainExtras`, `Session::drawPrecipitation`)

## Particles and memory

- **A particle budget a frame** (spell visuals, sprays): fewer sprites while frames run long, every visual keeping the
  same share. (`console.cpp spriteBudget`, `Session` frame pacing, `renderer.cpp`)
- **Far cells' objects are dropped from memory** past a reference budget, and read again when needed.
  (`console.cpp refBudget`, `World::evictCells`)
- **An Old 3DS keeps textures to 128 texels and draws from Project Atlas's sheets** (fewer draws). A sheet's cap is per
  picture in it, its short side, the long side up to 1024: a 256 x 2048 sheet of eight pictures is 128 x 1024, the
  memory of the eight at 128 apiece (taken on the long side it was 16 x 128, each wall 16 texels across). Seyda Neen:
  about 480 KB more texture memory. (`textures.py is_sheet`, `build_cia.py ATLAS_OVERLAYS`)
- **An Old 3DS draws every world texture at full size, never its smaller copies (mipmaps).** A sheet's smaller copies
  blend its pictures into each other (Hlaalu's wall bleeds into the dark trim packed under it), and far walls and
  walkways showed dark patches flickering in and out. Far ground shimmers a little more and reads more texture memory
  a frame; the Old 3DS's GPU has the room (about 15 ms of its 50). The New 3DS keeps them: no sheets there.
  (`ConsoleProfile::textureMips`, `cell.cpp loadTexture`)

## Movement

- **No navigation mesh.** Actors walk on the path grids and climb up to 60 degree slopes; the player 46. A navmesh
  costs memory and CPU the console doesn't have. In a fight an actor climbs only the player's 46 (a quest follower
  keeps 60), so backing up a steep slope wins nobody a fight (`npcClimbSlope`; our own rule).
