// The calendar and the game's clock
#include "mwworld/worldimp.hpp"
#include "mwrender/objectpaging.hpp"
#include "components/files/datapath.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extern/cjson/cJSON.h"
#include "platform/linear.hpp"
#include "platform/console.hpp"
extern "C" u32 __ctru_heap_size;
#include "platform/memstat.hpp"
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "mwworld/worldhelpers.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

// The calendar is the Day / Month / Year / DaysPassed globals (scripts set them; time moving on advances them)
GameDate World::date() const
{
	auto value = [&](const char* id, int fallback) {
		auto it = globals.find(id);
		if (it != globals.end())
			return (int)it->second;
		auto g = game.globals.find(id);
		return g != game.globals.end() ? (int)g->second.value : fallback;
	};
	GameDate d;
	d.day = value("day", 16);
	d.month = value("month", 7);
	d.year = value("year", 427);
	d.daysPassed = value("dayspassed", 1);
	d.hour = gameHour - floorf(gameHour / 24.0f) * 24.0f;
	if (d.month < 0 || d.month > 11)
		d.month = 0;
	return d;
}

float World::timescale() const
{
	auto it = globals.find("timescale");
	return it != globals.end() ? it->second : 30.0f;
}

// Whole days gone by: the day moves on through the months, years carry, days passed counts them
static void advanceCalendar(std::unordered_map<std::string, float>& g, int days)
{
	for (int k = 0; k < days; k++)
	{
		int month = std::max(0, std::min(11, (int)g["month"]));
		if (++g["day"] > kMonthDays[month])
		{
			g["day"] = 1.0f;
			if (month < 11)
				g["month"] = (float)(month + 1);
			else
			{
				g["month"] = 0.0f;
				g["year"] += 1.0f;
			}
		}
		g["dayspassed"] += 1.0f;
	}
}

void World::syncTime()
{
	float& gh = globals["gamehour"];
	// (a script that set GameHour another way)
	if (hourWritten >= 0.0f && fabsf(gh - hourWritten) > 0.001f)
		setGlobal("gamehour", gh);
	int today = (int)floorf(gameHour / 24.0f);
	if (dayCounted >= 0 && today > dayCounted)
		advanceCalendar(globals, today - dayCounted);
	dayCounted = today;
	globals["gamehour"] = hourWritten = gameHour - floorf(gameHour / 24.0f) * 24.0f;
}

// Morrowind's rest, wait and level-up rules (as OpenMW has them), skill increases and the calendar

static const char* kMonthKeys[12] = { "smonthmorningstar", "smonthsunsdawn", "smonthfirstseed", "smonthrainshand",
	"smonthsecondseed", "smonthmidyear", "smonthsunsheight", "smonthlastseed", "smonthheartfire", "smonthfrostfall",
	"smonthsunsdusk", "smontheveningstar" };
static const char* kMonthNames[12] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed",
	"Midyear", "Sun's Height", "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };

// "16 Last Seed, 9 a.m."
std::string Session::dateText(bool withHour)
{
	GameDate d = w.date();
	std::string t = std::to_string(d.day) + " " + w.game.gmst(kMonthKeys[d.month], kMonthNames[d.month]);
	if (withHour)
	{
		int h = (int)d.hour;
		t += ", " + std::to_string(h % 12 == 0 ? 12 : h % 12) + " "
			+ w.game.gmst(h < 12 ? "ssavemenuhelp04" : "ssavemenuhelp05", h < 12 ? "a.m." : "p.m.");
	}
	return t;
}
