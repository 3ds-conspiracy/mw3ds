# Findings: weather and daylight

Written from OpenMW's source only (`mwworld/weather.cpp`, `mwscript/skyextensions.cpp`, `mwworld/datetimemanager.cpp`);
the engine's weather code was not read. Full rules: `spec/weather.md`. Extends `spec/audit-findings.md` rows D7, J17,
J18, J19 (not redone).

## Rules

- **Colour by hour** (`weather.cpp: TimeOfDayInterpolator::getValue`): sky / fog / ambient / sun each blend
  night - sunrise - day - sunset - night through windows set by that channel's own lead / lag around 6 / 8 / 18 / 20
  (Weather_<Channel>_Pre/Post-Sunrise/Sunset_Time). Peak sunrise / sunset colour is at the window's middle, not at
  the boundary. Vanilla Sky window 5.5..9 (mid 7.25), 16.5..20.5 (mid 18.5). Stance: follow OpenMW exactly.
- **Fog depth**: same interpolator ("Fog" windows), but sunrise / day / sunset all carry the *day* depth; only the
  night uses the night depth, so the depth changes only inside the second half of sunset and the first half of
  sunrise. (A quirk of OpenMW's constructor; follows vanilla's observed behaviour. Follow.)
- **Transitions** (`addWeatherTransition`, `updateWeatherTransitions`): start immediately when idle; else queue one
  (a newer queued replaces an older; the same as next is ignored). Duration is real seconds: `1 / delta` of the NEXT
  weather (0.015 -> 67 s; Thunderstorm and Blizzard 0.03 -> 33 s). Not scaled by timescale. Skipping time
  (rest / wait / travel) completes at once to the last weather set.
- **GetCurrentWeather** returns the *outgoing* weather during a transition (J18 resolved: ours returns the target
  at once, which is wrong).
- **ChangeWeather** (`WeatherManager::changeWeather`): unknown weather number: silent no-op; unknown region: a
  warning only; other region: stored, no transition; current region: transition / queue. The weather is stored in the
  region, so it ends at the next 20-hour expiry (J17: not a permanent override).
- **ModRegion** (`skyextensions.cpp OpModRegion`, `RegionWeather::setChances`): chances for all ten weathers are
  replaced; weathers not given -> 0; each clamped 0..100; new roll only if the current weather's chance became 0;
  then a transition if the region is current (J19).
- **Region roll** (`RegionWeather::chooseNewWeather`): number 1..100, cumulative in weather order 0..9; remainder
  (sum < 100) -> Clear. Every 20 game hours all regions' weathers are cleared, the current region re-rolls and
  transitions. Entering another region by teleport / travel: immediate weather, no transition; by walking: transition.
- **Interiors** (`WeatherManager::update`): timers and transitions still advance; sky off, sounds stopped, wind 0;
  no fog / ambient / sun result (the cell's lighting applies). Quasi-exterior cells count as exterior. D7 (condition
  fails indoors) still stands.
- **Storm / precipitation**: storm = wind speed > fStromWindSpeed (0.7): Ashstorm, Blight, Blizzard. Precipitation
  = rain or particle effect, except Ashstorm (Blight counts): decides torches (`useTorches`: dark and none).
- **Transition blend of discrete values**: precipitation, particle effect, rain loop, thunder belong to the current
  weather below the next weather's Rain Threshold (0.6 Rain / Thunderstorm, 0.5 for others), then to the next;
  volume / alpha cross-fade. Colours, fog depth, wind, glare, cloud speed blend linearly.
- **Thunder**: needs ratio >= Thunder Threshold; chance per frame `freq*10/60*dt*scale`; four strike sounds; flash
  adds to fog, ambient and sun colours.
- **Sun**: enabled for 6 < h < 20; glare fade a triangle peaking at 13; sun path east to west with level at 6 / 20;
  night / day mode: exterior and h outside 6..20 = night; interior in daylight with glare view >= 0.5 = interior day.
- **Clock**: game seconds per real second = global timescale (30: 48 real minutes a day).

## Tests written

