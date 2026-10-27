// Script functions: weather
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

std::optional<float> skyExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	// ModRegion region clear cloudy foggy overcast rain thunder ash blight [snow blizzard]: its chances
	case F_MODREGION:
	{
		// a weather left out has no chance; each is 0..100
		std::vector<int> ch(10, 0);
		for (size_t k = 1; k < n.kids.size() && k <= 10; k++)
			ch[k - 1] = std::max(0, std::min(100, (int)argNum(c, n, k)));
		w.modRegion(lower(argStr(c, n, 0)), ch);
		return 0.0f;
	}
	// ChangeWeather region weather: that region's weather now, until the next weather change
	case F_CHANGEWEATHER:
		w.changeWeather(lower(argStr(c, n, 0)), (int)argNum(c, n, 1));
		return 0.0f;
	// the weather in effect: the new one only once the change to it is done
	case F_GETCURRENTWEATHER:
		return (float)w.weatherNow;
	// Bloodmoon's red moon: Secunda in [Moons] Script Color until TurnMoonWhite (OpenMW's setMoonColour). Kept in a
	// save (OpenMW forgets it on a load, and bloodmoonScript turns it red only once)
	case F_TURNMOONRED: case F_TURNMOONWHITE:
		w.moonRed = n.func == F_TURNMOONRED;
		return 0.0f;
	default:
		return std::nullopt;
	}
}
