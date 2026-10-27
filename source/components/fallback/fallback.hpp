#pragma once

// Morrowind.ini's sky, moons and weathers (OpenMW's Fallback::Map values)
#include <string>

// Third-person pieces (tools/convert/firstperson.py third_person): [female] race -> slot -> piece, the heads and
// hairs to choose from (body id, piece), and item -> slot -> (male, female piece)
// The sun, the moons and the stars (level.py "sky_bodies"; Morrowind.ini [Moons])
struct MoonDef { float size = 50, fadeInStart = 14, fadeInFinish = 15, fadeOutStart = 7, fadeOutFinish = 10,
	axisOffset = 35, speed = 0.5f, dailyIncrement = 1, fadeStartAngle = 50, fadeEndAngle = 40, earlyFadeAngle = 0.5f; };
struct SkyBodiesDef
{
	std::string sun, glare, stars, masser[8], secunda[8];
	std::string circle[2];            // Masser's and Secunda's disk (tx_mooncircle_full_m / _s), in the sky's colour
	MoonDef moons[2];
	float sunrise = 6, sunset = 18;
	unsigned char scriptColor[3] = { 255, 20, 20 };   // [Moons] Script Color: Secunda after TurnMoonRed (OpenMW's fallback when absent)
};

// A weather (Morrowind.ini [Weather X]): colours [sky, fog, ambient, sun][sunrise, day, sunset, night],
// fog depth by day / night (thicker = shorter view), its ambient loop sound
struct WeatherType { std::string name; float colors[4][4][3] = {}; float fog[2] = { 0.69f, 0.69f }; std::string sound;
	// the rest of what OpenMW reads for it (weather.cpp): how fast the change to it goes (per real second), wind,
	// sun glare when looked at, rain threshold, thunder, clouds maximum, rain / snow falling
	float delta = 0.015f, wind = 0.0f, glare = 1.0f, rainThreshold = 0.5f, thunderFreq = 0.0f, thunderThreshold = 0.0f,
		flashDecrement = 0.0f, cloudsMax = 1.0f;
	bool precip = false;
	std::string clouds; };                              // its cloud picture (Cloud Texture), as the sky draws it
// The day's boundaries (Morrowind.ini [Weather]) and, per colour channel [sky, fog, ambient, sun], the hours it
// leads / lags them: before sunrise, after sunrise, before sunset, after sunset
struct WeatherClock { float sunrise = 6.0f, sunset = 18.0f, sunriseDur = 2.0f, sunsetDur = 2.0f;
	float win[4][4] = { { 0.5f, 1.0f, 1.5f, 0.5f }, { 0.5f, 1.0f, 2.0f, 1.0f }, { 0.5f, 2.0f, 1.0f, 1.25f }, { 0.0f, 0.0f, 1.0f, 1.25f } }; };