`tools/specgen_weather.py` -> `tools/tests/openmw-spec-weather.txt` (about 1800 steps; every EXPECT is computed from
`out/world/game*.json` and the Morrowind.ini fallbacks):
- colours (sky / fog / ambient / sun channels) and fog depth for each of the ten weathers at boundary and random
  hours (section 1);
- storm flag, precipitation, glare view per weather; target wind range (2, 3);
- every region's chance table (4);
- sun on / off, glare fade, torches by hour (5);
- a transition: GetCurrentWeather, next, blend, colour mid-transition, completion (6); queueing (7); fast forward
  by sleeping (8); invalid ChangeWeather (9); ModRegion replace / clamp / re-roll (10, 11);
- interior: sky off, no wind, transition still runs, night / day mode (12); travel into another region (13);
- the clock at timescale 30 (14) (uses only existing kinds: `hour`, `SNAP`).

Only `weathernow` and `hour` exist today; everything else needs the hooks below. Until they exist a run reports
the unknown kinds.

## Hooks needed in the engine

Setup tokens:
- `SETWEATHER:<id>`: make the current region's weather this id at once (no transition, queue cleared); like
  `forceWeather`.
- `CHANGEWEATHER:<region>:<id>`: what the script function does. `region` has spaces as in the id (`%` for a space).
- `MODREGION:<region>:<c0>,<c1>,...`: what ModRegion does (the list may be shorter than 10; missing = 0).

