// The sky: sun, moons, stars, clouds, and the rain, snow and ash falling
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include <malloc.h>
#include <citro3d.h>
#include "mwrender/renderhelpers.hpp"

// The weather's ambient loop outdoors (the one it's changing to past halfway), thunder in storms
C3D_Tex* Session::skyTex(const std::string& name)
{
	if (name.empty())
		return nullptr;
	auto it = skyTextures.find(name);
	if (it != skyTextures.end())
		return it->second;
	return skyTextures[name] = w.textures.acquire(w.dataDir, name);
}

// Where the sun and the moons are (OpenMW's reading of Morrowind's sky): the sun rises in the east at
// Sunrise Time and sets in the west at Sunset Time; each moon rises Daily Increment hours later every
// day (from 16 Last Seed), climbs 15 x Speed degrees an hour over the sky on a path turned by Axis Offset,
// and goes through its 8 phases 3 days each, full on the first day; they fade in and out by the hour.
// Clouds, rain and storms hide them.
void Session::updateSky()
{
	g_skyBillboards.clear();
	g_starAlpha = 0.0f;
	if (w.current < 0 || w.cells[w.current].interior)
		return;
	const SkyBodiesDef& sky = w.game.skyBodies;
	GameDate d = w.date();
	float h = d.hour;
	float clear = w.weatherNow <= 1 ? 1.0f : w.weatherNow <= 3 ? 0.35f : 0.1f;   // clear / cloudy, foggy / overcast, worse
	// the sun: from sunrise to the end of sunset (OpenMW's night start), along Morrowind's fixed line across the sky,
	// (400 x orbit, -75, 100), orbit 1 at sunrise to -1 at night (OpenMW's WeatherManager::update)
	const WeatherClock& wc = w.game.weatherClock;
	float dayLength = fmaxf(1.0f, wc.sunset + wc.sunsetDur - wc.sunrise);
	float st = (h - wc.sunrise) / dayLength;
	if (st > 0.0f && st < 1.0f)
	{
		// (sizes as OpenMW's: the sun a 450-unit quad 1000 away, its flash 2.6 times that, the moons 450 x size / 125;
		// the billboards here sit 2700 away and take half their width)
		SkyBillboard sun = {};
		float orbit = 1.0f - 2.0f * st, len = sqrtf(160000.0f * orbit * orbit + 75.0f * 75.0f + 100.0f * 100.0f);
		sun.dir[0] = 400.0f * orbit / len;
		sun.dir[1] = -75.0f / len;
		sun.dir[2] = 100.0f / len;
		sun.size = 0.5f * 450.0f * 2.7f;
		sun.tex = skyTex(sky.sun);
		float edge = fminf(1.0f, fminf(st, 1.0f - st) * 20.0f);
		u8 a = (u8)(255 * clear * fmaxf(0.0f, edge));
		sun.rgba = 0x00FFFFFF | ((u32)a << 24);
		g_skyBillboards.push_back(sun);
		SkyBillboard glare = sun;
		glare.size = sun.size * 2.6f;              // the sun flash (tx_sun_flash_grey_05), white, as bright as the sun
		glare.tex = skyTex(sky.glare);
		glare.additive = false;                    // (blended by its own alpha, as OpenMW's sky draws it)
		glare.afterClouds = true;
		glare.rgba = 0x00FFFFFF | ((u32)a << 24);
		g_skyBillboards.push_back(glare);
	}
	// the moons (OpenMW's MoonModel): each rises Daily Increment hours later every day, in a 24-day loop that skips the
	// increments a day can't hold, and climbs 15 x Speed degrees an hour (at least half a turn in 23 hours) from the
	// north over to the south, its path turned by its Axis Offset (rising west of north). It is a disk in the sky's
	// own colour with its phase's picture added over it (OpenMW's moon pass in sky.frag): by day a faint pale disk,
	// by night the picture on the dark sky, hiding the stars behind it. The picture shows from Fade End Angle up its
	// path, whole from Fade Start Angle; the disk is gone below Moon Shadow Early Fade Angle under Fade End; by the
	// hour both fade in and out (Fade In / Out Start, Finish), and by the weather's Glare View (none overcast)
	float glareView = w.sunVisibility();
	for (int m = 0; m < 2; m++)
	{
		const MoonDef& md = sky.moons[m];
		moonAlpha[m] = 0.0f;
		float speed = fmaxf(md.speed, 180.0f / 23.0f / 15.0f), inc = fmodf(md.dailyIncrement, 24.0f);
		auto rotation = [&](float hours) { return 15.0f * speed * hours; };
		auto riseHour = [&](int days) {
			if (inc == 0.0f)
				return 0.0f;
			float offset = (24.0f - fabsf(24.0f / inc)) * floorf((days + 16) / 24.0f);
			return inc + fmodf((days - 1 + 16 - offset) * inc, 24.0f);
		};
		auto angleAt = [&](int days, float hour) {
			float riseToday = riseHour(days), a = 0.0f;
			if (hour < riseToday)
			{
				float riseYesterday = riseToday - inc;
				if (riseYesterday < 24.0f)
				{
					float early = md.fadeEndAngle - md.earlyFadeAngle;
					float cycle = riseYesterday + early / rotation(1.0f) > 24.0f ? inc : 0.0f;
					float yesterday = rotation(24.0f - (riseYesterday + cycle));
					if (yesterday < 180.0f)
						a = rotation(hour) + yesterday;
				}
			}
			else
				a = rotation(hour - riseToday);
			return a >= 180.0f ? 0.0f : a;
		};
		auto shadowAlpha = [&](float a) {        // the disk's own fade at the ends of its path
			float a1 = md.fadeEndAngle - md.earlyFadeAngle, e2 = 180.0f - md.fadeEndAngle, a2 = e2 + md.earlyFadeAngle;
			if (a >= a1 && a < md.fadeEndAngle)
				return (a - a1) / fmaxf(0.01f, md.earlyFadeAngle);
			if (a >= md.fadeEndAngle && a < e2)
				return 1.0f;
			if (a >= e2 && a < a2)
				return (a2 - a) / fmaxf(0.01f, md.earlyFadeAngle);
			return 0.0f;
		};
		auto hourAlpha = [&](float hour) {
			float outEnd = md.fadeOutFinish - 0.0167f;      // (culled a minute early, as Morrowind does)
			if (hour >= md.fadeOutStart && hour < outEnd)
				return (outEnd - hour) / fmaxf(0.01f, outEnd - md.fadeOutStart);
			if (hour >= outEnd && hour < md.fadeInStart)
				return 0.0f;
			if (hour >= md.fadeInStart && hour < md.fadeInFinish)
				return (hour - md.fadeInStart) / fmaxf(0.01f, md.fadeInFinish - md.fadeInStart);
			return 1.0f;
		};
		int day = d.daysPassed;
		float angle = angleAt(day, h);
		// the phase changes once the moon is out of sight at midnight, or a day's increment after it went
		float phaseHour = 0.0f;
		if (hourAlpha(0.0f) > 0.0f && shadowAlpha(angleAt(day, 0.0f)) > 0.0f)
			phaseHour = (180.0f - md.fadeEndAngle + md.earlyFadeAngle - angleAt(day, 0.0f)) / rotation(1.0f) + fmaxf(inc, 0.0f);
		int phase = h < phaseHour ? (day / 3) % 8 : ((day + 1) / 3) % 8;
		float fade = md.fadeStartAngle - md.fadeEndAngle, textured = 0.0f;
		if (angle >= md.fadeEndAngle && angle < md.fadeStartAngle)
			textured = (angle - md.fadeEndAngle) / fmaxf(0.01f, fade);
		else if (angle >= md.fadeStartAngle && angle < 180.0f - md.fadeStartAngle)
			textured = 1.0f;
		else if (angle >= 180.0f - md.fadeStartAngle && angle < 180.0f - md.fadeEndAngle)
			textured = (180.0f - md.fadeEndAngle - angle) / fmaxf(0.01f, fade);
		// (Moon::setState: north turned up its path by the angle first, then about the vertical by the Axis Offset, in
		// osg's order, so the angle is its height over the horizon; its quad faces the eye, the picture's top edge
		// ahead along its path, its u along the offset's east)
		float a = angle * 0.0174533f, z = md.axisOffset * 0.0174533f;
		moonDir[m][0] = -sinf(z) * cosf(a);
		moonDir[m][1] = cosf(z) * cosf(a);
		moonDir[m][2] = sinf(a);
		float alpha = glareView * hourAlpha(h) * shadowAlpha(angle);
		moonAlpha[m] = alpha;
		if (alpha <= 0.01f)
			continue;
		SkyBillboard mb = {};
		for (int k = 0; k < 3; k++)
			mb.dir[k] = moonDir[m][k];
		mb.fixed = true;
		mb.axisU[0] = cosf(z);
		mb.axisU[1] = sinf(z);
		mb.axisU[2] = 0.0f;
		mb.axisV[0] = -sinf(z) * sinf(a);
		mb.axisV[1] = cosf(z) * sinf(a);
		mb.axisV[2] = -cosf(a);
		mb.size = 0.5f * 450.0f * md.size / 125.0f * 2.7f;
		u8 a8 = (u8)(alpha * 255);
		// the disk in the sky's colour, blended by its alpha; then the picture added, as bright as it is textured
		if (!sky.circle[m].empty())
		{
			mb.tex = skyTex(sky.circle[m]);
			u32 c = 0;
			for (int i = 0; i < 3; i++)
				c |= (u32)(fminf(1.0f, fmaxf(0.0f, w.weatherColour(0, i))) * 255) << (8 * i);
			mb.rgba = c | ((u32)a8 << 24);
			g_skyBillboards.push_back(mb);
		}
		mb.tex = skyTex(m == 0 ? sky.masser[phase] : sky.secunda[phase]);
		u8 lit = (u8)(textured * 255);
		mb.rgba = lit | (lit << 8) | (lit << 16) | ((u32)a8 << 24);
		mb.additive = true;
		g_skyBillboards.push_back(mb);
	}
	// the stars: from an hour after sunset to two before sunrise, fading over two hours
	float night = 0.0f;
	float startH = sky.sunset + 1.0f, endH = sky.sunrise - 2.0f;
	if (h >= startH)
		night = fminf(1.0f, (h - startH) / 2.0f);
	else if (h <= endH)
		night = 1.0f;
	else if (h < endH + 2.0f)
		night = 1.0f - (h - endH) / 2.0f;
	g_starAlpha = night * clear;
	g_starTexture = skyTex(sky.stars);
	// the clouds: the weather's own picture, and the next weather's while it changes
	int n = (int)w.game.weatherTypes.size();
	g_cloudTex[0] = n ? skyTex(w.game.weatherTypes[std::min(w.weatherNow, n - 1)].clouds) : nullptr;
	g_cloudTex[1] = n && w.weatherNext != w.weatherNow ? skyTex(w.game.weatherTypes[std::min(w.weatherNext, n - 1)].clouds) : nullptr;
	g_cloudBlend = g_cloudTex[1] ? w.weatherBlend : 0.0f;
	for (C3D_Tex* t : g_cloudTex)
		if (t)
			C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
}

