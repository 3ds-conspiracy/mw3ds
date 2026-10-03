#pragma once

#include <3ds.h>
#include <string>
#include <vector>

#include "ui.h"

// Save files on the SD card and saves shipped with the game data.
//   sdmc:/3ds/mw3ds/save.json         autosave (cell changes, end of scripted runs)
//   sdmc:/3ds/mw3ds/saves/save_N.json saves made from the Saves screen
//   <dataDir>/saves/*.json            bundled starting points (e.g. Balmora), read-only
// Each save has a one-line "<file>.meta" beside it: name \t place \t level \t time (ms since 1900),
// so the list doesn't have to parse whole saves.

static const char* const kAutosavePath = "sdmc:/3ds/mw3ds/save.json";
static const char* const kSaveDir = "sdmc:/3ds/mw3ds/saves";

struct SaveInfo
{
	std::string path;
	std::string title;       // "Tester - Balmora"
	std::string detail;      // "Level 3, 24 Sep 2026 14:02" / "Autosave" / "Starting point"
	u64 time = 0;
	bool autosave = false, bundled = false;
};

void writeSaveMeta(const char* savePath, const std::string& name, const std::string& place, int level, u64 timeMs);
// Autosave first, then the player's saves newest first, then bundled ones
std::vector<SaveInfo> savesList(const char* dataDir);
// A new, unused path in kSaveDir
std::string saveNewPath();

// The list as UI rows; returns the row activated this frame (tap or A) or -1
int uiSaveList(UiList& list, const std::vector<SaveInfo>& saves, float x, float y, float w, float h, bool keys = true);
