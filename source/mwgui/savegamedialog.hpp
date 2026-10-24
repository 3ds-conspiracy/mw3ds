#pragma once

#include <3ds.h>
#include <string>
#include <vector>

#include "mwgui/widgets.hpp"

// Save files on the SD card and saves shipped with the game data.
//   sdmc:/3ds/mw3ds/save.json         autosave (cell changes, end of scripted runs)
//   sdmc:/3ds/mw3ds/saves/save_N.json saves made from the Saves screen
//   <dataDir>/saves/*.json            bundled starting points (e.g. Balmora), read-only
// Each save has a one-line "<file>.meta" beside it: name \t place \t level \t time (ms since 1900, the console's
// clock) \t save count (newest highest), so the list doesn't have to parse whole saves.

static const char* const kAutosavePath = "sdmc:/3ds/mw3ds/save.json";
static const char* const kSaveDir = "sdmc:/3ds/mw3ds/saves";

struct SaveInfo
{
	std::string path;
	std::string title;       // "Tester - Balmora"
	std::string detail;      // "Level 3, 24 Sep 2026 14:02" / "Autosave" / "Starting point"
	std::string name, place; // from the .meta (empty for a save without one)
	int level = 0;
	u64 time = 0;
	u32 order = 0;           // the .meta's save count: higher was saved later (0: older saves without one)
	bool autosave = false, bundled = false;
};

void writeSaveMeta(const char* savePath, const std::string& name, const std::string& place, int level, u64 timeMs);
// The test driver's CLOCKSHIFT: added to the time saves record (a console whose clock was reset or set back)
extern s64 g_saveClockShiftMs;
// The last of the player's own saves written (not the autosave), and its list row as savesList would read it back
extern std::string g_lastSavePath;
extern SaveInfo g_lastSaveInfo;
// The new row for a save just written, put where savesList would put it: after a save the list is updated from
// memory, not read again (reading the card's directory again after a write took seconds on the console)
void addSaveToList(std::vector<SaveInfo>& list);
// Autosave first, then the player's saves newest first, then bundled ones. Read from the card once, then kept up to date
// as the game saves and deletes (g_saveListReads: the times it read the card)
std::vector<SaveInfo> savesList(const char* dataDir);
extern int g_saveListReads;
// .meta files opened listing the saves (those the saves' index lacked), and the kept list dropped so the next listing
// reads the card as at a start (the test driver's SAVESRELOAD)
extern int g_saveMetaReads;
void forgetSavesList();
// A new, unused path in kSaveDir
std::string saveNewPath();

// A row of the list: the place (with the character's name only when the saves are of several), and its level, date and
// time for the column on the right
void saveRow(const std::vector<SaveInfo>& saves, size_t i, std::string& left, std::string& right);
// The list as UI rows; returns the row activated this frame (tap or A) or -1
int uiSaveList(UiList& list, const std::vector<SaveInfo>& saves, float x, float y, float w, float h, bool keys = true);