void Session::updateWeatherEffects(float dt)
{
	bool outside = w.current >= 0 && !w.cells[w.current].interior && !w.game.weatherTypes.empty();
	int wt = w.weatherShown();       // (OpenMW: the old weather's loops until the new one's rain threshold)
	std::string want;
	if (outside && wt < (int)w.game.weatherTypes.size())
	{
		want = w.game.weatherTypes[wt].sound;
		if (!want.empty() && !w.game.sounds.count(want))
			want = w.game.sounds.count("ashstorm") && (wt == WEATHER_BLIGHT || wt == WEATHER_ASH) ? "ashstorm" : "";
	}
	if (want != weatherSound || (weatherChannel >= 0 && !audioPlaying(weatherChannel)))
	{
		if (weatherChannel >= 0)
			audioStop(weatherChannel);
		weatherChannel = -1;
		weatherSound = want;
		auto it = w.game.sounds.find(want);
		if (it != w.game.sounds.end())
			weatherChannel = audioPlay(it->second.file, it->second.volume * 0.7f, 0.0f, true);
	}
	precipDt += dt;                  // (drawPrecipitation moves the rain and ash by it)
	if (outside && wt == WEATHER_THUNDER && (thunderIn -= dt) <= 0.0f)
	{
		thunderIn = 8.0f + rand() % 20;
		playSound(-1, "thunder" + std::to_string(rand() % 4));
		fade = fmaxf(fade, 0.0f);            // (no flash: bright frames hurt on the 3D screen)
	}
}

