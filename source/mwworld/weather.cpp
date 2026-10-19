// Weather and the time of day: transitions, colours, fog, sun and light
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>

// OpenMW's RegionWeather::chooseNewWeather: a number 1..100 walks the chances in weather order; chances that
// sum to under 100 leave Clear for the rest
int World::rollWeather(const std::string& region)
{
	int roll = rand() % 100 + 1, sum = 0;
	for (int k = 0; k <= WEATHER_BLIZZARD; k++)
		if (roll <= (sum += regionChance(region, k)))
			return k;
	return WEATHER_CLEAR;
}

int World::regionChance(const std::string& region, int weather) const
{
	auto mod = regionChances.find(region);
	if (mod != regionChances.end())
		return weather >= 0 && weather < (int)mod->second.size() ? mod->second[weather] : 0;
	auto def = game.regions.find(region);
	return def != game.regions.end() && weather >= 0 && weather < 10 ? def->second.chances[weather] : 0;
}

int World::regionWeather(const std::string& region)
{
	auto f = forcedWeather.find(region);
	if (f == forcedWeather.end())
		f = forcedWeather.emplace(region, rollWeather(region)).first;
	return f->second;
}

// OpenMW's addWeatherTransition: begins now when nothing is changing, else waits behind the change (a later one
// replaces an earlier wait; the one already coming is ignored)
void World::addWeatherTransition(int weather)
{
	if (weatherNext == weatherNow)
	{
		if (weather != weatherNow)
		{
			weatherNext = weather;
			weatherBlend = 0.0f;
		}
	}
	else if (weather != weatherNext)
		weatherQueued = weather;
}

void World::forceWeather(int weather)
{
	weatherNow = weatherNext = weather;
	weatherQueued = -1;
	weatherBlend = 1.0f;
}

void World::setWeatherHere(int weather)
{
	const LevelCell& lc = cells[current];
	if (!lc.interior && !lc.region.empty())
		weatherRegion = lc.region;
	weatherCell = current;
	if (!weatherRegion.empty())
		forcedWeather[weatherRegion] = weather;
	forceWeather(weather);
}

// ChangeWeather: an unknown region or weather does nothing; the weather is the region's until the next expiry,
// and changes at once (or queues) when the region is the player's
void World::changeWeather(const std::string& region, int weather)
{
	if (!game.regions.count(region) || weather < 0 || weather > WEATHER_BLIZZARD)
		return;
	forcedWeather[region] = weather;
	if (region == weatherRegion)
		addWeatherTransition(weather);
}

// ModRegion: the new chances replace the old; the region's weather is rerolled if they no longer allow it
void World::modRegion(const std::string& region, const std::vector<int>& chances)
{
	if (!game.regions.count(region))
		return;
	regionChances[region] = chances;
	auto f = forcedWeather.find(region);
	if (f == forcedWeather.end() || regionChance(region, f->second) == 0)
		forcedWeather[region] = rollWeather(region);
	if (region == weatherRegion)
		addWeatherTransition(forcedWeather[region]);
}

static float weatherDelta(const GameData& g, int w)
{
	return fmaxf(0.001f, g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)].delta);
}

// One gust step (OpenMW calculateWindSpeed): the wind drifts by up to half of what the weather aims for (a
// quarter in rain), never under half or over double it
static float gust(const WeatherType& t, float speed)
{
	float target = fminf(8.0f * t.wind, 70.0f);
	// (a speed left over from another weather, outside this one's range, would never drift back into it)
	if (speed == 0.0f || speed < 0.5f * target || speed > 2.0f * target)
		speed = target;
	float next = (rand() / (float)RAND_MAX - 0.5f) * (t.precip ? 0.5f : 1.0f) * target + speed;
	return next > 0.5f * target && next < 2.0f * target ? next : speed;
}

