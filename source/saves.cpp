#include "saves.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

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
			out.title = strcmp(fields[0], fields[1]) == 0 ? fields[0] : std::string(fields[0]) + " - " + fields[1];
		if (n >= 3)
			out.detail = std::string("Level ") + fields[2];
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

int uiSaveList(UiList& list, const std::vector<SaveInfo>& saves, float x, float y, float w, float h, bool keys)
{
	std::vector<std::string> rows;
	for (auto& s : saves)
		rows.push_back(s.title + "   (" + s.detail + ")");
	return uiList(list, x, y, w, h, rows, 0.45f, keys);
}
