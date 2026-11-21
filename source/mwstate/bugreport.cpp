// The player's bug report: one entry appended to sdmc:/3ds/mw3ds/reports.json, with a screenshot
// (report_NN.bmp) and a copy of the game (report_NN_save.json, the save format) beside it. The engine knows
// where it is and what it has complained about (the log's "monitor:" lines), so an entry writes down what a
// player can only guess at: the exact position, the cell file and its grid square, and the place's name as
// the game shows it. The position is where the player stood, which is usually a step from the wall they ran
// into, so the save copy matters: it puts them back there with the same cells, items and quest stages.
//
// reports.json is an object with a "reports" array, printed in full every time (a report is a few KB, and a
// whole file is easier to read on a PC than a stream of blocks), written to a .tmp and renamed over the old
// one: the saves' trick, so an interrupted write never loses the reports before it. A file that will not
// parse is kept as reports.json.bad and a new one started: one corrupt report should not swallow the rest.
// Reports go to the folder the log and the crash notes already use, and use the same names as the save screen.
#include <3ds.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "bugreport.hpp"
#include "extern/cjson/cJSON.h"
#include "mwbase/session.hpp"
#include "mwrender/screenshotmanager.hpp"
#include "mwworld/worldimp.hpp"
#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "platform/console.hpp"

#ifndef MW3DS_VERSION
#define MW3DS_VERSION "unknown"     // the Makefile passes the git tag; a build without it says so
#endif

static const char* const kReportsPath = "sdmc:/3ds/mw3ds/reports.json";
static const int kMaxWarnings = 80;      // the warnings kept per report: a missing texture repeated in every
                                         // cell of a region is one line each, and the card has to hold them
static const int kMaxTail = 12;          // log lines kept to show what happened just before the report

// report_NN.bmp / report_NN_save.json: NN goes up with every report and is never used twice, so an old
// report's screenshot is never overwritten by mistake. The number comes from the last entry in reports.json
// (so deleting a report's files doesn't hand its name to the next one) and moves on from there until both
// names are free on the card (so a report whose files were copied off the card, or interrupted between the
// two writes, still leaves the old ones alone)
static int reportNumber(int lastFiled)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mw3ds", 0777);
	for (int i = lastFiled + 1; i <= lastFiled + 500; i++)
	{
		char path[96];
		struct stat st;
		snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/report_%02d.bmp", i);
		if (stat(path, &st) == 0)
			continue;
		snprintf(path, sizeof(path), "sdmc:/3ds/mw3ds/report_%02d_save.json", i);
		if (stat(path, &st) == 0)
			continue;
		return i;
	}
	return lastFiled + 1;      // nothing free in sight: take the next number and let the write say what happened
}

// The log's warnings, each "monitor:" line once (log.cpp keeps each key once per run): what the game missed
// while this place was loaded. Kept from the end of the log too, so a report says what happened just before it.
static void collectLog(std::vector<std::string>& warnings, std::vector<std::string>& tail, int& total)
{
	FILE* f = fopen("sdmc:/3ds/mw3ds/log.txt", "r");
	if (!f)
		return;
	std::string line;
	int c;
	auto flush = [&]() {
		if (line.empty())
			return;
		const char* m = strstr(line.c_str(), "monitor: ");
		if (m)
		{
			total++;
			if ((int)warnings.size() < kMaxWarnings)
				warnings.push_back(m + 9);
		}
		tail.push_back(line);
		if ((int)tail.size() > kMaxTail)
			tail.erase(tail.begin());
		line.clear();
	};
	while ((c = fgetc(f)) != EOF)
	{
		if (c == '\n')
			flush();
		else if (c != '\r' && line.size() < 4096)
			line += (char)c;
	}
	fclose(f);
	flush();
}

