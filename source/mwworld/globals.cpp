// Global script variables
#include "mwworld/worldimp.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include "mwworld/worldhelpers.hpp"

// OpenMW's DateTimeManager setters (datetimemanager.cpp): a day past the month's end wraps into the next months,
// a month past 11 carries years and cuts the day to its length, an hour past 24 moves the day but not DaysPassed
void World::setGlobal(const std::string& name, float value)
{
	if (name == "gamehour")
	{
		float h = fmaxf(0.0f, value);
		int days = (int)(h / 24.0f);
		float hour = fminf(fmodf(h, 24.0f), 23.99999809f);
		gameHour = floorf(gameHour / 24.0f) * 24.0f + hour;
		weatherSeen = gameHour;          // (setting the hour is not time passing: no weather change)
		globals[name] = hourWritten = hour;
		if (days > 0)
			setGlobal("day", globals["day"] + (float)days);
		return;
	}
	if (name == "day")
	{
		int day = std::max(1, (int)value), month = std::max(0, std::min(11, (int)globals["month"]));
		int year = (int)globals["year"];
		for (;;)
		{
			if (day <= kMonthDays[month])
				break;
			day -= kMonthDays[month];
			if (month < 11)
				month++;
			else
			{
				month = 0;
				year++;
			}
		}
		globals["day"] = (float)day;
		globals["month"] = (float)month;
		globals["year"] = (float)year;
		return;
	}
	if (name == "month")
	{
		int m = std::max(0, (int)value);
		int month = m % 12;
		globals["year"] = globals["year"] + (float)(m / 12);
		globals["month"] = (float)month;
		if (globals["day"] > kMonthDays[month])
			globals["day"] = (float)kMonthDays[month];
		return;
	}
	if (name == "year" || name == "dayspassed")
	{
		globals[name] = truncf(value);
		return;
	}
	globals[name] = value;
}
