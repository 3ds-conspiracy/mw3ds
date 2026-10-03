# Weather and daylight

Source: OpenMW `apps/openmw/mwworld/weather.cpp` (`WeatherManager`, `Weather`, `RegionWeather`,
`TimeOfDayInterpolator`), `mwscript/skyextensions.cpp` (the script functions), `mwworld/datetimemanager.cpp` (the
clock). Every number below comes from the Morrowind.ini `[Weather ...]` / `[Weather]` sections that OpenMW reads as
"fallback" values, plus the GMST `fStromWindSpeed` (0.7). Oracle: `tools/test/specgen_weather.py`
-> `tools/test/cases/openmw-spec-weather.txt`. What was found: `spec/findings/weather.md`.

## Constants (vanilla)

Day boundaries: sunrise 6, sunrise duration 2, sunset 18, sunset duration 2. OpenMW derives from them
`nightEnd = 6`, `dayStart = 8`, `dayEnd = 18`, `nightStart = 20`. A weather change is considered every 20 game hours.
Each of the four blended channels has its own lead and lag around the boundaries (hours before sunrise, after
sunrise, before sunset, after sunset):

| Channel | pre-sunrise | post-sunrise | pre-sunset | post-sunset |
|---|---|---|---|---|
| Sky | 0.5 | 1 | 1.5 | 0.5 |
| Ambient | 0.5 | 2 | 1 | 1.25 |
| Fog | 0.5 | 1 | 2 | 1 |
| Sun | 0 | 0 | 1 | 1.25 |

Per weather the game files give: four colours each for sky, fog, ambient and sun (sunrise, day, sunset, night), a
land fog depth for day and one for night, a transition delta, wind speed, clouds-maximum-percent, glare view, rain
threshold, thunder frequency / threshold / flash decrement, cloud and ambient / rain loop sounds. Order of weathers
(the script id and the index in a region's chance list): 0 Clear, 1 Cloudy, 2 Foggy, 3 Overcast, 4 Rain,
5 Thunderstorm, 6 Ashstorm, 7 Blight, 8 Snow, 9 Blizzard.

## Colour by hour (`TimeOfDayInterpolator::getValue`)

A weather's sky, fog, ambient and sun colours are each a function of the hour through four zones, per channel,
using that channel's lead / lag:

- **Night**: before `nightEnd - preSunrise` or after `nightStart + postSunset`: the night colour.
- **Sunrise window** `[nightEnd - preSunrise, dayStart + postSunrise]`, length D, middle M: up to M the colour goes
  from night to the sunrise colour (blend factor `2 (M - h) / D` toward night: the sunrise colour at M, the night
  colour at the window's start); after M from the sunrise colour to the day colour (factor `2 (h - M) / D`
  toward day). If D is 0, the first half is night, the second day.
- **Day**: strictly between the end of the sunrise window and `dayEnd - preSunset`: the day colour.
- **Sunset window** `[dayEnd - preSunset, nightStart + postSunset]`: day to sunset colour up to the middle, then
  sunset to night. Same shape as sunrise.

Windows are closed on both ends, so the boundary hour itself belongs to the window (a continuous value either way).
The sky and fog colours are in 0..1 per channel (alpha ignored). Examples (vanilla Sky): the window is 5.5..9 with
middle 7.25; the sunset window 16.5..20.5 with middle 18.5; fog sunset 16..21.

**Fog depth** uses the same interpolator with the "Fog" lead / lag, but the weather's sunrise, day and sunset values
are all the day depth and only the night is the night depth: depth is day depth from the middle of sunrise to the
middle of sunset, falling to the night depth toward night. The renderer scales it (`configureFog`,
`mwrender/renderingmanager.cpp`, not covered here). The underwater fog likewise blends four `Water_Underwater*Fog`
values by the "Fog" windows. Distant-land fog factor / offset per weather are OpenMW's own numbers (Clear 1 / 0,
Cloudy 0.9 / 0, Foggy 0.2 / 30, Overcast 0.7 / 0, Rain 0.5 / 10, Thunderstorm 0.5 / 20, Ashstorm 0.2 / 50,
Blight 0.2 / 60, Snow 0.5 / 40, Blizzard 0.16 / 70); they only matter if distant land is drawn.

