// Session's test hooks: give, place, go to a cell
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"

void Session::testBoost(int value)
{
	w.recomputeStats();
	for (int i = 0; i < 8; i++)
		w.stats.attrBonus[i] += value - w.stats.attributes[i];
	for (int k = 0; k < 27; k++)
		w.stats.skillBonus[k] += value - w.stats.skills[k];
	w.recomputeStats();
	logf("test: boost to %d, health %.0f fatigue %.0f", value, w.stats.healthMax, w.stats.fatigueMax);
}

static std::string underscoresToSpaces(std::string s)
{
	for (auto& ch : s)
		if (ch == '_')
			ch = ' ';
	return s;
}

int Session::testFindRef(const std::string& id)
{
	// the creature the test placed, even once it is dead: another with its id (a spawn in a loaded neighbour) is not it
	if (testPlaced >= 0 && w.active(testPlaced))
	{
		const std::string& pid = w.refs[testPlaced].idLower;
		if (pid == lower(id) || pid == lower(underscoresToSpaces(id)))
			return testPlaced;
	}
	// (or one PLACEAT put somewhere: ten people placed, one of them an Imperial Guard among many)
	for (int i : testPlacedAll)
		if (w.active(i) && (w.refs[i].idLower == lower(id) || w.refs[i].idLower == lower(underscoresToSpaces(id))))
			return i;
	int ref = w.findRef(id);
	if (!w.active(ref))
	{
		int spaced = w.findRef(underscoresToSpaces(id));
		if (w.active(spaced) || ref < 0)
			ref = spaced;
	}
	// a dead one when a living one shares the id (a creature placed again): the living one
	if (w.active(ref) && w.refs[ref].dead)
	{
		const std::string want = w.refs[ref].idLower;
		w.forLoadedRefs([&](int i) {
			if (w.refs[ref].dead && w.refs[i].idLower == want && !w.refs[i].dead)
				ref = i;
		});
	}
	return ref;
}

void Session::testGive(const std::string& id, int count, bool topUp)
{
	const Object* o = w.game.object(id);
	if (!o)
		o = w.game.object(underscoresToSpaces(id));
	if (!o)
	{
		logf("test: no item %s", id.c_str());
		return;
	}
	bool ammo = o->type == "WEAP" && o->subtype >= 12;
	int want = count > 0 ? count : ammo ? 50 : 1;
	// (a chained chapter: only up to the count, so an item the last chapter's story handed over isn't doubled)
	if (topUp)
		want -= w.itemCount(o->id);
	if (want > 0)
		w.addItem(o->id, want);
	// (lockpicks and probes are only added: equipping one would replace the weapon in hand, and OpenMW never equips on
	// add; USELOCKPICK / USEPROBE / EQUIP hold one when a case needs it)
	bool wearable = o->type == "WEAP" || o->type == "ARMO" || o->type == "CLOT" || o->type == "LIGH";
	for (size_t k = 0; k < w.inventory.size() && wearable; k++)
		if (lower(w.inventory[k].id) == lower(o->id))
			equipItem(k, true);
	logf("test: gave %s%s", o->id.c_str(), wearable ? " and equipped it" : "");
}

void Session::testPlace(const std::string& id, float dist)
{
	float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
	float to[3] = { w.player.feet[0] + fx * dist, w.player.feet[1] + fy * dist, w.player.feet[2] };
	testPlaceAt(id, to, atan2f(-fx, -fy));
}

// The actor at that point (on the floor there), turned to yaw: PLACEAT, and PLACE in front of the player
void Session::testPlaceAt(const std::string& id, const float at[3], float yaw)
{
	int ri = testFindRef(id);
	if (!w.active(ri) || w.refs[ri].dead)
	{
		// None to move (or only a corpse): a new one, as PlaceAtPC would
		ri = w.spawnActor(id, w.player.feet, yaw);
		if (ri < 0)
		{
			logf("test: %s not loaded", id.c_str());
			return;
		}
	}
	Ref& r = w.refs[ri];
	Cell* cell = w.cellOf(ri);
	float to[3] = { at[0], at[1], at[2] };
	float fz;
	if (cell && collisionFloor(cell->collision, to[0], to[1], to[2] + 200.0f, to[2] - 400.0f, &fz))
		to[2] = fz;
	for (int k = 0; k < 3; k++)
		r.pos[k] = r.home[k] = to[k];
	r.fitBox();
	r.rot[2] = yaw;
	r.moved = true;
	w.syncActor(ri);
	testPlaced = ri;
	testPlacedAll.insert(ri);
	logf("test: placed %s at %.0f %.0f %.0f", r.id.c_str(), to[0], to[1], to[2]);
}

