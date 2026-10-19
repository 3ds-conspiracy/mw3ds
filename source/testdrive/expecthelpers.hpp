#pragma once

// What the EXPECT files share
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>

extern const char* g_playerBlock;
#include <malloc.h>

std::optional<bool> numberOfStats(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfVisuals(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfRules(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfInput(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfRefs(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfItems(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfWorld(Session& s, const std::string& what, const std::string& arg, float& v);
std::optional<bool> numberOfState(Session& s, const std::string& what, const std::string& arg, float& v);

// How bright step shot n's top screen is, 0..255 (its pixels' mean); -1: no such shot. With spread: how far its pixels'
// brightness (the mean of a pixel's three channels) strays from their mean instead (standard deviation: clouds against
// the sky, a pattern against a flat colour)
static inline float shotLuma(int n, bool spread = false)
{
	char path[64];
	snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_step_%02d.bmp", n);
	float v = -1.0f;
	if (FILE* f = fopen(path, "rb"))
	{
		// 400 x 480, 24-bit, rows bottom first: the top screen is the last 240 rows (held only while read)
		const size_t half = 400 * 240 * 3;
		u8* top = (u8*)malloc(half);
		if (top && fseek(f, 54 + (long)half, SEEK_SET) == 0 && fread(top, half, 1, f) == 1)
		{
			double sum = 0.0, sq = 0.0;
			for (size_t i = 0; i < half; i += 3)
			{
				double l = (top[i] + top[i + 1] + top[i + 2]) / 3.0;
				sum += l;
				sq += l * l;
			}
			double px = (double)(half / 3), mean = sum / px;
			v = spread ? (float)sqrt(fmax(0.0, sq / px - mean * mean)) : (float)mean;
		}
		free(top);
		fclose(f);
	}
	return v;
}