**Night flag** (`mNight`): hour before sunrise, or after `nightStart + starsPostSunsetStart - starsFadingDuration`
(= 19 with vanilla 1 and 2). **Star fade** is a fifth interpolation (0 by day, 1 at night) with lead / lag 2 / 0 around sunrise and 1 / 1
around sunset (from the star settings and the 2-hour fading duration).

## Sun, glare, and sun disc

- Sun is enabled for `sunrise < h < nightStart` (6 < h < 20), disabled at both boundaries and beyond.
- Sun direction: the arc runs from one horizon at sunrise to the other at nightStart (`orbit` from +1 to -1 across the
  day; across the night from -1 to +1), so the sun is "level with the horizon" at 6 and 20, not at day start / end.
  Fixed vector `(-400 orbit, 75, -100)`. The "night" flag for lighting is `h >= nightStart` or `h < sunrise`.
- Glare fade (a multiplier on the weather's glare view): 0 before sunrise and after nightStart, a triangle with the
  peak 1 at the midpoint (13) of 6..20. Final glare = weather glare view * fade.
- Sun visibility during a transition: the glare view blends from the current to the next only for the first part
  of the transition (factor below the next weather's clouds-maximum-percent), then stays on the current.
- Sun disc colour: white before `dayEnd - sunPreSunset` (17); after, a blend toward the weather's disc-sunset colour
  over one hour, then an ambient-boosted clamp; alpha 1 by day, fading with the square of
  `(h - dayEnd) / (nightStart - dayEnd)` after dayEnd, and rising from 0 at sunrise by one per hour.
- `getSunPercentage(h)`: 0 outside `(nightEnd, nightStart)`, rising over the sunrise duration, 1 in day, falling
  over the sunset duration.

## Torches and night / day mode

- `useTorches(h)`: dark (`h < sunrise` or `h > nightStart`) and not precipitation. Precipitation means the weather
  has rain (Rain, Thunderstorm) or a particle effect (Snow, Blizzard, Blight), except Ashstorm, which is expressly
  excluded (a quirk of the original, kept).
- Night / day mode: exterior and not day (`h < 6` or `h > 20`) is "exterior night"; interior while day (6..20) with
  the current weather's glare view >= 0.5 is "interior day" (the windows look bright); else default. (The current
  weather decides, not the transition's blend.)

## Weather transitions

State: current weather, next weather (none outside a transition), queued weather (none), transition factor
(1 at the start, falling to 0), update timer (game hours).

- **Starting** (`addWeatherTransition`): if no transition is running and the new weather differs from the current,
  next = new, factor 1. If one is running and the new weather differs from next, queued = new (replacing an earlier
  queued one). Otherwise nothing.
- **Speed**: the factor falls by `realSeconds * delta` of the *next* weather (delta in Hz of real time: vanilla
  0.015 = 67 s; Thunderstorm and Blizzard 0.03 = 33 s; Ashstorm 0.035, Blight 0.04). Game time speed does not matter.
- **Finishing**: at factor <= 0, current = next, next = queued, queued = none. If a queued weather becomes next, it
  starts with the leftover time (converted to the new delta). The blend used by the renderer is `1 - factor`:
  0 = all current, 1 = all next.
- **Fast forward**: when the game skips time (rest, wait, jail, travel, training: `advanceTime` with a
  non-incremental step) the transition is not played: current = queued if any, else next, everything else cleared.
- **Paused**: transitions do not advance while the game is paused (menus) unless a fast-forward is pending.
- **During a transition** the colours, fog depth, wind, glare, cloud speed, distant fog factor are linear blends of
  the two weathers' own values at that hour; the cloud blend factor is `blend / next.cloudsMaximumPercent`.
  Precipitation, storm flag, particle / rain effect, loop sounds and rain geometry do not blend: they belong to the
  current weather while `blend < threshold` and to the next after, where threshold is the *next* weather's rain
  threshold (0.5 if that is 0 or unset: the ini's Storm / Snow thresholds are ignored; only Rain and Thunderstorm
  carry 0.6). Loop volume and precipitation alpha fade `1 - blend/threshold` out, then
  `(blend - threshold)/(1 - threshold)` in.
- **Scripts**: GetCurrentWeather returns the script id of the *current* weather, which during a transition is still the
  outgoing weather (the new one shows only when the transition ends).

## Region weather, and the 20-hour re-roll

- A region holds chances per weather and its current weather (empty until first needed). Rolling: pick a number
  1..100 and walk the weathers in the order 0..9 adding each one's chance until the number is <= the running sum;
  if the chances sum to under 100 and the roll is above the sum, Clear (index 0). Chances above 100 in total mean
  later weathers can never be reached (the roll stops first).
- **Expiry**: every 20 game hours (`Hours Between Weather Changes`; the timer is counted down in game hours
  passed, including time skipped by resting) every region's weather is cleared; the current region immediately
  rolls a new weather and starts a transition to it (queued if one is running). A weather set by ChangeWeather
  therefore only lasts until that expiry.
- **Entering a region** (travel / teleport to an exterior cell of another region, `playerTeleported`): the weather
  becomes the region's (rolled if empty) at once, with no transition and the pending ones discarded. Walking over a
  region border in the exterior (`updateWeatherRegion`) starts a transition to the new region's weather. Cells with
  no region (most interiors) do not change the current region.
- The initial weather is Clear.

## Script functions

- **ChangeWeather region, id**: if the region does not exist, a warning is shown and nothing else happens. If the
  weather number is not 0..9 nothing happens (silently). Otherwise the region's current weather is set to it
  (stored: the region keeps it until the next expiry or ModRegion), and if the region is the player's current one a
  transition is started or queued as above. For another region the weather is only stored.
- **GetCurrentWeather**: see above (the current weather's number).
- **ModRegion region, c0 .. cN**: replaces the region's whole chance list: weather i gets the i-th argument clamped to
  0..100; weathers not given get 0 (not their old value); extra arguments are dropped. If the region's current weather
  now has chance 0 (or none is set) a new one is rolled. If the region is the current one, a transition starts or
  is queued to the region's weather (nothing if it equals the current / next). The change is permanent (saved).
  Unknown region: nothing.
- Weather condition in dialogue (`Weather` select) uses the same current-weather number; per audit D7, it fails
  in interiors.

## Interiors

`update()` still runs the transition timers when the player is in an interior (a ChangeWeather for the last exterior
region proceeds). But for interiors (not exterior or quasi-exterior) it then switches the sky off, stops the
ambient / rain / thunder sounds, sets all wind speeds to 0, and returns before computing any colour, fog, ambient or
sun: the cell's own lighting applies. Exteriors compute the whole result every frame. A quasi-exterior (an interior
cell flagged as such, e.g. Mournhold) counts as exterior.

## Wind, storms, thunder

- Storm flag: the current weather's wind speed > `fStromWindSpeed` (0.7): Ashstorm, Blight and Blizzard.
- Target wind speed: `min(8 * windSpeed, 70)`. Each frame adds a random step of up to +-50% (rain weathers
  +-25%) of the target to the current speed, accepted only if the result stays between 0.5 and 2 times the target;
  the first value is the target itself. During a transition: blend of the two weathers' separately gusting speeds.
- Storm direction (for ash / blight): from Red Mountain (25000, 70000, 0) toward the player's xy position,
  normalised; else (0, 1, 0).
- Thunder: only when the (blend or 1) is at least the Thunder Threshold and frequency > 0. Per frame chance
  `freq * 10/60 * dt * (ratio - threshold) / (1 - threshold)`. A strike picks one of four sounds uniformly; flash
  brightness += `1 - 0.25 * index`, decaying at `Flash Decrement` per second; the flash is added to fog, ambient and
  sun colours. Paused: no strikes, flash held.
- Precipitation, rain loops: Rain and Thunderstorm use rain sound `rain` (a weather's Rain Loop Sound ID overrides;
  "None" silences it); a weather's Ambient Loop Sound ID, when set, plays as a loop ("None" silences it).

## Time (`datetimemanager.cpp`)

The game hour is a float in 0..24 (setHour carries whole days into the day counter); game time per real second is
`timescale` game seconds, where `timescale` is the global of that name (30 by default: a day = 48 real minutes, an
hour = 2 min). Time skipped by rest / wait is applied in one step and calls `advanceTime(hours, incremental=false)`
for the weather timer. Moon phase: a 3-day cycle, 8 phases, starting full on 16 Last Seed (`MoonModel::phase`);
`GetMasserPhase` / `GetSecundaPhase` return 0..7. Not tested here.
