#include "mwgui/savegamedialog.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <cmath>
#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <array>
#include <map>
#include <functional>
#include <tex3ds.h>
#include <citro2d.h>
#include <cstdlib>
#include <malloc.h>

static std::string formatTime(u64 ms)
{
	if (ms == 0)
		return "";
	// osGetTime counts from 1900-01-01; time_t from 1970-01-01
	time_t t = (time_t)(ms / 1000 - 2208988800ULL);
	struct tm* tm = gmtime(&t);
	char buf[32];
	strftime(buf, sizeof(buf), "%d %b %Y %H:%M", tm);
	return buf;
}

s64 g_saveClockShiftMs = 0;
std::string g_lastSavePath;
SaveInfo g_lastSaveInfo;
// One past the highest save count on the card, known once savesList has read the saves (0: not read yet)
static u32 g_nextOrder = 0;
// The saves as savesList last read them, kept as the game writes and deletes them: on the console each save's file
// checked and its .meta opened took a moment on the card, and the Saves screen read them all at every opening. Read
// once (the title screen lists them at boot), then the list here. Files changed on the card behind the game's back
// (over FTP) show at the next start
static std::vector<SaveInfo> s_saveCache;
static std::string s_saveCacheDir;
static bool s_saveCacheOk = false;
int g_saveListReads = 0, g_saveMetaReads = 0;

static bool readInfo(const std::string& path, SaveInfo& out, bool listed = false);
static void parseMeta(char* line, SaveInfo& out);
// The player's saves' details in one file, so listing them reads it and the folder's names instead of opening a .meta
// a save (with hundreds of saves, as many file opens on the card): a line a save written, "<file>\t<its .meta line>",
// and "-\t<file>" a save deleted, the later line winning. A save it doesn't know (an older build's, or one copied
// onto the card) is read from its .meta; then, or once it holds many more lines than saves, it is written again whole.
// The .meta files are still written beside the saves (older builds read them)
static const char* const kSaveIndex = "sdmc:/3ds/mw3ds/saves/index.txt";

static std::string fileOf(const std::string& path)
{
	return path.substr(path.find_last_of('/') + 1);
}

static void appendIndex(const std::string& line)
{
	if (FILE* f = fopen(kSaveIndex, "a"))
	{
		fputs(line.c_str(), f);
		fclose(f);
	}
}
static std::vector<std::string> jsonFiles(const std::string& dir);

void writeSaveMeta(const char* savePath, const std::string& name, const std::string& place, int level, u64 timeMs)
{
	timeMs += g_saveClockShiftMs;
	// The save's count, one past the highest of the player's saves: the list is ordered by it, as the console's clock
	// can be reset or set back (a save made then sorted below older ones by its time). Taken from the list already
	// read, as every directory read on the card right after the save's write took seconds on the console; the saves
	// are read once only when nothing has listed them yet (a test's SAVE with the Saves screen never opened)
	u32 order = 0;
	if (strcmp(savePath, kAutosavePath) != 0)
	{
		g_lastSavePath = savePath;
		if (g_nextOrder == 0)
		{
			for (auto& p : jsonFiles(kSaveDir))
			{
				SaveInfo s;
				if (p != savePath && readInfo(p, s) && s.order > order)
					order = s.order;
			}
			g_nextOrder = order + 1;
		}
		order = g_nextOrder++;
		SaveInfo& s = g_lastSaveInfo;
		s = SaveInfo();
		s.path = savePath;
		s.title = name == place ? name : name + " - " + place;
		s.name = name;
		s.place = place;
		s.level = level;
		s.time = timeMs;
		s.order = order;
		s.detail = "Level " + std::to_string(level) + ", " + formatTime(timeMs);
	}
	char line[320];
	snprintf(line, sizeof(line), "%s\t%s\t%d\t%llu\t%lu\n", name.c_str(), place.c_str(), level, timeMs, (unsigned long)order);
	std::string meta = std::string(savePath) + ".meta";
	if (FILE* f = fopen(meta.c_str(), "w"))
	{
		fputs(line, f);
		fclose(f);
	}
	if (order)
		appendIndex(fileOf(savePath) + "\t" + line);
	// the kept list as a fresh read would now find it: the save at the top of the player's own, the autosave first
	if (!s_saveCacheOk)
		return;
	s_saveCache.erase(std::remove_if(s_saveCache.begin(), s_saveCache.end(),
		[&](const SaveInfo& s) { return s.path == savePath; }), s_saveCache.end());
	if (order)
	{
		addSaveToList(s_saveCache);
		return;
	}
	SaveInfo a;
	a.path = savePath;
	a.autosave = true;
	a.title = name == place ? name : name + " - " + place;
	a.name = name;
	a.place = place;
	a.level = level;
	a.time = timeMs;
	a.detail = "Autosave, Level " + std::to_string(level) + ", " + formatTime(timeMs);
	s_saveCache.insert(s_saveCache.begin(), a);
}