void World::updateWeather(float dt)
{
	if (game.weatherTypes.empty() || current < 0)
		return;
	int n = (int)game.weatherTypes.size();
	if (weatherSeen < 0.0f)
		weatherSeen = gameHour;
	// the hours gone by since the last look: this frame's, and any skipped over (rest, wait, travel: a skip
	// finishes the change in progress at once)
	float inc = dt * timescale() / 3600.0f;
	float skipped = gameHour - weatherSeen - inc;
	bool fast = fabsf(skipped) > 0.01f;
	weatherSeen = gameHour;
	weatherTimer -= inc + fmaxf(0.0f, skipped);
	bool changed = false;
	if (weatherTimer <= 0.0f)
	{
		// every region forgets its weather; the player's rolls a new one
		forcedWeather.clear();
		weatherTimer += game.weatherHours;
		changed = true;
	}
	const LevelCell& lc = cells[current];
	if (!lc.interior && !lc.region.empty() && lc.region != weatherRegion)
	{
		// walking over a border: a change to the new region's weather; arriving any other way (a door, travel):
		// that weather at once
		bool walked = !weatherRegion.empty() && weatherCell >= 0 && !cells[weatherCell].interior
			&& abs(cells[weatherCell].gx - lc.gx) <= 1 && abs(cells[weatherCell].gy - lc.gy) <= 1;
		weatherRegion = lc.region;
		if (game.regions.count(weatherRegion) && !walked)
		{
			forceWeather(regionWeather(weatherRegion));
			changed = false;
		}
		else
			changed = true;
	}
	weatherCell = current;
	if (changed && game.regions.count(weatherRegion))
		addWeatherTransition(regionWeather(weatherRegion));
	// the change: at the next weather's delta per real second, the time left over going to the one queued
	if (fast)
	{
		weatherNow = weatherQueued >= 0 ? weatherQueued : weatherNext;
		weatherNext = weatherNow;
		weatherQueued = -1;
		weatherBlend = 1.0f;
	}
	else if (weatherNext != weatherNow)
	{
		weatherBlend += dt * weatherDelta(game, weatherNext);
		if (weatherBlend >= 1.0f)
		{
			float over = (weatherBlend - 1.0f) / weatherDelta(game, weatherNext);
			weatherNow = weatherNext;
			weatherNext = weatherQueued >= 0 ? weatherQueued : weatherNext;
			weatherQueued = -1;
			weatherBlend = weatherNext != weatherNow ? fminf(0.999f, over * weatherDelta(game, weatherNext)) : 1.0f;
		}
	}
	// wind: none indoors; outdoors each weather gusts about what it aims for, the two blending while changing
	if (lc.interior)
		windCur = windNxt = windNow = 0.0f;
	else
	{
		const WeatherType& now = game.weatherTypes[std::min(weatherNow, n - 1)];
		if (weatherNext == weatherNow)
		{
			windCur = windNow = gust(now, windNow);
			windNxt = 0.0f;
		}
		else
		{
			const WeatherType& next = game.weatherTypes[std::min(weatherNext, n - 1)];
			windCur = gust(now, windCur);
			windNxt = gust(next, windNxt);
			windNow = windCur * (1.0f - weatherBlend) + windNxt * weatherBlend;
		}
	}
}

// OpenMW's TimeOfDayInterpolator (weather.cpp): which two of a weather's four values (0 sunrise, 1 day, 2 sunset,
// 3 night) hour h blends for colour channel ch (0 sky, 1 fog, 2 ambient, 3 sun), and how much of the second. Each
// channel has its own lead and lag around the day's boundaries
static void timeOfDay(const WeatherClock& c, int ch, float h, int& a, int& b, float& f)
{
	float preSr = c.win[ch][0], postSr = c.win[ch][1], preSs = c.win[ch][2], postSs = c.win[ch][3];
	float nightEnd = c.sunrise, dayStart = c.sunrise + c.sunriseDur, dayEnd = c.sunset, nightStart = c.sunset + c.sunsetDur;
	a = b = 3;
	f = 0.0f;
	if (h < nightEnd - preSr || h > nightStart + postSs)
		return;
	if (h <= dayStart + postSr)
	{
		float dur = dayStart + postSr - nightEnd + preSr, mid = nightEnd - preSr + dur / 2.0f;
		a = 0;
		b = h <= mid ? 3 : 1;
		f = dur > 0.0f ? fabsf(mid - h) / dur * 2.0f : h <= mid ? 0.0f : 1.0f;
		return;
	}
	if (h < dayEnd - preSs)
	{
		a = b = 1;
		return;
	}
	float dur = nightStart + postSs - dayEnd + preSs, mid = dayEnd - preSs + dur / 2.0f;
	a = 2;
	b = h <= mid ? 1 : 3;
	f = dur > 0.0f ? fabsf(mid - h) / dur * 2.0f : h <= mid ? 0.0f : 1.0f;
}

// One weather's colour channel at hour h
static float colourAt(const GameData& g, int w, int kind, int c, float h)
{
	int a, b;
	float f;
	timeOfDay(g.weatherClock, kind, h, a, b, f);
	const WeatherType& t = g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)];
	return t.colors[kind][a][c] * (1.0f - f) + t.colors[kind][b][c] * f;
}