EXPECT kinds (all return a number; `eq / ge / le` as usual):
- `weathernow` (exists): the number GetCurrentWeather gives: must be the OUTGOING weather during a transition.
- `wnext`: next weather number, -1 if none. `wqueued`: queued weather number, -1 if none.
- `wtrans`: blend toward next, 0..1 (0 when idle) (= 1 - OpenMW's transition factor).
- `wcolor:<sky|fog|ambient|sun>,<r|g|b>`: the weather result colour channel for the current hour (0..1), as set on
  the scene (fog colour, ambient, sun) and the sky.
- `wfogdepth`: the land fog depth (the interpolated per-weather value, before any view-distance scale).
- `wwind`: current wind speed (0 indoors). `wstorm`, `wprecip`, `wtorches`, `wsunon`: 0 / 1. `wskyon`: 0 / 1 sky shown.
- `wsunvis`: the sun visibility (glare view blend). `wglarefade`: the time-of-day glare factor.
- `wnightday`: 0 default, 1 interior day, 2 exterior night.
- `wloop`: 1 if an ambient or rain loop is playing from the weather, else 0.
- `regionchance:<region>,<weather id>`: the stored chance (0..100). `regionweather:<region>`: that region's stored
  weather id, -1 if unset.

## Open questions

- Our `game.json` carries only colours, fog depth, one sound id and the region chances per weather; transition delta,
  wind, thresholds, glare view, thunder and the day boundaries are not in it (the oracle reads the cfg / ini). The
  engine must hold them to pass tests 2, 3, 5, 6; the fix agent should check where it keeps them.
- The 8-byte region chance list in `game.json` (Bloodmoon regions have 10): the oracle pads with 0.
- `SLEEP:1:2` for the fast-forward step: second arg 2 should mean no creature interrupts; check.
- The test relies on region names for `GOTO` cells (Seyda Neen, Balmora, Ald-ruhn) being the cells' regions as in the
  data (bitter coast, west gash, ashlands).
- Not covered: moons, star fade, sun-disc colour, thunder randomness, ash / snow particle layout, cloud textures.

## Mismatches

Fix phase done (compiled; not yet run on the emulator). `game.json` needs a rebuild: it now carries per weather
`delta`, `wind`, `glare`, `rain_threshold`, `thunder_freq`, `thunder_threshold`, `flash_decrement`, `clouds_max`, `precip`
and a `weather_clock` (day boundaries and the four channels' lead / lag windows). Missing keys fall back to vanilla
values (Morrowind.ini lacks most per-weather ones; the converter's table is OpenMW's built-in fallbacks). The test now
writes `%` for spaces in region names (the step lines split at spaces).

| Rule | Our behaviour before | Cause | Status | File |
|---|---|---|---|---|
| J18 GetCurrentWeather = outgoing weather | already the outgoing one in a running change, but a ChangeWeather took effect a frame late and ModRegion / expiry bypassed the queue | the stored weather and the transition were two states | fixed: one state (now / next / queued) as OpenMW | world.cpp |
| J17 ChangeWeather ends at the next expiry | the weather was taken up once and the next 20 h roll dropped it by `gameHour` arithmetic (and a HOUR jump re-rolled) | no per-region stored weather; timer was absolute hours | fixed: region keeps it (`forcedWeather`) until the countdown `weatherTimer` clears all regions | world.cpp |
| J19 ModRegion: chances not given become 0 | script padded 0 already; the re-roll happened only when the *next* weather's chance was 0 and only for the current region | rule read the wrong weather | fixed: re-roll when the region's stored weather has chance 0 (any region), then a change if current | world.cpp |
| D7 Weather condition indoors fails | already returns 0 in interiors (dialogue.cpp:199) | an earlier agent | already fixed, not touched | dialogue.cpp |
| Region roll: 1..100 walk, remainder Clear | `rand() % total` over the chances (sum under 100 never gave Clear) | own invention | fixed | world.cpp |
| Colour by hour, per channel windows | one table (5 / 6.5 / 8 / 17 / 18.5 / 20) for all channels | no per-channel lead / lag | fixed: `timeOfDay()` is OpenMW's interpolator, windows from the ini | world.cpp, level.py, game.cpp |
| Fog depth by hour | night factor ramp 5..8 and 17..20 | same | fixed: day depth sunrise-mid to sunset-mid, night depth toward night (OpenMW's constructor) | world.cpp |
| Fog depth to view distance (0.69 / depth, clamp 0.25..1) | own mapping | 3DS view distance | differs on purpose: only the hour part follows OpenMW | world.cpp |
| Baked light scale (`daylight()` land / sky factors, clamps) | own mapping of weather colour to the baked cell light | 3DS draws pre-lit cells | differs on purpose | world.cpp |
| Transition speed | fixed half a game hour | invention | fixed: real seconds, `1 / delta` of the next weather, leftover carried to the queued one | world.cpp |
| Queue (one wait, a newer replaces, same as next ignored) | none | none | fixed | world.cpp |
| Fast forward on rest / wait / travel / training | blend ran on | none | fixed: any hours skipped (`gameHour` jump beyond the frame's) finish the change to the last weather; HOUR / SetGameHour do not count | world.cpp, main.cpp |
| 20 h expiry counted in game hours passed (incl. skipped) | absolute-hour compare | see J17 | fixed | world.cpp |
| Arrival by door / travel: weather at once; walking over a border: a change | always at once | none | fixed (walking = last cell an adjacent exterior) | world.cpp |
| Timers and changes keep running indoors | updateWeather returned early indoors | none | fixed | world.cpp |
| Wind: min(8 w, 70), gust within 0.5..2x, rain 25 %, blend, 0 indoors | no wind | not modelled | fixed (value only; nothing in the engine uses it yet) | world.cpp |
| Storm / precipitation (ash excluded) / torches / sun on / glare fade / sun visibility / night-day mode | not modelled (storm walking uses fixed weather ids 6, 7, 9 = the same set) | none | fixed as queries with hooks; nothing consumes torches / night-day yet | world.cpp |
| Rain loop, precipitation and storm belong to the old weather until the next weather's rain threshold, then fade in | switch at 0.5, linear cross-fade | none | fixed (loop weather by `weatherShown()`, falling rain / ash alpha as OpenMW) | world.cpp, session.cpp |
| Thunder strikes (chance per frame, four sounds, flash adds to fog / ambient / sun) | timer 8..28 s, sound only, no flash | own | differs on purpose: the flash is skipped (bright frames hurt on the 3D screen, see session.cpp); strike timing open | session.cpp |
| Sun disc: ours fades in over the horizon from 5% outside sunrise / sunset, no disc colour | own | sky drawing | differs on purpose; wsunon only exposes OpenMW's flag | session.cpp |
| Transitions paused while a menu is open | not checked | World::update does not know pause | open | world.cpp |
| Storm direction origin (OpenMW 25000, 70000; ours 20480, 69632) | own constants | sessions's walk slow-down | open (not weather timing; outside this task) | session.cpp |
