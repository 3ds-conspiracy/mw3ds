#pragma once

// What the world files share

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>

static const char* kItemTypes[] = { "MISC", "BOOK", "ALCH", "INGR", "WEAP", "ARMO", "CLOT", "LIGH", "APPA", "LOCK", "PROB", "REPA" };

// Reads cells/<file>.json and appends its references
// A cell's references and path grid as read from cells/<file>.json. Reading only looks at the game
// data, so the streaming thread can do it; appendRefs (main thread) makes them part of the world.
struct ParsedRefs
{
	// (a deque, as World::refs: a Ref is 832 bytes, and a vector of a town's references doubling as it grew wanted one
	// free block of 400 KB on the streaming thread, more than an Old 3DS's cut-up heap had late in a long session)
	std::deque<Ref> refs;
	std::vector<PathPoint> points;
	std::vector<std::pair<int, int>> edges;
};

static const int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void initActorState(const GameData& g, Ref& ref);
bool parseRefs(const World& w, int cellIndex, ParsedRefs& out);
int takeSlots(std::vector<std::pair<int, int>>& free, int n);
void freeSlots(std::vector<std::pair<int, int>>& free, int first, int n);
void appendRefs(World& w, int cellIndex, ParsedRefs& p);
void hideRanges(World& w, Ref& ref);
void hideCollision(World& w, Ref& ref);
void showRanges(World& w, Ref& ref);

static inline bool isItemType(const std::string& t)
{
	for (auto* it : kItemTypes)
		if (t == it)
			return true;
	return false;
}

static inline std::string cellFileName(const std::string& cellName)
{
	std::string out;
	for (char c : lower(cellName))
		out += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') ? c : '_';
	return out;
}

// Reference slots given back; a free run at the end of the list goes from it (its memory back to the heap)
static inline void freeRefSlots(World& w, int first, int n)
{
	w.refsChanged();
	freeSlots(w.freeRefs, first, n);
	if (!w.freeRefs.empty() && w.freeRefs.back().first + w.freeRefs.back().second == (int)w.refs.size())
	{
		w.refs.resize(w.freeRefs.back().first);
		w.freeRefs.pop_back();
	}
}
