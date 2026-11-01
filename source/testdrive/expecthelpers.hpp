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

// How bright step shot n's top screen is, 0..255 (its pixels' mean); -1: no such shot. Kind 1 (spread): how far its
// pixels' brightness (the mean of a pixel's three channels) strays from their mean instead (standard deviation: clouds
// against the sky, a pattern against a flat colour); kind 2 (dark): the percentage of its pixels under 0.6 x the mean
// (a dark shape on a bright sky); kind 3 (black): the percentage of them all but black, brightness under 16 (a model
// drawn without its picture's colours)
static inline float shotLuma(int n, int kind = 0)
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
			v = kind == 1 ? (float)sqrt(fmax(0.0, sq / px - mean * mean)) : (float)mean;
			if (kind == 2 || kind == 3)
			{
				int dark = 0;
				for (size_t i = 0; i < half; i += 3)
					dark += (top[i] + top[i + 1] + top[i + 2]) / 3.0 < (kind == 2 ? 0.6 * mean : 16.0);
				v = (float)(100.0 * dark / px);
			}
		}
		free(top);
		fclose(f);
	}
	return v;
}

// How bright a box (x, y, w x h, bottom-screen pixels 0..319 x 0..239) of step shot n's bottom screen is, 0..255 (its
// pixels' mean); -1: no such shot or the box off the screen
static inline float shotLumaBox(int n, int x, int y, int bw, int bh)
{
	if (x < 0 || y < 0 || bw <= 0 || bh <= 0 || x + bw > 320 || y + bh > 240)
		return -1.0f;
	char path[64];
	snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/shot_step_%02d.bmp", n);
	float v = -1.0f;
	if (FILE* f = fopen(path, "rb"))
	{
		// 400 x 480, 24-bit, rows bottom first: the bottom screen is the first 240 rows, 40 pixels in from each side
		const size_t row = 400 * 3;
		u8* line = (u8*)malloc(row);
		double sum = 0.0;
		bool ok = line != nullptr;
		for (int yy = y; ok && yy < y + bh; yy++)
		{
			ok = fseek(f, 54 + (long)((239 - yy) * row), SEEK_SET) == 0 && fread(line, row, 1, f) == 1;
			for (int xx = x; ok && xx < x + bw; xx++)
			{
				const u8* p = line + (40 + xx) * 3;
				sum += (p[0] + p[1] + p[2]) / 3.0;
			}
		}
		if (ok)
			v = (float)(sum / (bw * bh));
		free(line);
		fclose(f);
	}
	return v;
}