// The place as the game names it: an interior's name, outdoors the region when the grid square has no name
// of its own (the recipe in mwworld's displayCellName)
static std::string shownPlace(const World& w, const LevelCell& c)
{
	if (!c.name.empty())
		return c.name.str();
	if (!c.region.empty())
	{
		auto rg = w.game.regions.find(std::string(c.region.c_str()));
		if (rg != w.game.regions.end() && !rg->second.name.empty())
			return rg->second.name;
		return std::string(c.region.c_str());
	}
	return std::string();
}

// The file's "reports" array, made again if the file is missing or will not parse (its old contents kept
// under a new name, since a report someone has already copied off the card should still be there)
static cJSON* reportsRoot()
{
	size_t size = 0;
	char* text = zreadAll(kReportsPath, &size);
	if (text)
	{
		cJSON* root = cJSON_Parse(text);
		free(text);
		if (root && cJSON_IsObject(root) && cJSON_IsArray(cJSON_GetObjectItem(root, "reports")))
			return root;
		if (root)
			cJSON_Delete(root);
		char bad[128];
		snprintf(bad, sizeof(bad), "%s.bad", kReportsPath);
		remove(bad);
		rename(kReportsPath, bad);
		logf("report: %s would not parse, kept as %s", kReportsPath, bad);
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddItemToObject(root, "reports", cJSON_CreateArray());
	return root;
}

static void addStrings(cJSON* parent, const char* key, const std::vector<std::string>& lines)
{
	cJSON* arr = cJSON_AddArrayToObject(parent, key);
	for (const std::string& s : lines)
		cJSON_AddItemToArray(arr, cJSON_CreateString(s.c_str()));
}

// The report's own text: what the player typed (an empty description still files: the position and the
// warnings are the part a player cannot write down)
int bugReportFile(World& w, Session& session, const std::string& description, bool wantShot)
{
	// The file is read before anything is named, so the number follows the last entry filed here
	cJSON* root = reportsRoot();
	cJSON* list = cJSON_GetObjectItem(root, "reports");
	int lastFiled = 0;
	for (cJSON* e = list->child; e; e = e->next)
	{
		const cJSON* en = cJSON_GetObjectItem(e, "n");
		if (en && cJSON_IsNumber(en) && (int)en->valuedouble > lastFiled)
			lastFiled = (int)en->valuedouble;
	}
	int n = reportNumber(lastFiled);
	char savePath[96], shot[96];
	snprintf(savePath, sizeof(savePath), "sdmc:/3ds/mw3ds/report_%02d_save.json", n);
	snprintf(shot, sizeof(shot), "sdmc:/3ds/mw3ds/report_%02d.bmp", n);

	// The save copy first: the entry says whether it got written, and a card that refuses one file
	// likely refuses the rest
	bool saved = w.saveGame(savePath) == SAVE_WRITTEN;

	// The screenshot goes at the next C3D_FrameBegin (engine.cpp): screenshotSave wants the frame's transfers
	// finished, and a report is filed between frames, with the keyboard just closed
	bool shotAsked = wantShot;
	if (shotAsked)
		session.reportShot = shot;

	std::vector<std::string> warnings, tail;
	int logged = 0;
	collectLog(warnings, tail, logged);

	const LevelCell& c = w.current >= 0 ? w.cells[w.current] : LevelCell();
	cJSON* o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "n", n);
	cJSON_AddStringToObject(o, "text", description.c_str());
	cJSON_AddNumberToObject(o, "console_ms", (double)osGetTime());
	cJSON_AddStringToObject(o, "ingame", session.dateText(true).c_str());
	cJSON_AddNumberToObject(o, "hour", w.gameHour);
	cJSON_AddNumberToObject(o, "played_hours", w.time);
	cJSON_AddStringToObject(o, "player", w.stats.name.c_str());
	cJSON_AddNumberToObject(o, "level", w.stats.level);
	// the same x:/y:/z: wording the reports players type by hand use
	cJSON_AddNumberToObject(o, "x", w.player.feet[0]);
	cJSON_AddNumberToObject(o, "y", w.player.feet[1]);
	cJSON_AddNumberToObject(o, "z", w.player.feet[2]);
	cJSON_AddNumberToObject(o, "yaw", w.player.yaw);
	cJSON_AddNumberToObject(o, "pitch", w.player.pitch);
	cJSON_AddStringToObject(o, "cell", c.file.c_str());
	cJSON_AddStringToObject(o, "cell_name", shownPlace(w, c).c_str());
	cJSON_AddBoolToObject(o, "interior", c.interior);
	if (!c.interior)
	{
		cJSON_AddNumberToObject(o, "grid_x", c.gx);
		cJSON_AddNumberToObject(o, "grid_y", c.gy);
		cJSON_AddStringToObject(o, "region", c.region.c_str());
	}
	cJSON_AddStringToObject(o, "build", MW3DS_VERSION);
	cJSON_AddBoolToObject(o, "new_3ds", !g_profile.slowCpu);
	cJSON_AddBoolToObject(o, "small_heap", g_profile.smallHeap);
	cJSON_AddNumberToObject(o, "linear_free_kb", (double)(linearSpaceFree() / 1024));
	cJSON_AddStringToObject(o, "save", savePath);
	cJSON_AddBoolToObject(o, "save_written", saved);
	cJSON_AddStringToObject(o, "screenshot", shot);
	cJSON_AddBoolToObject(o, "screenshot_taken", shotAsked);      // engine.cpp's log line says if it failed
	cJSON_AddStringToObject(o, "save_in_use", session.savePath ? session.savePath : "");
	cJSON_AddNumberToObject(o, "warnings", logged);
	addStrings(o, "warning_lines", warnings);
	addStrings(o, "log_tail", tail);
	cJSON_AddItemToArray(cJSON_GetObjectItem(root, "reports"), o);

	// Written whole, to a .tmp, then renamed over the old file
	char* text = cJSON_Print(root);
	bool written = false;
	std::string tmp = std::string(kReportsPath) + ".tmp";
	if (text)
	{
		size_t len = strlen(text);
		if (FILE* f = fopen(tmp.c_str(), "w"))
		{
			written = fwrite(text, 1, len, f) == len;
			fclose(f);
			if (written)
			{
				remove(kReportsPath);
				written = rename(tmp.c_str(), kReportsPath) == 0;
			}
		}
		free(text);
	}
	if (!written)
		remove(tmp.c_str());
	cJSON_Delete(root);

	logf("report: %d %s %s (%d warnings) screenshot %s save %s%s", n, written ? "added to" : "FAILED",
		kReportsPath, logged, shotAsked ? "asked for" : "skipped", saved ? "saved" : "FAILED",
		description.empty() ? "" : " (no description typed)");
	session.notify(written ? ("Report #" + std::to_string(n) + " saved to reports.json").c_str()
		: "Report failed: reports.json could not be written");
	return written ? n : 0;
}

// The game menu's button: the main loop puts up the system keyboard, whose answer comes back to
// Session::setText, which files the report
void bugReportPrompt(Session& session)
{
	session.wantText = Session::TEXT_BUGREPORT;
}

// The console's "report [what went wrong]" line, tried before a line is compiled (DevConsole::commandHook).
// The word alone files a report with no description: the position, the cell and the warnings are the part a
// player cannot write down. Returns 0 for any other line, which compiles as a script line as before.
int bugReportCommand(World& w, Session& session, const std::string& line)
{
	size_t at = line.find_first_not_of(" \t");
	if (at == std::string::npos || at + 6 > line.size())
		return 0;
	for (int i = 0; i < 6; i++)
		if (tolower((unsigned char)line[at + i]) != "report"[i])
			return 0;
	if (at + 6 < line.size() && line[at + 6] != ' ' && line[at + 6] != '\t')
		return 0;                                  // "report_of_firemoth": a script's word, not this command
	size_t from = line.find_first_not_of(" \t", at + 6);
	std::string text = from == std::string::npos ? std::string()
		: line.substr(from, line.find_last_not_of(" \t") - from + 1);
	return bugReportFile(w, session, text);
}