void addSaveToList(std::vector<SaveInfo>& list)
{
	// newest of the player's own saves: right after the autosave row when there is one
	size_t at = 0;
	while (at < list.size() && list[at].autosave)
		at++;
	list.insert(list.begin() + at, g_lastSaveInfo);
}

// listed: the file came from the folder's own listing (no check that it is there: one card access less a save)
static bool readInfo(const std::string& path, SaveInfo& out, bool listed)
{
	struct stat st;
	if (!listed && stat(path.c_str(), &st) != 0)
		return false;
	out.path = path;
	out.title = path.substr(path.find_last_of('/') + 1);
	g_saveMetaReads++;
	FILE* f = fopen((path + ".meta").c_str(), "r");
	if (!f)
		return true;
	char line[256] = {};
	if (fgets(line, sizeof(line), f))
		parseMeta(line, out);
	fclose(f);
	return true;
}

// A .meta line (name, place, level, time, save count, tab-separated) into the save's row
static void parseMeta(char* line, SaveInfo& out)
{
	line[strcspn(line, "\r\n")] = 0;
	char* fields[5] = {};
	int n = 0;
	for (char* p = line; n < 5; n++)
	{
		fields[n] = p;
		char* tab = strchr(p, '\t');
		if (!tab)
		{
			n++;
			break;
		}
		*tab = 0;
		p = tab + 1;
	}
	if (n >= 2)
	{
		out.title = strcmp(fields[0], fields[1]) == 0 ? fields[0] : std::string(fields[0]) + " - " + fields[1];
		out.name = fields[0];
		out.place = fields[1];
	}
	if (n >= 3)
	{
		out.detail = std::string("Level ") + fields[2];
		out.level = atoi(fields[2]);
	}
	if (n >= 4)
		out.time = strtoull(fields[3], nullptr, 10);
	if (n >= 5)
		out.order = strtoul(fields[4], nullptr, 10);
}

static std::vector<std::string> jsonFiles(const std::string& dir)
{
	std::vector<std::string> out;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return out;
	while (struct dirent* e = readdir(d))
	{
		std::string name = e->d_name;
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
			out.push_back(dir + "/" + name);
	}
	closedir(d);
	return out;
}