// ---- Rain, ash and blight in the world (OpenMW's SkyManager)
// Each kind is a set of particles in a box round the eye, every one moving on its own; one the eye leaves behind
// wraps to the box's far side (OpenMW's WrapAroundOperator). So they hang in the world: turning the view or walking
// leaves them where they are, and both eyes of the 3D screen see them at their depth. Drawn over the screen they
// moved with the view, flat on the HUD. Rain: OpenMW's box (Rain Diameter 600 across, half of Rain Height Min + Max
// high) falling at Weather_Precip_Gravity (575); ash and blight: ashcloud.nif / blightcloud.nif's flakes, falling
// fast and blown out from Red Mountain, and its dust clouds rolling with the wind, in OpenMW's 1024 x 1024 x 800 box
// (the dust in a wider one: its clouds are 400 across). Fewer than OpenMW's (450 to 650 raindrop batches, 100 flakes,
// 72 clouds), and the clouds fade out near the eye: the console's fill rate (spec/performance-tradeoffs.md). Past the
// streaks' box, rain in the distance: a ring of falling drops round the eye, 450 out, the picture fixed to the world's
// directions (turning is right) and sliding down at the drops' speed. A ring always has a side ahead, so nothing is
// turned edge-on or swapped as the eye turns. Only a band of it, round the horizon (overhead and underfoot the streaks
// are enough): a whole ring is a see-through layer over the whole screen, a band about 0.4 of it (sheets stacked
// across the view, the screen's whole area each). The ring and the ripples are a New 3DS's alone
// (ConsoleProfile::rainExtras)
namespace
{
	enum { PRECIP_RAIN, PRECIP_FLAKES, PRECIP_DUST, PRECIP_LAYERS };
	enum { PTEX_RAIN, PTEX_FLAKE, PTEX_ASH, PTEX_BLIGHT, PTEX_SHEET, PTEX_SPLASH, PTEX_COUNT };
	const int kPrecipMax[PRECIP_LAYERS] = { 450, 240, 24 };
	const float kPrecipBox[PRECIP_LAYERS][3] = { { 420, 420, 380 }, { 600, 600, 450 }, { 1100, 1100, 400 } };
	const float kPrecipLift[PRECIP_LAYERS] = { 40, 100, 120 };   // the box's centre over the eye
	const int kRingSides = 16, kRings = 1;
	const float kRingRadius[kRings] = { 450.0f };
	const int kRingRepeats[kRings] = { 16 };       // the picture round the ring (128 x 256 texels: 177 units across)
	// Drops landing: a ripple spreading on the ground or the water where one hit, round the eye (OpenMW ripples water
	// alone, and only with its shaders; on the street they're what makes rain read as rain at this size)
	const int kSplashes = 40;
	const float kSplashLife = 0.35f;
	struct Splash { float pos[3]; float age; };
	Splash s_splash[kSplashes];
	float s_splashDue = 0.0f;
	const int kPrecipQuads = 450 + 240 + 24 + kRings * kRingSides * 2 + kSplashes;
	float s_precipClock = 0.0f;
	struct PrecipLayer
	{
		float pos[450][3];
		float pace[450];          // of the layer's speed (OpenMW's speed variance: a quarter either way)
		bool seeded = false;
	};
	PrecipLayer s_precip[PRECIP_LAYERS];
	C3D_Tex s_precipTex[PTEX_COUNT];
	bool s_precipTexOk[PTEX_COUNT];
	bool s_precipTexTried = false;
	u8* s_precipVerts = nullptr;       // the frame's quads, built in the first eye and drawn in both
	u16* s_precipIndices = nullptr;
	struct PrecipDraw { int first, n, tex; bool repeat; };
	PrecipDraw s_precipDraws[PRECIP_LAYERS + kRings + 1];
	int s_precipDrawCount = 0;
}
// What the last frame drew, for the test driver: rain streaks, flakes, the far ring's quads, and rain particle 0's
// place across the world (it stays put while the view turns: the particles hang in the world)
int g_precipStreaks = 0, g_precipFlakes = 0, g_precipRing = 0;
float g_precipX0 = 0.0f;
namespace
{