// One weather's land fog depth at hour h: the day's from sunrise to sunset, only the night's differs (OpenMW passes
// the day depth for sunrise, day and sunset)
static float fogDepthAt(const GameData& g, int w, float h)
{
	int a, b;
	float f;
	timeOfDay(g.weatherClock, 1, h, a, b, f);
	const WeatherType& t = g.weatherTypes[std::min(w, (int)g.weatherTypes.size() - 1)];
	return t.fog[a == 3 ? 1 : 0] * (1.0f - f) + t.fog[b == 3 ? 1 : 0] * f;
}

float World::weatherColour(int kind, int c) const
{
	if (game.weatherTypes.empty())
		return game.weather[kind][1][c];
	float h = fmodf(gameHour, 24.0f);
	return colourAt(game, weatherNow, kind, c, h) * (1.0f - weatherBlend) + colourAt(game, weatherNext, kind, c, h) * weatherBlend;
}

float World::weatherFogDepth() const
{
	if (game.weatherTypes.empty())
		return 0.69f;
	float h = fmodf(gameHour, 24.0f);
	return fogDepthAt(game, weatherNow, h) * (1.0f - weatherBlend) + fogDepthAt(game, weatherNext, h) * weatherBlend;
}

// Morrowind.ini [Water] Underwater Sunrise / Day / Sunset / Night Fog (3, 2.5, 3, 4) by the fog channel's hours, Indoor
// Fog (3) inside (OpenMW's mUnderwaterFog, FogManager): the fog starts that many views behind the eye less one, so the
// water is murky from the eye and thickens to the view's end
float World::underwaterFog() const
{
	if (current < 0 || cells[current].interior || game.weatherTypes.empty())
		return 3.0f;
	static const float kFog[4] = { 3.0f, 2.5f, 3.0f, 4.0f };
	int a, b;
	float f;
	timeOfDay(game.weatherClock, 1, fmodf(gameHour, 24.0f), a, b, f);
	return kFog[a] * (1.0f - f) + kFog[b] * f;
}

int World::weatherShown() const
{
	if (weatherNext == weatherNow || game.weatherTypes.empty())
		return weatherNow;
	float thr = game.weatherTypes[std::min(weatherNext, (int)game.weatherTypes.size() - 1)].rainThreshold;
	return weatherBlend < (thr > 0.0f ? thr : 0.5f) ? weatherNow : weatherNext;
}

bool World::weatherPrecip() const
{
	int w = weatherShown();
	if (game.weatherTypes.empty() || w == WEATHER_ASH)      // (OpenMW: an ash storm does not count)
		return false;
	return game.weatherTypes[std::min(w, (int)game.weatherTypes.size() - 1)].precip || w >= WEATHER_BLIGHT;
}

bool World::weatherStorm() const
{
	return !game.weatherTypes.empty()
		&& game.weatherTypes[std::min(weatherShown(), (int)game.weatherTypes.size() - 1)].wind > game.gmstf("fstromwindspeed", 0.7f);
}

float World::sunVisibility() const
{
	if (game.weatherTypes.empty())
		return 1.0f;
	int n = (int)game.weatherTypes.size();
	const WeatherType& now = game.weatherTypes[std::min(weatherNow, n - 1)];
	if (weatherNext != weatherNow)
	{
		const WeatherType& next = game.weatherTypes[std::min(weatherNext, n - 1)];
		float left = 1.0f - weatherBlend;
		if (left < next.cloudsMax)
		{
			float t = left / next.cloudsMax;
			return (1.0f - t) * now.glare + t * next.glare;
		}
	}
	return now.glare;
}

float World::glareFade() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f), end = c.sunset + c.sunsetDur, peak = c.sunrise + (end - c.sunrise) / 2.0f;
	if (h < c.sunrise || h > end)
		return 0.0f;
	return h < peak ? 1.0f - (peak - h) / (peak - c.sunrise) : 1.0f - (h - peak) / (end - peak);
}

bool World::sunOn() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	return !(h >= c.sunset + c.sunsetDur || h <= c.sunrise);
}

// Time for torches (OpenMW's World::useTorches): outdoors at night with nothing falling; indoors where the cell's
// ambient light is dim (its channels add up to 201 or less) and resting there is allowed
bool World::useTorches() const
{
	if (current >= 0 && cells[current].interior)
	{
		const LoadedCell* l = cells[current].live;
		return l && !cells[current].noSleep && l->cell.ambient[0] + l->cell.ambient[1] + l->cell.ambient[2] <= 201;
	}
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	return (h < c.sunrise || h > c.sunset + c.sunsetDur) && !weatherPrecip();
}