std::vector<SaveInfo> savesList(const char* dataDir)
{
	if (s_saveCacheOk && s_saveCacheDir == dataDir)
		return s_saveCache;
	g_saveListReads++;
	std::vector<SaveInfo> out, mine, bundled;
	SaveInfo info;
	if (readInfo(kAutosavePath, info))
	{
		info.autosave = true;
		info.detail = "Autosave" + (info.detail.empty() ? "" : ", " + info.detail);
		out.push_back(info);
	}
	std::map<std::string, std::string> index;
	int indexLines = 0;
	if (FILE* f = fopen(kSaveIndex, "r"))
	{
		char line[400];
		while (fgets(line, sizeof(line), f))
		{
			line[strcspn(line, "\r\n")] = 0;
			char* tab = strchr(line, '\t');
			if (!tab)
				continue;
			*tab = 0;
			indexLines++;
			if (strcmp(line, "-") == 0)
				index.erase(tab + 1);
			else
				index[line] = tab + 1;
		}
		fclose(f);
	}
	size_t known = 0;
	bool learned = false;
	for (auto& p : jsonFiles(kSaveDir))
	{
		SaveInfo s;
		auto it = index.find(fileOf(p));
		if (it != index.end())
		{
			s.path = p;
			s.title = it->first;
			std::string line = it->second;
			parseMeta(&line[0], s);
			mine.push_back(s);
			known++;
		}
		else if (readInfo(p, s, true))
		{
			mine.push_back(s);
			learned = learned || !s.name.empty();
		}
	}
	// (written again whole: saves it lacked, saves gone from the card, or many lines over the saves it lists)
	if (learned || index.size() > known || indexLines > (int)known + 32)
		if (FILE* f = fopen(kSaveIndex, "w"))
		{
			for (const SaveInfo& s : mine)
				if (!s.name.empty())
					fprintf(f, "%s\t%s\t%s\t%d\t%llu\t%lu\n", fileOf(s.path).c_str(), s.name.c_str(), s.place.c_str(),
						s.level, s.time, (unsigned long)s.order);
			fclose(f);
		}
	// Newest first: by the save count, then (older saves without one) by the time they were made
	std::sort(mine.begin(), mine.end(), [](const SaveInfo& a, const SaveInfo& b) {
		return a.order != b.order ? a.order > b.order : a.time > b.time;
	});
	g_nextOrder = (mine.empty() ? 0 : mine[0].order) + 1;
	for (auto& p : jsonFiles(std::string(dataDir) + "/saves"))
	{
		SaveInfo s;
		if (readInfo(p, s, true))
		{
			s.bundled = true;
			s.detail = "Starting point" + (s.detail.empty() ? "" : ", " + s.detail);
			s.time = 0;
			bundled.push_back(s);
		}
	}
	for (auto& s : mine)
		if (s.time)
			s.detail += ", " + formatTime(s.time);
	if (!out.empty() && out[0].time)
		out[0].detail += ", " + formatTime(out[0].time);
	out.insert(out.end(), mine.begin(), mine.end());
	out.insert(out.end(), bundled.begin(), bundled.end());
	s_saveCache = out;
	s_saveCacheDir = dataDir;
	s_saveCacheOk = true;
	return out;
}

void forgetSavesList()
{
	s_saveCacheOk = false;
}

// A save deleted: gone from the kept list as well
static void forgetSave(const std::string& path)
{
	s_saveCache.erase(std::remove_if(s_saveCache.begin(), s_saveCache.end(),
		[&](const SaveInfo& s) { return s.path == path; }), s_saveCache.end());
}

std::string saveNewPath()
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mw3ds", 0777);
	mkdir(kSaveDir, 0777);
	// the first number free: past those the kept list holds without asking the card (one check, of that one)
	for (int i = 1; i < 1000; i++)
	{
		char path[96];
		snprintf(path, sizeof(path), "%s/save_%03d.json", kSaveDir, i);
		if (s_saveCacheOk && std::any_of(s_saveCache.begin(), s_saveCache.end(), [&](const SaveInfo& s) { return s.path == path; }))
			continue;
		struct stat st;
		if (stat(path, &st) != 0)
			return path;
	}
	return std::string(kSaveDir) + "/save_999.json";
}

