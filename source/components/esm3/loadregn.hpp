#pragma once

// Regions: weather chances, sleep creatures, ambient sounds (OpenMW's ESM::Region)
#include <string>
#include <vector>

enum { WEATHER_CLEAR, WEATHER_CLOUDY, WEATHER_FOGGY, WEATHER_OVERCAST, WEATHER_RAIN, WEATHER_THUNDER, WEATHER_ASH,
	WEATHER_BLIGHT, WEATHER_SNOW, WEATHER_BLIZZARD };
struct RegionDef { std::string name; int chances[10] = {}; std::string sleep;   // sleep: its sleep creatures' leveled list
	std::vector<std::pair<std::string, int>> ambient; };                        // sounds heard outdoors, chance %