// OpenMW's night / day mode: by the current weather's glare, not the blend
int World::nightDay() const
{
	const WeatherClock& c = game.weatherClock;
	float h = fmodf(gameHour, 24.0f);
	bool day = h >= c.sunrise && h <= c.sunset + c.sunsetDur, outdoors = current >= 0 && !cells[current].interior;
	if (outdoors && !day)
		return 2;
	float glare = game.weatherTypes.empty() ? 1.0f : game.weatherTypes[std::min(weatherNow, (int)game.weatherTypes.size() - 1)].glare;
	return !outdoors && day && glare >= 0.5f ? 1 : 0;
}

bool World::underWater(float z) const
{
	for (const LoadedCell* l : loaded)
		if (l->cell.hasWater() && z < l->cell.waterZ)
			return true;
	return false;
}

// The share of the view drawn in this weather outdoors: 0.69 (clear weather's day fog depth, which the view distance is
// tuned for) over the weather's land fog depth, a quarter at least: an ash storm (1.1) draws 0.63 of it, rain 0.86.
// Differs from OpenMW on purpose, for speed: OpenMW draws the whole view and only moves the fog's start toward
// the eye (weatherFogStart, which still applies over the shorter view, so the fog is whole where the drawing stops).
// Drawing land hidden in thick fog cost both consoles their frame rate in rain and storms
float World::weatherFogScale() const
{
	if (game.weatherTypes.empty() || current < 0 || cells[current].interior)
		return 1.0f;
	return fmaxf(0.25f, fminf(1.0f, 0.69f / fmaxf(0.1f, weatherFogDepth())));
}

// Where the land fog starts outdoors, a share of where it ends: OpenMW's FogManager puts it at the view distance x
// (1 - the weather's land fog depth), the end at the view distance (Morrowind.ini Land Fog Day / Night Depth: 0.69
// clear, 1.0 fog, 1.1 an ash storm, 1.2 blight: fogged from the eye). Below -1.5 without weathers
float World::weatherFogStart() const
{
	if (game.weatherTypes.empty() || current < 0 || cells[current].interior)
		return -2.0f;
	return fmaxf(-1.0f, fminf(0.95f, 1.0f - weatherFogDepth()));
}

// The weather's colours for the hour (OpenMW's interpolation: each channel its own sunrise / sunset windows)
void World::daylight(float land[3], float sky[3], float fog[3]) const
{
	for (int c = 0; c < 3; c++)
		land[c] = sky[c] = 1.0f;
	if (!game.hasWeather || current < 0 || cells[current].interior)
	{
		for (int c = 0; c < 3; c++)
			fog[c] = cells[current].live ? cells[current].live->cell.fog[c] / 255.0f : 0.0f;
		return;
	}
	// The baked light is the day's: scale by now's ambient + sun against the day's (never pitch black)
	for (int c = 0; c < 3; c++)
	{
		fog[c] = weatherColour(1, c);
		float day = game.weather[2][1][c] + 0.6f * game.weather[3][1][c];
		land[c] = fmaxf(0.02f, fminf(1.2f, (weatherColour(2, c) + 0.6f * weatherColour(3, c)) / fmaxf(0.01f, day)));
		sky[c] = fmaxf(0.05f, fminf(1.5f, weatherColour(0, c) / fmaxf(0.01f, game.weather[0][1][c])));
	}
}

// OpenMW's WeatherManager::update: the light runs east to west along (400 x orbit, -75, 100), orbit 1 at sunrise to
// -1 at night's start, and through the night back from the west (-1) to the east (1), in the weather's sun colour of
// the hour (its night colour by night). The cells bake its noon place (overhead, to the south); this is its east-west
// share, 400 x |orbit| / its length, on a wall square to it
void World::sunSide(float out[4]) const
{
	out[0] = out[1] = out[2] = out[3] = 0.0f;
	if (!game.hasWeather || current < 0 || cells[current].interior)
		return;
	const WeatherClock& wc = game.weatherClock;
	float h = fmodf(gameHour, 24.0f), nightStart = wc.sunset + wc.sunsetDur;
	if (h < wc.sunrise)
		h += 24.0f;
	if (nightStart < wc.sunrise)
		nightStart += 24.0f;
	float day = fmaxf(0.1f, nightStart - wc.sunrise), night = fmaxf(0.1f, 24.0f - day);
	float orbit = h < nightStart ? 1.0f - 2.0f * (h - wc.sunrise) / day : 2.0f * (h - nightStart) / night - 1.0f;
	float k = 400.0f * fabsf(orbit) / sqrtf(160000.0f * orbit * orbit + 75.0f * 75.0f + 100.0f * 100.0f);
	for (int c = 0; c < 3; c++)
		out[c] = weatherColour(3, c) * k;
	out[3] = orbit > 0.0f ? 1.0f : orbit < 0.0f ? -1.0f : 0.0f;
}