void saveRow(const std::vector<SaveInfo>& saves, size_t i, std::string& left, std::string& right)
{
	const SaveInfo& s = saves[i];
	bool names = false;               // (one character's saves: its name on every row says nothing)
	for (const SaveInfo& o : saves)
		names = names || (!o.name.empty() && !s.name.empty() && o.name != s.name);
	left = s.place.empty() ? s.title : names ? s.name + " - " + s.place : s.place;
	if (s.autosave)
		left = "Autosave: " + left;
	else if (s.bundled)
		left = "Starting point: " + left;
	right.clear();
	if (s.level > 0)
		right = "Lv " + std::to_string(s.level);
	if (s.time && !s.bundled)
	{
		time_t t = (time_t)(s.time / 1000 - 2208988800ULL);
		const struct tm* tm = gmtime(&t);
		char buf[32] = {};
		if (tm)              // month / day and the hour: "10/18 13:18"
			snprintf(buf, sizeof(buf), "%d/%d %02d:%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
		right += (right.empty() ? "" : "  ") + std::string(buf);
	}
}

int uiSaveList(UiList& list, const std::vector<SaveInfo>& saves, float x, float y, float w, float h, bool keys)
{
	std::vector<std::string> rows, right;
	for (size_t i = 0; i < saves.size(); i++)
	{
		rows.emplace_back();
		right.emplace_back();
		saveRow(saves, i, rows.back(), right.back());
	}
	return uiList(list, x, y, w, h, rows, 0.45f, keys, &right);
}

// Save a new game or load any save (autosave, your saves, bundled starting points)
void Session::drawSaves()
{
	header("Saves");
	int pick = uiSaveList(list, saveList, 4, 24, 312, 176, !deletingSave);
	if (saveList.empty())
		uiTextCentered(160, 100, 0.5f, col::textDim, "No saves yet");
	// Saving mid-chargen would lose the menus the scripts queued, so wait until they're done
	bool canSave = pendingMenus.empty() && !wantName && !playerDead;     // (dead: load only)
	bool canLoad = list.selected >= 0 && list.selected < (int)saveList.size();
	// (the starting points that come with the game stay)
	bool canDelete = canLoad && !saveList[list.selected].bundled;
	if (deletingSave)
	{
		// "Delete <name>?" Yes / No
		if (!canDelete)
			deletingSave = false;
		else
		{
			// (a solid box over the list, and its two buttons inside it, not across the whole screen as a button row)
			uiRect(20, 70, 280, 100, C2D_Color32(0, 0, 0, 255));
			uiPanel(20, 70, 280, 100);
			uiTextBox(scroll, 26, 76, 268, 50, w.game.gmstLabel("sdelete", "Delete") + " " + saveList[list.selected].title + "?", 0.5f, false);
			if (uiIn().down & (KEY_LEFT | KEY_RIGHT))
				focus = 1 - focus;
			int yn = -1;
			const std::string kAnswer[2] = { w.game.gmstLabel("syes", "Yes"), w.game.gmstLabel("sno", "No") };
			for (int i = 0; i < 2; i++)
				if (uiButton(34 + i * 132, 132, 120, 28, kAnswer[i], i == focus))
					yn = i;
			if (yn == 0)
			{
				remove(saveList[list.selected].path.c_str());
				remove((saveList[list.selected].path + ".meta").c_str());
				logf("saves: deleted %s", saveList[list.selected].path.c_str());
				forgetSave(saveList[list.selected].path);
				appendIndex("-\t" + fileOf(saveList[list.selected].path) + "\n");
				saveList = savesList(dataDir);
				if (list.selected >= (int)saveList.size())
					list.selected = (int)saveList.size() - 1;
				notify("Save deleted.");
			}
			if (yn >= 0 || (uiIn().down & KEY_B))
			{
				deletingSave = false;
				focus = 0;
			}
		}
		return;
	}
	int b = buttonRow({ canSave ? "New save" : playerDead ? "(dead)" : "(finish chargen)", "Load", w.game.gmstLabel("sdelete", "Delete"),
		w.game.gmstLabel("sback", "Back") }, focus);
	if (b == 2 && canDelete)
	{
		deletingSave = true;
		focus = 1;               // (No)
	}
	else if (b == 0 && canSave)
	{
		std::string path = saveNewPath();
		if (w.saveGame(path.c_str()))
		{
			notify("Game saved.");
			addSaveToList(saveList);
		}
	}
	else if ((b == 1 || (pick >= 0 && (uiIn().down & KEY_A))) && canLoad)
	{
		reloadPath = saveList[list.selected].path;
		wantReload = true;
		closeScreen();
	}
	else if (b == 3 || (uiIn().down & KEY_B))
	{
		if (playerDead)
			openScreen(SCR_DEATH);     // back to the death screen: no way back into the game but a load
		else
			closeScreen();
	}
}
