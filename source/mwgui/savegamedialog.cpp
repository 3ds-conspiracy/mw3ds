#include "mwgui/savegamedialog.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <cmath>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <array>
#include <functional>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include <cstdlib>
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"
#include "mwgui/windowhelpers.hpp"

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

static bool readInfo(const std::string& path, SaveInfo& out);
static std::vector<std::string> jsonFiles(const std::string& dir);

void writeSaveMeta(const char* savePath, const std::string& name, const std::string& place, int level, u64 timeMs)
{
	timeMs += g_saveClockShiftMs;
	// The save's count, one past the highest of the player's saves: the list is ordered by it, as the console's clock
	// can be reset or set back (a save made then sorted below older ones by its time)
	u32 order = 0;
	if (strcmp(savePath, kAutosavePath) != 0)
	{
		g_lastSavePath = savePath;
		for (auto& p : jsonFiles(kSaveDir))
		{
			SaveInfo s;
			if (p != savePath && readInfo(p, s) && s.order > order)
				order = s.order;
		}
		order++;
	}
	std::string meta = std::string(savePath) + ".meta";
	if (FILE* f = fopen(meta.c_str(), "w"))
	{
		fprintf(f, "%s\t%s\t%d\t%llu\t%lu\n", name.c_str(), place.c_str(), level, timeMs, (unsigned long)order);
		fclose(f);
	}
}

static bool readInfo(const std::string& path, SaveInfo& out)
{
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return false;
	out.path = path;
	out.title = path.substr(path.find_last_of('/') + 1);
	FILE* f = fopen((path + ".meta").c_str(), "r");
	if (!f)
		return true;
	char line[256] = {};
	if (fgets(line, sizeof(line), f))
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
	fclose(f);
	return true;
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
	std::vector<SaveInfo> out, mine, bundled;
	SaveInfo info;
	if (readInfo(kAutosavePath, info))
	{
		info.autosave = true;
		info.detail = "Autosave" + (info.detail.empty() ? "" : ", " + info.detail);
		out.push_back(info);
	}
	for (auto& p : jsonFiles(kSaveDir))
	{
		SaveInfo s;
		if (readInfo(p, s))
			mine.push_back(s);
	}
	// Newest first: by the save count, then (older saves without one) by the time they were made
	std::sort(mine.begin(), mine.end(), [](const SaveInfo& a, const SaveInfo& b) {
		return a.order != b.order ? a.order > b.order : a.time > b.time;
	});
	for (auto& p : jsonFiles(std::string(dataDir) + "/saves"))
	{
		SaveInfo s;
		if (readInfo(p, s))
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
	return out;
}

std::string saveNewPath()
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mw3ds", 0777);
	mkdir(kSaveDir, 0777);
	for (int i = 1; i < 1000; i++)
	{
		char path[96];
		snprintf(path, sizeof(path), "%s/save_%03d.json", kSaveDir, i);
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
			notify("Game saved.");
		saveList = savesList(dataDir);
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