// COC (the console): a named exterior (its street, for a town) or an interior, entered where a door into it lands
// (OpenMW's findExteriorPosition / findInteriorPosition); false when the level has no such place
bool Session::centerOnCell(const std::string& name)
{
	std::string want = lower(name);
	bool known = w.cellIndex(name) >= 0 || w.game.townSpawns.count(want);
	for (size_t i = 0; i < w.refs.size() && !known; i++)
		known = w.refs[i].hasDest && lower(w.refs[i].destCell) == want;
	if (!known)
		return false;
	testGoto(name);
	return true;
}

// COE x y (the console): the middle of that exterior cell, set down on the ground
bool Session::centerOnExterior(int x, int y)
{
	int cell = w.gridCell(x, y);
	if (cell < 0)
		return false;
	travelCell = cell;
	travelPos[0] = (x + 0.5f) * 8192.0f;
	travelPos[1] = (y + 0.5f) * 8192.0f;
	travelPos[2] = 0.0f;
	travelYaw = 0.0f;
	travelToSpawn = true;
	return true;
}

// Enters a cell through a door that leads there (any loaded door; else the first one found)
void Session::testGoto(const std::string& cellName)
{
	std::string want = lower(cellName);
	int door = -1;
	for (size_t i = 0; i < w.refs.size(); i++)
	{
		const Ref& r = w.refs[i];
		if (r.hasDest && lower(r.destCell) == want && (door < 0 || w.active(i)))
			door = i;
	}
	if (door < 0)
	{
		// No door read yet (cells' objects load on demand): straight in; a TP step places the player
		int dest = w.cellIndex(cellName);
		if (dest < 0)
		{
			logf("test: %s is not in the level", cellName.c_str());
			return;
		}
		travelCell = dest;
		travelPos[0] = travelPos[1] = travelPos[2] = 0.0f;
		travelYaw = 0.0f;
		// an exterior cell: its middle (the position decides which cells load outdoors), not world 0 0
		if (!w.cells[dest].interior)
		{
			travelPos[0] = (w.cells[dest].gx + 0.5f) * 8192.0f;
			travelPos[1] = (w.cells[dest].gy + 0.5f) * 8192.0f;
			travelToSpawn = true;
			// a town: its street, in front of a door (the converter's pick, as a new game there starts), not the
			// cell's middle (Seyda Neen's is out in the bay)
			auto town = w.game.townSpawns.find(want);
			int at = town != w.game.townSpawns.end()
				? w.gridCell((int)floorf(town->second[0] / 8192.0f), (int)floorf(town->second[1] / 8192.0f)) : -1;
			if (at >= 0)
			{
				travelCell = at;
				memcpy(travelPos, town->second.data(), sizeof(travelPos));
				travelYaw = town->second[3];
				travelToSpawn = false;
				logf("test: going into %s (its street)", cellName.c_str());
				return;
			}
		}
		// where a door into it lands (converter: cell_entries.txt), not the cell's origin (the void, often)
		static std::unordered_map<std::string, std::array<float, 4>> entries;
		if (entries.empty())
		{
			char path[256];
			snprintf(path, sizeof(path), "%s/cell_entries.txt", w.dataDir);
			if (FILE* f = fopen(path, "r"))
			{
				char line[320];
				while (fgets(line, sizeof(line), f))
				{
					char* tab = strchr(line, '	');
					std::array<float, 4> e;
					if (tab && sscanf(tab + 1, "%f %f %f %f", &e[0], &e[1], &e[2], &e[3]) == 4)
						entries[std::string(line, tab - line)] = e;
				}
				fclose(f);
			}
		}
		auto e = entries.find(want);
		if (e != entries.end())
		{
			memcpy(travelPos, e->second.data(), sizeof(travelPos));
			travelYaw = e->second[3];
			logf("test: going into %s (where its door lands)", cellName.c_str());
			return;
		}
		logf("test: going straight into %s", cellName.c_str());
		return;
	}
	const Ref& r = w.refs[door];
	int dest = r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
	if (dest < 0)
	{
		logf("test: %s is not in the level", cellName.c_str());
		return;
	}
	travelCell = dest;
	memcpy(travelPos, r.destPos, sizeof(travelPos));
	travelYaw = r.destRot[2];
	logf("test: going to %s", cellName.c_str());
}
