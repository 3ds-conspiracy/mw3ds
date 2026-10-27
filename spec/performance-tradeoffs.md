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
  (`convert_cell.py`, `terrain.py`, `shaders/cell.v.pica` lamp, `mwrender/renderingmanager.cpp setLamp`)
- **The player's torch is the one live light.** In first person it is lit per pixel through the fog's texture lookup
  (depth from the eye, not true distance: the screen's sides get a little more), no vertex work; in third person per
  vertex. (`mwrender/renderingmanager.cpp setLight`, `updateLightRamp`)
- **People's torches light people only.** A torch held out lights its holder (`Actor::heldLight`) and the people near
  it (`Session`, per person on the CPU), never walls or ground; OpenMW lights the world from them. Per-vertex torch
  lights on nearby batches cost the GPU a large part of a town at night (one batch is up to a 2048-unit chunk).
- **People take one light for the whole body.** A tint a person, from the lamps and torches near where they stand
  (`Session`, `mwrender/renderingmanager.cpp actorTint`); OpenMW lights each vertex of each person by its normal.
- **The sun's light from the side is one number a vertex; the rest is baked.** Opaque outdoor geometry bakes the noon
  sun (overhead, to the south) and keeps in its colours' alpha how much it faces east or west; the low sun of morning
  and evening and the night light (both along OpenMW's path) add their colour to the side facing them: four vertex
  instructions, no memory. Terrain and see-through or alpha-tested things (leaves, fences) keep the noon pattern all
  day, tinted by the hour; OpenMW lights every vertex by its normal. (`convert_cell.py side_facing`, `World::sunSide`,
  `shaders/cell.v.pica sideLight`)
- **In the heaviest outdoor spots a tree takes one baked light for all its placements in a cell.** Where a 3 x 3 spot's
  placed models pass 600 k vertices (Solstheim's pine forests and Raven Rock; never Vvardenfell, whose busiest is
  554 k), a static or activator placed 3 or more times in a cell keeps its vertices once and is drawn once a placement
  with its own transform (cell format 11, shared models). A rock or a wall keeps each placement's baked colours (4
  bytes a vertex); a tree or plant (`flora_*`) shares one, the model lit unturned by the cell's ambient light and the
  noon sun, so a turned tree's lit side and the low sun's side light don't follow its turn. Side by side the forest
  looked the same (`bm-shared-pines` shots: about 5 % of the top screen's pixels moved by more than 16 of 255, the
  trees' shading). Gain of the one light: the Hirstaang Forest's spot from 26.0 to 23.6 MB of linear memory (level.py's
  report; 36.2 with the trees copied into the batches); natively the Old 3DS profile kept 4.7 MB free there with
  every cell in, where each placement's own colours left 1.8. A placement a lamp lights stays baked. The cost is draw calls: the forest's view drew 210 things instead of 146, Raven Rock's 395 instead of 289.
  OpenMW lights every vertex of every placement. (`convert_cell.py share_models`, `SHARE_ONE_LIGHT`, `level.py
  heavy_spots`, `mwrender/renderingmanager.cpp`)
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
  a frame at its threshold, and on an Old 3DS the walkways' edges in the fog flashed. (`source/mwrender/objects.hpp`,
  `Session::viewScale`, `Session::viewDrawn`, `Session::adaptView`, `platform/console.cpp`)
- **Rain and storms shorten the view.** OpenMW draws the whole view and moves the fog's start in; drawing land hidden in
  thick fog cost both consoles their frame rate. (`World::weatherFogScale`)
- **Small clutter stops at a distance by its size.** Each size tier vanishes at about 25 pixels on screen. An Old 3DS
  stops it at 0.8 of that (the tiers' 2500 / 5000 / 10000 units as 2000 / 4000 / 8000): side by side
  (`trial-detail-distance`, `trial-detail-balmora`) little was lost but far fence posts.
  (`convert_cell.py detail_distance`, `ConsoleProfile::detailScale`)
- **Mournhold's big areas under the sky are treated as outdoors** (user, 2026-10-27, after the Great Bazaar ran about
  20 fps in 3D): the dynamic view and its fog close in there when frames run long, small clutter stops by its size
  (the Bazaar from its entrance in 3D: 199000 triangles to 178000; the dynamic view takes little more, the whole area
  fits in its shortest reach), and the cell is cut in 2048-unit pieces for culling. Shared models don't reach them:
  every placement there is lamp lit, and a shared model has no lamp light. OpenMW draws them whole. Other interiors
  keep their whole view. (`Session::adaptView`, `engine.cpp` fogScale, `convert_cell.py` sky interiors;
  `perf-mournhold-sky-interiors`, `bug-bazaar-3d-view`)
- **People and creatures past 6000 (4000 on an Old 3DS) aren't drawn; small pieces of them stop sooner.**
  (`kActorViewDistance`, `actorViewLimit`, `kActorDetailDistance`)
- **People are re-posed on a budget, by time, not every frame.** A New 3DS: 30 poses a second for the three people
  most in view, 15 for the rest near and the middle distance, 10 far and out of view (user, 2026-10-27: the Old 3DS's
  way a step up). Counted in frames it was every 2nd, 3rd and 4th frame, tuned for 60 fps: at 30 fps the nearest moved
  at 15 a second and looked slow; shortening the steps below 50 fps fixed that but posed a Balmora street 1.8 x as
  often (6772 poses in 20 s against 3715) and the town ran slower; the budget makes 4467. An Old 3DS (and 2DS): 20 a
  second for the two most in view, 10 for the rest near and the middle, 6.7 far and out of view (counted in frames it
  was 10, 5 and 3 a second at 20 fps: choppy). OpenMW poses everyone every frame. (`World::update`,
  `ConsoleProfile::poseHz`, `perf-new3ds-pose-budget`, `perf-o3ds-smooth-poses`, `bug-poses-at-30fps`)

## Rain and storms

- **Rain, ash and blight are particles in a box round the eye, fewer than OpenMW's.** Up to 450 rain streaks (OpenMW
  450 to 650 raindrop batches), 240 ash or blight flakes, 24 dust clouds (OpenMW: ashcloud.nif's 100 flakes and 72
  clouds), each wrapped to the box's far side as the eye leaves it behind (OpenMW's WrapAroundOperator), moved and
  built on the CPU in one pass a frame and drawn in a few draws (about 0.5 ms of an Old 3DS's CPU). The dust clouds
  fade out within 400 units of the eye: a 400-unit cloud near the eye is a see-through layer over most of the screen,
  and blending costs the GPU per pixel covered. (`Session::drawPrecipitation`)
- **Bloodmoon's snow and blizzard: 240 flake sheets on a New 3DS, 120 on an Old one; the blizzard's view a quarter
  of the whole.** (Agreed 2026-10-25: snow budgeted like ash.) Morrowind.ini asks for 750 flakes and OpenMW's snow.nif
  / blizzard.nif emit 2000 particles of a 75 (150)-unit sheet of a few dozen specks each, plus 600 blizzard clouds. Ours
  take the ash flakes' places in the box round the eye: each a 60-unit sheet of four dozen of the original's specks
  (`artassets.py snow_sheet`: the original sheet is mostly empty texels the GPU would blend for nothing), turned to the
  eye and gone within 40 units of it (a near sheet would fill the screen); about 10,000 specks in view on a New 3DS, the
  CPU's work that of 240 ash flakes. An Old 3DS has half (`ConsoleProfile::snowFlakes`). The blizzard's clouds are the
  ash storm's 24 dust clouds with its picture, at half their strength. The blizzard's Land Fog Depth 2.8 puts the view
  at `World::weatherFogScale`'s floor, a quarter of the whole drawn, fogged from the eye: less to draw, as in blight.
  (`Session::drawPrecipitation`, `bm-weather-snow`, `bm-weather-blizzard`, `bm-o3ds-snow`)
- **On an Old 3DS, the streaks alone; a New 3DS adds two far-rain rings and ripples.** The rings are bands of falling
  drops round the horizon: one 450 out, about 0.4 of the screen blended, and a fainter, narrower one 900 out, about 0.2
  of it (rain behind rain: depth to a storm, in 3D most of all). A whole ring covered the screen, two of them about 2 ms
  of GPU in 3D; the two bands blend about 0.6 of it. The farther band goes while frames run under 30 a second and
  comes back past 33 (`feat-rain-far-band-slow`). The ripples are up to 40 small quads and about two floor checks a
  frame. An Old 3DS has the least
  to spare in a busy town, in 3D most of all. Neither is in OpenMW (it has no distant rain; its ripples are on water,
  with its shaders). Full rings, sheets stacked across the view, or a screen-wide rain picture were each tried or
  weighed and turned down for the blending cost. (`platform/console.cpp rainExtras`, `Session::drawPrecipitation`)

## Particles and memory

- **A particle budget a frame** (spell visuals, sprays): fewer sprites while frames run long, every visual keeping the
  same share. (`platform/console.cpp spriteBudget`, `Session` frame pacing, `mwrender/renderingmanager.cpp`)
- **Far cells' objects are dropped from memory** past a reference budget, and read again when needed.
  (`platform/console.cpp refBudget`, `World::evictCells`)
- **The sound cache holds 1 MB on an Old 3DS, 3 MB on a New 3DS** (sounds heard lately; the rest read from the card
  again). OpenMW keeps far more. The sounds are DSP-ADPCM, a little over a quarter of PCM16, so this still holds nearly
  twice the sounds the old 2 / 6 MB of decoded ones did. (`platform/console.cpp soundCacheBytes`,
  `mwsound/soundmanagerimp.cpp`)
- **An Old 3DS keeps textures to 128 texels and draws from Project Atlas's sheets** (fewer draws). A sheet's cap is per
  picture in it, its short side, the long side up to 1024: a 256 x 2048 sheet of eight pictures is 128 x 1024, the
  memory of the eight at 128 apiece (taken on the long side it was 16 x 128, each wall 16 texels across). Seyda Neen:
  about 480 KB more texture memory. (`textures.py is_sheet`, `build_cia.py ATLAS_OVERLAYS`)
- **An Old 3DS draws every world texture at full size, never its smaller copies (mipmaps).** A sheet's smaller copies
  blend its pictures into each other (Hlaalu's wall bleeds into the dark trim packed under it), and far walls and
  walkways showed dark patches flickering in and out. Far ground shimmers a little more and reads more texture memory
  a frame; the Old 3DS's GPU has the room (about 15 ms of its 50). The New 3DS keeps them: no sheets there.
  (`ConsoleProfile::textureMips`, `mwrender/objects.cpp loadTexture`)
- **An Old 3DS leaves a neighbouring cell unloaded while under 5 MB of linear memory is left** (the distant land stands
  in until it comes). Its 22 MB ran dry with Seyda Neen's nine cells in: the actors, the cells' last batches (white
  walls), the item icons and a book's pages had none. Seyda Neen now holds six or seven of its nine cells.
  (`World::streamExterior`, `bug-o3ds-seyda-linear`)

## Movement

- **No navigation mesh.** Actors walk on the path grids and climb up to 60 degree slopes; the player 46. A navmesh
  costs memory and CPU the console doesn't have. In a fight an actor climbs only the player's 46 (a quest follower
  keeps 60), so backing up a steep slope wins nobody a fight (`npcClimbSlope`; our own rule).