	// The rain's own dice: the game's rand() is the AI's and the scripts' too, and drawing rain from it changed who fought
	// whom while it rained (and made a run depend on what the screen drew)
	u32 s_precipSeed = 12345u;
	float frand()
	{
		s_precipSeed = s_precipSeed * 1664525u + 1013904223u;
		return (s_precipSeed >> 8) / 16777216.0f;
	}

	// One quad: centre c, half extents along a and b, colour 0xAABBGGRR. thin: the picture's middle column across it
	// (a streak a pixel or two wide sampled its sides, where the drop isn't, and mostly vanished)
	void precipQuad(u8* v, const float c[3], const float a[3], const float b[3], u32 rgba, bool thin)
	{
		static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		for (int i = 0; i < 4; i++)
		{
			float* p = (float*)(v + i * 24);
			for (int k = 0; k < 3; k++)
				p[k] = c[k] + a[k] * corner[i][0] + b[k] * corner[i][1];
			p[3] = thin ? 0.5f : corner[i][0] * 0.5f + 0.5f;
			p[4] = corner[i][1] * 0.5f + 0.5f;
			memcpy(v + i * 24 + 20, &rgba, 4);
		}
	}

	u32 precipColour(int r, int g, int b, float a)
	{
		int ai = (int)(fminf(1.0f, fmaxf(0.0f, a)) * 255.0f);
		return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)ai << 24);
	}
}

