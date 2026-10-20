#pragma once

// Morrowind's calendar

// Morrowind's calendar (months of 28..31 days): what the Day / Month / Year / DaysPassed globals hold
struct GameDate
{
	int day, month, year;             // month 0 = Morning Star
	int daysPassed;
	float hour;                       // 0..24
};