void Session::drawPrecipitation(const float eye[3], bool secondEye)
{
	if (!secondEye)
	{
		s_precipDrawCount = 0;
		g_precipStreaks = g_precipFlakes = g_precipRing = 0;
		float dt = precipDt;
		precipDt = 0.0f;
		if (w.current < 0 || w.cells[w.current].interior || w.game.weatherTypes.empty())
			return;
		Cell& here = w.here();
		if (here.hasWater() && eye[2] < here.waterZ)       // (OpenMW: none under water)
			return;
		// OpenMW: the falling stuff fades out of the old weather up to the new one's rain threshold, then in
		float thr = w.game.weatherTypes[std::min(w.weatherNext, (int)w.game.weatherTypes.size() - 1)].rainThreshold;
		thr = thr > 0.0f ? thr : 0.5f;
		auto amount = [&](int kind) {
			if (w.weatherNow == w.weatherNext)
				return w.weatherNow == kind ? 1.0f : 0.0f;
			float out = fmaxf(0.0f, 1.0f - w.weatherBlend / thr), in = fmaxf(0.0f, (w.weatherBlend - thr) / (1.0f - thr));
			return (w.weatherNow == kind ? out : 0.0f) + (w.weatherNext == kind ? in : 0.0f);
		};
		// a thunderstorm's Max Raindrops 650 against the rain's 450
		float rain = amount(WEATHER_RAIN) * (450.0f / 650.0f) + amount(WEATHER_THUNDER);
		float ash = amount(WEATHER_ASH), blight = amount(WEATHER_BLIGHT);
		if (rain < 0.02f && ash + blight < 0.02f)
			return;
		if (!s_precipTexTried)
		{
			s_precipTexTried = true;
			static const char* names[PTEX_COUNT] = { "rain", "ash_flake", "ash_cloud", "blight_cloud", "rain_sheet", "splash" };
			for (auto& sp : s_splash)
				sp.age = kSplashLife;
			for (int t = 0; t < PTEX_COUNT; t++)
			{
				// (the far rain's sheet and the splashes are never drawn without rainExtras: their 70 KB stay free)
				if (!g_profile.rainExtras && (t == PTEX_SHEET || t == PTEX_SPLASH))
				{
					s_precipTexOk[t] = false;
					continue;
				}
				char path[256];
				snprintf(path, sizeof(path), "%s/art/weather/%s.t3x", w.dataDir, names[t]);
				s_precipTexOk[t] = texImportFile(&s_precipTex[t], path);
				if (s_precipTexOk[t])
				{
					C3D_TexSetFilter(&s_precipTex[t], GPU_LINEAR, GPU_LINEAR);
					C3D_TexSetWrap(&s_precipTex[t], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
				}
			}
			s_precipVerts = (u8*)lockedLinearAlloc(kPrecipQuads * 4 * 24);
			s_precipIndices = (u16*)lockedLinearAlloc(kPrecipQuads * 6 * 2);
			if (s_precipIndices)
			{
				for (int q = 0; q < kPrecipQuads; q++)
				{
					static const u16 idx[6] = { 0, 1, 2, 0, 2, 3 };
					for (int k = 0; k < 6; k++)
						s_precipIndices[q * 6 + k] = (u16)(q * 4 + idx[k]);
				}
				GSPGPU_FlushDataCache(s_precipIndices, kPrecipQuads * 6 * 2);
			}
			logf("weather: rain %d, flake %d, ash %d, blight %d, rain sheet %d, splash %d textures", s_precipTexOk[0],
				s_precipTexOk[1], s_precipTexOk[2], s_precipTexOk[3], s_precipTexOk[4], s_precipTexOk[5]);
		}
		if (!s_precipVerts || !s_precipIndices)
			return;
		// Storms blow out from Red Mountain (OpenMW's calculateStormDirection)
		float sx = eye[0] - 20480.0f, sy = eye[1] - 69632.0f, sl = sqrtf(sx * sx + sy * sy);
		float wind[2] = { sl > 1.0f ? sx / sl : 0.0f, sl > 1.0f ? sy / sl : 1.0f };
		// Each layer's velocity: rain straight down (Rain Speed's tilt by a wind of 0.3 is under a degree); flakes
		// blown with the wind and falling slowly (Morrowind's fall at 900, pushed 270 sideways on average: at 30 frames
		// a second each jumped 30 units a frame, a flicker of dark rain rather than ash on the wind); the dust with
		// the wind at its sprays' 75 to 375, rising on its gravity
		float vel[PRECIP_LAYERS][3] = { { 0, 0, -575.0f }, { wind[0] * 360.0f, wind[1] * 360.0f, -220.0f },
			{ wind[0] * 225.0f, wind[1] * 225.0f, 40.0f } };
		int want[PRECIP_LAYERS] = { (int)(kPrecipMax[0] * fminf(1.0f, rain)), (int)(kPrecipMax[1] * fminf(1.0f, ash + blight)),
			(int)(kPrecipMax[2] * fminf(1.0f, ash + blight)) };
		float right[3], up[3], back[3];
		rendererViewAxes(right, up, back);
		u8* v = s_precipVerts;
		int quads = 0;
		// The far rain's ring, drawn first (it's behind the streaks): from 60 under the eye to 130 over it, faded out at
		// both ends (no edge against the sky or the ground below a hill)
		s_precipClock = fmodf(s_precipClock + dt, 600.0f);
		if (rain >= 0.02f && g_profile.rainExtras && s_precipTexOk[PTEX_SHEET])
			for (int r = 0; r < kRings; r++)
			{
				float rad = kRingRadius[r], across = 2.0f * 3.14159265f * rad / kRingRepeats[r], tall = across * 2.0f;
				float slide = s_precipClock * 575.0f / tall + r * 0.37f;
				static const float rowZ[3] = { -60.0f, 30.0f, 130.0f }, rowA[3] = { 0.0f, 1.0f, 0.0f };
				float strength = fminf(1.0f, rain) * 0.7f;
				s_precipDraws[s_precipDrawCount++] = { quads, kRingSides * 2, PTEX_SHEET, true };
				g_precipRing += kRingSides * 2;
				for (int k = 0; k < kRingSides; k++)
					for (int h = 0; h < 2; h++)
					{
						float* f = (float*)(s_precipVerts + quads * 96);
						for (int i = 0; i < 4; i++)
						{
							int side = k + (i == 1 || i == 2), row = h + (i >= 2);
							float ang = side * 2.0f * 3.14159265f / kRingSides;
							float* p = f + i * 6;
							p[0] = eye[0] + sinf(ang) * rad;
							p[1] = eye[1] + cosf(ang) * rad;
							p[2] = eye[2] + rowZ[row];
							p[3] = (float)side * kRingRepeats[r] / kRingSides;
							p[4] = (rowZ[row] - rowZ[0]) / tall + slide;
							u32 col = precipColour(210, 220, 235, rowA[row] * strength);
							memcpy(p + 5, &col, 4);
						}
						quads++;
					}
			}
		for (int L = 0; L < PRECIP_LAYERS; L++)
		{
			PrecipLayer& P = s_precip[L];
			const float* box = kPrecipBox[L];
			float c[3] = { eye[0], eye[1], eye[2] + kPrecipLift[L] };
			if (!P.seeded)
			{
				for (int i = 0; i < kPrecipMax[L]; i++)
				{
					for (int k = 0; k < 3; k++)
						P.pos[i][k] = c[k] + (frand() - 0.5f) * box[k];
					P.pace[i] = 0.75f + frand() * 0.5f;
				}
				P.seeded = true;
			}
			int n = std::min(want[L], kPrecipQuads - quads);
			if (n <= 0)
				continue;
			// (only those drawn move: the rest wait where they were seeded, as scattered as these)
			for (int i = 0; i < n; i++)
				for (int k = 0; k < 3; k++)
				{
					float q = P.pos[i][k] + vel[L][k] * P.pace[i] * dt - c[k], h = box[k] * 0.5f;
					if (q > h || q < -h)
						q -= box[k] * floorf(q / box[k] + 0.5f);
					P.pos[i][k] = c[k] + q;
				}
			int tex = L == PRECIP_RAIN ? PTEX_RAIN : L == PRECIP_FLAKES ? PTEX_FLAKE : blight > ash ? PTEX_BLIGHT : PTEX_ASH;
			s_precipDraws[s_precipDrawCount++] = { quads, n, tex, false };
			if (L == PRECIP_RAIN)
			{
				g_precipStreaks = n;
				g_precipX0 = P.pos[0][0];
			}
			else if (L == PRECIP_FLAKES)
				g_precipFlakes = n;
			float vl = sqrtf(vel[L][0] * vel[L][0] + vel[L][1] * vel[L][1] + vel[L][2] * vel[L][2]);
			float axis[3] = { vel[L][0] / vl, vel[L][1] / vl, vel[L][2] / vl };
			for (int i = 0; i < n; i++)
			{
				const float* p = P.pos[i];
				float e[3] = { p[0] - eye[0], p[1] - eye[1], p[2] - eye[2] };
				float d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
				// near the box's sides they fade (the wrap's jump unseen)
				float edge = fmaxf(fabsf(p[0] - c[0]) / box[0], fabsf(p[1] - c[1]) / box[1]) * 2.0f;
				float fade = fminf(1.0f, (1.0f - edge) * 4.0f);
				if (L == PRECIP_DUST)
				{
					// clouds turned to the eye, 400 across (the sprays' size), gone within 150 of it
					float a = fade * fminf(1.0f, fmaxf(0.0f, (d - 150.0f) / 250.0f));
					float half = 200.0f, ar[3], au[3];
					for (int k = 0; k < 3; k++)
					{
						ar[k] = right[k] * half;
						au[k] = up[k] * half;
					}
					precipQuad(v + quads * 96, p, ar, au, precipColour(255, 255, 255, a), false);
				}
				else
				{
					// a streak along its fall, turned round that line to the eye; at least about a pixel wide (the
					// top screen shows 208 / d pixels a unit at distance d)
					float s[3] = { axis[1] * e[2] - axis[2] * e[1], axis[2] * e[0] - axis[0] * e[2], axis[0] * e[1] - axis[1] * e[0] };
					float sl2 = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
					if (sl2 < 1e-3f)
					{
						memcpy(s, right, sizeof(s));
						sl2 = 1.0f;
					}
					// rain: about a pixel wide however near (a world width made the nearest drops bars five pixels
					// wide), faster drops longer (the eye's blur of a falling drop), each its own brightness, and
					// faded out right at the eye
					// flakes: the whole picture (a soft speck), a little drawn out along their flight, no more than
					// about 3 pixels across (ashcloud.nif's 4.5 units filled 40 near the eye, a dark block hanging
					// there), faded near the eye like the rain
					float width = L == PRECIP_RAIN ? fmaxf(0.05f, d * 0.0028f) : fminf(2.25f, fmaxf(0.4f, d * 0.0075f));
					float len = L == PRECIP_RAIN ? 15.0f * P.pace[i] : width * 1.6f;
					if (L == PRECIP_RAIN)
						fade *= fminf(1.0f, (d - 8.0f) / 50.0f) * (0.55f + 0.45f * ((i * 37) % 11) / 10.0f);
					else
						fade *= fminf(1.0f, fmaxf(0.0f, (d - 20.0f) / 80.0f));
					float sa[3], la[3];
					for (int k = 0; k < 3; k++)
					{
						sa[k] = s[k] / sl2 * width;
						la[k] = axis[k] * len;
					}
					u32 col = L == PRECIP_RAIN ? precipColour(210, 220, 235, fade) : precipColour(255, 255, 255, fade);
					precipQuad(v + quads * 96, p, sa, la, col, L == PRECIP_RAIN);
				}
				quads++;
			}
		}
		// Ripples where drops land: new ones in view, 30 to 180 out, on the floor found there (the water's
		// surface over it); 50 a second in rain, 70 in a thunderstorm
		if (rain >= 0.02f && g_profile.rainExtras && s_precipTexOk[PTEX_SPLASH])
		{
			float fwd = atan2f(back[0], back[1]);     // (rendererViewAxes: "back" is the view's z row, along the look here)
			for (s_splashDue += fminf(dt, 0.1f) * 70.0f * fminf(1.0f, rain); s_splashDue >= 1.0f; s_splashDue -= 1.0f)
			{
				Splash* sp = nullptr;
				for (auto& c : s_splash)
					if (c.age >= kSplashLife)
					{
						sp = &c;
						break;
					}
				if (!sp)
					break;
				float ang = fwd + (frand() - 0.5f) * 1.8f, dist = 30.0f + frand() * 150.0f;
				float x = eye[0] + sinf(ang) * dist, y = eye[1] + cosf(ang) * dist, floorZ = -1e9f;
				for (LoadedCell* l : w.loaded)
				{
					float z;
					if (collisionReaches(l->cell.collision, x, y, 0.0f)
						&& collisionFloor(l->cell.collision, x, y, eye[2] + 200.0f, eye[2] - 600.0f, &z) && z > floorZ)
						floorZ = z;
				}
				if (here.hasWater() && here.waterZ > floorZ)
					floorZ = here.waterZ;
				if (floorZ < -1e8f)
					continue;
				*sp = { { x, y, floorZ + 1.0f }, 0.0f };
			}
			int first = quads, n = 0;
			for (auto& sp : s_splash)
			{
				if (sp.age >= kSplashLife || quads >= kPrecipQuads)
					continue;
				sp.age += fminf(dt, 0.1f);       // (a long frame skips nothing: native draws one frame in many)
				float f = fminf(1.0f, sp.age / kSplashLife), half = 3.0f + 11.0f * f;
				float ax[3] = { half, 0, 0 }, ay[3] = { 0, half, 0 };
				precipQuad(v + quads * 96, sp.pos, ax, ay, precipColour(230, 236, 245, 1.0f - f * f), false);
				quads++;
				n++;
			}
			if (n)
				s_precipDraws[s_precipDrawCount++] = { first, n, PTEX_SPLASH, false };
		}
		GSPGPU_FlushDataCache(s_precipVerts, quads * 96);
	}
	for (int k = 0; k < s_precipDrawCount; k++)
	{
		const PrecipDraw& d = s_precipDraws[k];
		ActorMesh m = {};
		m.flags = BATCH_BLEND | BATCH_TWO_SIDED | (d.repeat ? 0 : BATCH_CLAMP);
		m.numVerts = d.n * 4;
		m.numIndices = d.n * 6;
		m.verts = s_precipVerts + d.first * 96;
		m.indices = s_precipIndices;
		rendererDrawMesh(m, nullptr, s_precipTexOk[d.tex] ? &s_precipTex[d.tex] : nullptr);
	}
}

std::vector<SkyBillboard> g_skyBillboards;
float g_starAlpha = 0.0f;
C3D_Tex* g_starTexture = nullptr;
C3D_Tex* g_cloudTex[2] = { nullptr, nullptr };
float g_cloudBlend = 0.0f;

// The stars (a dome, tiled) and the sun / moons (squares toward the eye) around the camera, no depth: before the
// clouds, as OpenMW's sky draws them; afterClouds: the pictures drawn over the clouds (the sun's glare)
void drawSkyBodies(const C3D_Mtx& view, bool secondEye, bool afterClouds)
{
	static const int kMax = 16;
	static u8* ring = nullptr;                   // this frame's billboard vertices (each eye its own)
	static u16* quad = nullptr;
	static u8* dome = nullptr;
	static u16* domeIdx = nullptr;
	static int domeVerts = 0, domeIndices = 0, slot = 0;
	if (!ring)
	{
		ring = (u8*)lockedLinearAlloc(kMax * 2 * 4 * 24);
		quad = (u16*)lockedLinearAlloc(6 * 2);
		static const u16 q[6] = { 0, 1, 2, 0, 2, 3 };
		if (quad)
		{
			memcpy(quad, q, sizeof(q));
			GSPGPU_FlushDataCache(quad, sizeof(q));
		}
		// star dome: elevation rings 5..90 degrees, 12 around; the texture repeats 4 times around
		const int seg = 12, rings = 5;
		const float elev[rings] = { 0, 15, 35, 60, 90 };
		domeVerts = (seg + 1) * rings;
		dome = (u8*)lockedLinearAlloc(domeVerts * 24);
		std::vector<u16> idx;
		for (int r = 0; r < rings && dome; r++)
			for (int i = 0; i <= seg; i++)
			{
				float e = elev[r] * 0.0174533f, a = 6.2831853f * i / seg;
				float* p = (float*)(dome + (r * (seg + 1) + i) * 24);
				p[0] = cosf(e) * sinf(a) * 2800.0f;
				p[1] = cosf(e) * cosf(a) * 2800.0f;
				p[2] = sinf(e) * 2800.0f;
				p[3] = i * 4.0f / seg;
				p[4] = r * 1.5f / (rings - 1);
				u32 white = 0xFFFFFFFF;
				memcpy(dome + (r * (seg + 1) + i) * 24 + 20, &white, 4);
				if (r < rings - 1 && i < seg)
				{
					u16 v = r * (seg + 1) + i, u = v + seg + 1;
					u16 t[6] = { v, (u16)(v + 1), (u16)(u + 1), v, (u16)(u + 1), u };
					idx.insert(idx.end(), t, t + 6);
				}
			}
		domeIndices = idx.size();
		domeIdx = (u16*)lockedLinearAlloc(idx.size() * 2);
		if (domeIdx)
		{
			memcpy(domeIdx, idx.data(), idx.size() * 2);
			GSPGPU_FlushDataCache(domeIdx, idx.size() * 2);
		}
		if (dome)
			GSPGPU_FlushDataCache(dome, domeVerts * 24);
	}
	if (!ring || !quad)
		return;
	C3D_Mtx skyView = view;
	skyView.r[0].w = skyView.r[1].w = skyView.r[2].w = 0.0f;
	setModelView(skyView);
	static const float one[3] = { 1, 1, 1 };
	setTint(one);
	// the stars, fading in at night
	if (!afterClouds && g_starTexture && g_starAlpha > 0.01f && dome && domeIdx)
	{
		u8 a = (u8)(fminf(1.0f, g_starAlpha) * 255.0f);
		for (int i = 0; i < domeVerts; i++)
			dome[i * 24 + 23] = a;
		GSPGPU_FlushDataCache(dome, domeVerts * 24);
		CellBatch b = {};
		b.tex = 0;
		b.flags = BATCH_BLEND | BATCH_ADDITIVE | BATCH_TWO_SIDED;
		b.numVerts = domeVerts;
		b.numIndices = domeIndices;
		b.verts = dome;
		b.indices = domeIdx;
		b.stride = CELL_VERTEX_SIZE;
		drawBatchTex(g_starTexture, b, DRAW_SKY, 0.0f);
	}
	if (!secondEye && !afterClouds)
		slot = 0;
	float right[3] = { skyView.r[0].x, skyView.r[0].y, skyView.r[0].z };
	float up[3] = { skyView.r[1].x, skyView.r[1].y, skyView.r[1].z };
	for (auto& sb : g_skyBillboards)
	{
		if (sb.afterClouds != afterClouds)
			continue;
		if (slot >= kMax * 2 || !sb.tex)
			break;
		u8* v = ring + (slot++) * 4 * 24;
		const float* ax = sb.fixed ? sb.axisU : right;
		const float* ay = sb.fixed ? sb.axisV : up;
		static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
		static const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		float cr = sb.fixed ? 1.0f : cosf(sb.roll), sr = sb.fixed ? 0.0f : sinf(sb.roll);
		for (int i = 0; i < 4; i++)
		{
			float cx = corner[i][0] * cr - corner[i][1] * sr, cy = corner[i][0] * sr + corner[i][1] * cr;
			float* p = (float*)(v + i * 24);
			for (int k = 0; k < 3; k++)
				p[k] = sb.dir[k] * 2700.0f + (ax[k] * cx + ay[k] * cy) * sb.size;
			p[3] = uv[i][0];
			p[4] = uv[i][1];
			memcpy(v + i * 24 + 20, &sb.rgba, 4);
		}
		GSPGPU_FlushDataCache(v, 4 * 24);
		CellBatch b = {};
		b.tex = 0;
		b.flags = BATCH_BLEND | BATCH_TWO_SIDED | BATCH_CLAMP | (sb.additive ? BATCH_ADDITIVE : 0);
		b.numVerts = 4;
		b.numIndices = 6;
		b.verts = v;
		b.indices = quad;
		b.stride = CELL_VERTEX_SIZE;
		drawBatchTex(sb.tex, b, DRAW_SKY, 0.0f);
	}
}
