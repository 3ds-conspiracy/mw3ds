#include "mwworld/worldimp.hpp"
#include "mwrender/objectpaging.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "mwworld/worldhelpers.hpp"
#include "platform/console.hpp"

float* ScriptInstance::local(const std::string& lowerName)
{
	int i = script ? script->localIndex(lowerName) : -1;
	return i < 0 ? nullptr : &locals[i];
}

int World::spawnActor(const std::string& id, const float pos[3], float yaw, int inCell)
{
	auto sp = game.spawns.find(lower(id));
	if (sp == game.spawns.end() || sp->second.actor < 0 || sp->second.actor >= (int)game.actors.size())
	{
		logf("world: can't place %s (not converted for placing)", id.c_str());
		return -1;
	}
	const SpawnDef& d = sp->second;
	// The cell under the spot (outdoors a neighbour of the player's may hold it)
	int c = inCell >= 0 ? inCell : current;
	if (inCell < 0 && !cells[current].interior)
	{
		int g = gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		if (g >= 0 && cells[g].live)
			c = g;
	}
	Ref r;
	r.cell = c;
	r.id = game.actors[d.actor].id;
	r.idLower = lower(r.id);
	r.type = d.type; r.recType = recTypeOf(r.type.c_str());
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.rot[0] = r.rot[1] = 0.0f;
	r.rot[2] = yaw;
	r.actor = d.actor;
	r.obj = game.object(r.id);
	r.moved = true;
	// A box to aim at, a person's size (none before: people placed by scripts couldn't be talked to, nor
	// their bodies searched)
	r.hasBox = true;
	memcpy(r.start, pos, sizeof(r.pos));
	r.start[3] = yaw;
	r.boxStart[0] = pos[0] - 30.0f; r.boxStart[1] = pos[1] - 30.0f; r.boxStart[2] = pos[2];
	r.boxStart[3] = pos[0] + 30.0f; r.boxStart[4] = pos[1] + 30.0f; r.boxStart[5] = pos[2] + 128.0f;
	r.fitBox();
	fillContents(game, r, stats.level, (unsigned)rand());
	initActorState(game, r);
	int slot = takeSlots(freeRefs, 1);
	if (slot < 0)
	{
		slot = refs.size();
		refs.emplace_back();
	}
	refs[slot] = std::move(r);
	refs[slot].spawnedRef = true;
	refsChanged();
	spawned.push_back(slot);
	startScriptFor(slot);
	attachSpawned(slot);
	rebuildLoadedLists();
	logf("world: placed %s at %.0f %.0f %.0f", id.c_str(), pos[0], pos[1], pos[2]);
	return slot;
}

void World::attachSpawned(int ri)
{
	Ref& r = refs[ri];
	LoadedCell* l = r.cell >= 0 ? cells[placeOf(ri)].live : nullptr;
	auto sp = game.spawns.find(r.idLower);
	const ActorDef* def = r.actor >= 0 ? &game.actors[r.actor] : nullptr;
	std::string lib = sp != game.spawns.end() ? sp->second.lib : def ? def->lib.str() : "";
	int skel = sp != game.spawns.end() ? sp->second.skeleton : def ? def->skel : -1;
	if (!l || lib.empty() || skel < 0)
		return;
	// A person's box to aim at where they stand, as spawnActor gives one: a leveled list's pick drawn from its own
	// library (Solstheim's reavers and smugglers) had none, and the crosshair never found them
	if (!r.hasBox)
	{
		r.hasBox = true;
		memcpy(r.start, r.pos, sizeof(r.pos));
		r.start[3] = r.rot[2];
		r.boxStart[0] = r.pos[0] - 30.0f; r.boxStart[1] = r.pos[1] - 30.0f; r.boxStart[2] = r.pos[2];
		r.boxStart[3] = r.pos[0] + 30.0f; r.boxStart[4] = r.pos[1] + 30.0f; r.boxStart[5] = r.pos[2] + 128.0f;
		r.fitBox();
	}
	// (at its size, as a placement of it has: a Grizzly's record makes it 1.5, a person's race height; OpenMW's
	// adjustScale. Unscaled, a spawned Grizzly stood two thirds of OpenMW's: case bug-spawned-scale)
	const float across = sp != game.spawns.end() ? sp->second.size[0] : 1.0f;
	const float up = sp != game.spawns.end() ? sp->second.size[1] : 1.0f;
	float c = cosf(r.rot[2]) * across, s = sinf(r.rot[2]) * across;
	float place[12] = { c, s, 0.0f, r.pos[0], -s, c, 0.0f, r.pos[1], 0.0f, 0.0f, up, r.pos[2] };
	// (lit by the cell's light as a placement there is: the library's even light stood out dark)
	r.anim = actorsAddFromLibrary(l->actors, l->cell, textures, dataDir, lib, ri, skel, place, l->actors.cellLight);
	CellActor ca = {};
	strncpy(ca.id, r.id.c_str(), sizeof(ca.id) - 1);
	strncpy(ca.name, game.actors[r.actor].name.c_str(), sizeof(ca.name) - 1);
	memcpy(ca.pos, r.pos, sizeof(ca.pos));
	ca.yaw = r.rot[2];
	ca.radius = 28.0f;
	ca.height = 128.0f;
	l->cell.actors.push_back(ca);
	r.cellActor = l->cell.actors.size() - 1;
	syncActor(ri);
}

void World::werewolfBody(int ri)
{
	Ref& r = refs[ri];
	LoadedCell* l = r.cell >= 0 ? cells[placeOf(ri)].live : nullptr;
	ActorSet* set = r.anim >= 0 ? actorsOf(ri) : nullptr;
	if (!l)
		return;
	if (r.werewolf && r.humanAnim < 0)
	{
		auto sp = game.spawns.find("bm_werewolf_default");
		if (sp == game.spawns.end() || !set || r.anim >= (int)set->actors.size())
			return;
		float c = cosf(r.rot[2]), s = sinf(r.rot[2]);
		float place[12] = { c, s, 0.0f, r.pos[0], -s, c, 0.0f, r.pos[1], 0.0f, 0.0f, 1.0f, r.pos[2] };
		// (in the light their own body has where it stands)
		u8 tint[3];
		memcpy(tint, set->actors[r.anim].tint, sizeof(tint));
		int wolf = actorsAddFromLibrary(l->actors, l->cell, textures, dataDir, sp->second.lib, ri, sp->second.skeleton, place,
			tint);
		if (wolf < 0)
			return;
		const Actor& human = set->actors[r.anim];
		memcpy(set->actors[wolf].lamp, human.lamp, sizeof(human.lamp));
		set->actors[wolf].lampSplit = human.lampSplit;
		set->actors[r.anim].ref = -1;          // (their own model kept, not drawn)
		r.humanAnim = r.anim;
		r.anim = wolf;
	}
	else if (!r.werewolf && r.humanAnim >= 0)
	{
		if (set && r.anim < (int)set->actors.size())
			set->actors[r.anim].ref = -1;
		if (set && r.humanAnim < (int)set->actors.size())
			set->actors[r.humanAnim].ref = ri;
		r.anim = r.humanAnim;
		r.humanAnim = -1;
	}
	syncActor(ri);
}

void World::despawn(int ri)
{
	if (ri < 0 || ri >= (int)refs.size() || !refs[ri].spawnedRef)
		return;
	detachActor(ri);
	for (size_t s = 0; s < scripts.size(); s++)
		if (scripts[s].ref == ri && scripts[s].item.empty())
		{
			scripts.erase(scripts.begin() + s);
			break;
		}
	for (size_t k = 0; k < spawned.size(); k++)
		if (spawned[k] == ri)
		{
			spawned.erase(spawned.begin() + k);
			break;
		}
	for (auto& s : scripts)
		if (s.target == ri)
			s.target = -1;
	refs[ri] = Ref();
	refs[ri].cell = -1;
	freeRefSlots(*this, ri, 1);
	rebuildScriptIndex();
	rebuildLoadedLists();
}

// Gone for good: a spawned one is freed, a placed one is marked taken (hidden, and kept so by the cell's state)
void World::deleteRef(int ri)
{
	Ref& r = refs[ri];
	if (r.spawnedRef)
	{
		despawn(ri);
		return;
	}
	r.pickedUp = true;
	hideRanges(*this, r);
}

int World::dropItem(const InventoryItem& it, const float pos[3], float yaw, int inCell)
{
	const Object* o = game.object(it.id);
	if (!o || it.count <= 0)
		return -1;
	int c = inCell >= 0 ? inCell : current;
	if (inCell < 0 && c >= 0 && !cells[c].interior)
	{
		int g = gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		if (g >= 0 && cells[g].live)
			c = g;
	}
	if (c < 0)
		return -1;
	Ref r;
	r.cell = c;
	r.id = it.id;
	r.idLower = lower(it.id);
	r.type = o->type; r.recType = recTypeOf(r.type.c_str());
	r.obj = o;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.rot[0] = r.rot[1] = 0.0f;
	r.rot[2] = yaw;
	r.count = it.count;
	r.moved = true;
	r.dropped = true;
	r.dropState.condition = it.condition;
	r.dropState.soul = it.soul;
	r.dropState.charge = it.charge;
	// A box to aim at: items are small
	r.hasBox = true;
	r.boxMin[0] = pos[0] - 18.0f; r.boxMin[1] = pos[1] - 18.0f; r.boxMin[2] = pos[2];
	r.boxMax[0] = pos[0] + 18.0f; r.boxMax[1] = pos[1] + 18.0f; r.boxMax[2] = pos[2] + 16.0f;
	memcpy(r.start, pos, sizeof(r.pos));
	memcpy(r.boxStart, r.boxMin, sizeof(r.boxMin));
	memcpy(r.boxStart + 3, r.boxMax, sizeof(r.boxMax));
	int slot = takeSlots(freeRefs, 1);
	if (slot < 0)
	{
		slot = refs.size();
		refs.emplace_back();
	}
	refs[slot] = std::move(r);
	refs[slot].spawnedRef = true;
	refsChanged();
	spawned.push_back(slot);
	rebuildLoadedLists();
	logf("world: dropped %d x %s at %.0f %.0f %.0f", it.count, it.id.c_str(), pos[0], pos[1], pos[2]);
	return slot;
}

// SetScale (OpenMW's World::scaleObject): the reference's size. A person or creature is drawn that much bigger and its
// collision cylinder grows (syncActor), a door's drawing follows (it is drawn with its scale); what the level baked
// into the cell's batches keeps its size (no Bloodmoon script scales one)
void World::scaleObject(int ri, float scale)
{
	Ref& r = refs[ri];
	float now = r.scale > 0.0f ? r.scale : 1.0f;
	scale = fmaxf(0.01f, scale);
	if (scale == now)
		return;
	r.scaleFactor *= scale / now;
	r.scale = scale;
	if (r.actor >= 0)
		syncActor(ri);
	else if (r.doorMesh < 0)
		logf("script: SetScale %s %.2f: drawn at the level's size (baked)", r.id.c_str(), scale);
}

// PlaceItem / PlaceItemCell (OpenMW's placeObject): a person or creature placed as PlaceAtMe places them, an item as one
// dropped there, anything else (Tribunal's dart trap projectile, an activator) as an object of its own with its script
// running from the start (drawn when it has a world visual: particles.WORLD_FX). Nothing for an id the game lacks
int World::placeObject(const std::string& id, int cell, const float pos[3], float yaw)
{
	std::string l = lower(id);
	if (game.spawns.count(l))
		return spawnActor(l, pos, yaw, cell);
	const Object* o = game.object(l);
	if (!o)
	{
		// (Tribunal's Bamz-Amschend fight watcher places in_dwe_all_tribunal, which no file has: OpenMW stops that
		// script with an error, the original game places nothing either)
		logf("world: can't place %s (no such object)", id.c_str());
		return -1;
	}
	switch (o->recType)
	{
	case REC_MISC: case REC_WEAP: case REC_ARMO: case REC_CLOT: case REC_ALCH: case REC_INGR: case REC_BOOK:
	case REC_APPA: case REC_LOCK: case REC_PROB: case REC_REPA: case REC_LIGH:
		return dropItem({ l, 1, false }, pos, yaw, cell);
	default: break;
	}
	int c = cell >= 0 ? cell : current;
	if (cell < 0 && c >= 0 && !cells[c].interior)
	{
		int g = gridCell((int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
		if (g >= 0 && cells[g].live)
			c = g;
	}
	if (c < 0)
		return -1;
	Ref r;
	r.cell = c;
	r.id = o->id.str();
	r.idLower = l;
	r.type = o->type; r.recType = o->recType;
	r.obj = o;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.rot[0] = r.rot[1] = 0.0f;
	r.rot[2] = yaw;
	r.count = 1;
	r.moved = true;
	r.hasBox = true;
	for (int k = 0; k < 3; k++)
	{
		r.boxMin[k] = pos[k] - (k < 2 ? 16.0f : 0.0f);
		r.boxMax[k] = pos[k] + (k < 2 ? 16.0f : 32.0f);
	}
	memcpy(r.start, pos, sizeof(r.pos));
	memcpy(r.boxStart, r.boxMin, sizeof(r.boxMin));
	memcpy(r.boxStart + 3, r.boxMax, sizeof(r.boxMax));
	int slot = takeSlots(freeRefs, 1);
	if (slot < 0)
	{
		slot = refs.size();
		refs.emplace_back();
	}
	refs[slot] = std::move(r);
	refs[slot].spawnedRef = true;
	refsChanged();
	spawned.push_back(slot);
	rebuildLoadedLists();
	startScriptFor(slot);
	logf("world: placed %s at %.0f %.0f %.0f", id.c_str(), pos[0], pos[1], pos[2]);
	return slot;
}

int World::dropItemAt(const InventoryItem& it, const float base[3], float yaw, float dist)
{
	float pos[3] = { base[0] + sinf(yaw) * dist, base[1] + cosf(yaw) * dist, base[2] };
	// Onto the floor there, not through it (from a little above the feet)
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], base[2] + 60.0f, base[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f)
		pos[2] = best;
	return dropItem(it, pos, yaw);
}

void World::refreshStats()
{
	float h = stats.health, m = stats.magicka, f = stats.fatigue;
	recomputeStats();
	stats.health = fminf(h, stats.healthMax);
	stats.magicka = fminf(m, stats.magickaMax);
	stats.fatigue = fminf(f, stats.fatigueMax);
}

const SpellDef* World::enchantmentOf(const InventoryItem& it) const
{
	const Object* o = game.object(it.id);
	if (!o || o->ench.empty())
		return nullptr;
	auto sp = game.spells.find(o->ench);
	return sp != game.spells.end() ? &sp->second : nullptr;
}

float World::chargeOf(const InventoryItem& it) const
{
	const SpellDef* en = enchantmentOf(it);
	return !en ? 0.0f : it.charge < 0.0f ? (float)en->charge : it.charge;
}

void World::relocate(int ri, int cell, const float pos[3], float yaw)
{
	Ref& r = refs[ri];
	if (cell < 0)
		return;
	if (placeOf(ri) != cell && r.actor >= 0)
		detachActor(ri);
	bool newPlace = placeOf(ri) != cell;
	memcpy(r.pos, pos, sizeof(r.pos));
	memcpy(r.home, pos, sizeof(r.home));
	r.fitBox();
	r.rot[2] = yaw;
	r.moved = true;
	r.path.clear();
	if (r.spawnedRef)
		r.cell = cell;
	r.at = cell == r.cell ? -1 : cell;
	auto listed = std::find(spawned.begin(), spawned.end(), ri);
	if (r.at >= 0 && listed == spawned.end())
		spawned.push_back(ri);
	else if (r.at < 0 && !r.spawnedRef && listed != spawned.end())
		spawned.erase(listed);              // back in its own cell: listed there again (not twice)
	if (newPlace && r.actor >= 0 && cells[cell].live)
		attachSpawned(ri);
	else if (r.actor >= 0)
		syncActor(ri);
	rebuildLoadedLists();
	logf("world: %s moved to %s", r.idLower.c_str(), cells[cell].name.c_str());
}

// OpenMW sweeps an actor's collision box down onto the ground (MovementSolver::traceDown): it comes to rest on the
// highest ground anywhere under the box, and the origin sits the box's lift above that. A creature's box is its model's
// "Bounding Box" node (tools/convert/creature.py creature_box); one centred and about square stays axis-aligned (the
// default "actor collision shape type"), any other turns with the creature (Actor::Actor's RotatingBox). How much
// higher than the ground under its feet (where it stands: walking, falling and slopes go by that, as before) its model
// is drawn. Sampled at the box's corners and edges, each counting up to a slope's rise from the feet (45 degrees, at
// most a stair's 48 units): higher is a rock or a wall beside it, which OpenMW's box is pushed away from, not stood on.
// (Standing it there instead made the next step search for floor from that much higher: creatures walked up slopes too
// steep for anyone, and a wandering guar climbed a rock beside it)
float World::actorBoxLift(int ri)
{
	const Ref& r = refs[ri];
	const ActorDef* def = r.actor >= 0 ? &game.actors[r.actor] : nullptr;
	if (!def || !def->hasBox)
		return 0.0f;
	float s = r.scale > 0.0f ? r.scale : 1.0f;
	if (const Actor* a = actorOf(ri))
		s = sqrtf(a->place[0] * a->place[0] + a->place[4] * a->place[4] + a->place[8] * a->place[8]);
	const float* b = def->box;
	float ratio = b[3] > 0.0f ? b[2] / b[3] : 0.0f;
	bool turns = b[0] != 0.0f || b[1] != 0.0f || ratio < 1.0f / 1.1f || ratio > 1.1f;
	float c = turns ? cosf(r.rot[2]) : 1.0f, sn = turns ? sinf(r.rot[2]) : 0.0f;
	const float kMaxRise = 48.0f;
	float ground = r.pos[2], best = ground;
	for (int i = -1; i <= 1; i++)
		for (int j = -1; j <= 1; j++)
		{
			float lx = (b[0] + i * b[2]) * s, ly = (b[1] + j * b[3]) * s, z;
			float px = r.pos[0] + c * lx + sn * ly, py = r.pos[1] - sn * lx + c * ly;
			float rise = fminf(kMaxRise, sqrtf(lx * lx + ly * ly));
			for (LoadedCell* l : loaded)
				if (collisionFloor(l->cell.collision, px, py, ground + kMaxRise, ground - 200.0f, &z))
					best = fmaxf(best, fminf(z, ground + rise));
		}
	return best - ground + b[4] * s;
}

void World::snapToFloor(float pos[3])
{
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], pos[2] + 20.0f, pos[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f && best < pos[2])
		pos[2] = best;
}

// OpenMW's PlaceAtMe: every one goes to the same spot, the first free direction (asked, then the one a
// quarter turn back, half a turn, the other side) with nothing in the way; items need no such check
void World::placeAt(const std::string& id, int count, float dist, int dir, const float base[3], float yaw)
{
	if (count <= 0 || dir < 0 || dir > 3)
		return;
	const bool actor = game.spawns.count(lower(id)) != 0;
	const Object* item = actor ? nullptr : game.object(lower(id));
	if (!actor && (!item || (item->recType != REC_MISC && item->recType != REC_WEAP && item->recType != REC_ARMO && item->recType != REC_CLOT
		&& item->recType != REC_ALCH && item->recType != REC_INGR && item->recType != REC_BOOK && item->recType != REC_APPA
		&& item->recType != REC_LOCK && item->recType != REC_PROB && item->recType != REC_REPA && item->recType != REC_LIGH)))
	{
		monitorOnce(("placeat:" + id).c_str(), "PlaceAtMe of %s: can't place that kind of record", id.c_str());
		return;
	}
	float pos[3] = { base[0], base[1], base[2] };
	for (int t = 0; t < 4; t++)
	{
		// the asked direction, then +3, +2, +1 (mod 4)
		int d = (dir + (t == 0 ? 0 : 4 - t)) % 4;
		float a = yaw + (d == 1 ? 3.14159265f : d == 2 ? -1.5707963f : d == 3 ? 1.5707963f : 0.0f);
		pos[0] = base[0] + sinf(a) * dist;
		pos[1] = base[1] + cosf(a) * dist;
		pos[2] = base[2];
		if (!actor)
			break;
		float from[3] = { pos[0], pos[1], pos[2] + 30.0f }, to[3] = { base[0], base[1], base[2] + 20.0f };
		if (lineOfSight(from, to, false))
			break;
	}
	// Onto the floor there
	float best = -1e9f;
	for (LoadedCell* l : loaded)
	{
		float z;
		if (collisionFloor(l->cell.collision, pos[0], pos[1], base[2] + 150.0f, base[2] - 600.0f, &z) && z > best)
			best = z;
	}
	if (best > -1e9f)
		pos[2] = best;
	for (int k = 0; k < count; k++)
	{
		if (actor)
			spawnActor(id, pos, yaw);       // facing the way the one it was placed from does
		else
			dropItem({ lower(id), 1, false }, pos, yaw);
	}
}

// The game data a session leaves behind (World::keepGame) for the next one from the same folder: loading a
// save needn't parse 32 MB of it again (16 s), nor lay a second copy over a heap the first one fragmented
static GameData* s_keptGame = nullptr;
static std::string s_keptDir;

void World::keepGame()
{
	delete s_keptGame;
	// what this session made (potions, enchantments, spells: "mw3ds_..." ids) goes; a save brings its own
	for (auto it = game.objects.begin(); it != game.objects.end();)
		it = it->first.compare(0, 6, "mw3ds_") == 0 ? game.objects.erase(it) : std::next(it);
	for (auto it = game.spells.begin(); it != game.spells.end();)
		it = it->first.compare(0, 6, "mw3ds_") == 0 ? game.spells.erase(it) : std::next(it);
	s_keptGame = new GameData(std::move(game));
	g_gameGeneration++;
	s_keptDir = dataDir ? dataDir : "";
}

bool World::load(const char* dir, const std::string& startCell)
{
	u64 loadStart = osGetTime();
	dataDir = dir;
	char path[256];
	snprintf(path, sizeof(path), "%s/game.json", dir);
	if (s_keptGame && s_keptDir == dir)
	{
		game = std::move(*s_keptGame);
		delete s_keptGame;
		s_keptGame = nullptr;
		logf("world: game data kept from the last session");
	}
	else
	{
		logf("world: reading %s", path);
		if (!gameLoad(game, path))
			return false;
	}
	distantLoad(dir);
	if (g_sharedSkeletons.empty())
	{
		snprintf(path, sizeof(path), "%s/cells/skeletons.skl", dir);
		skeletonsLoad(path);
	}

	// Older data has only the starting cell
	if (game.cells.empty())
		game.cells.push_back({ game.cellName, cellFileName(game.cellName), true });
	for (auto& c : game.cells)
	{
		LevelCell lc;
		lc.name = c.name;
		lc.file = c.file;
		lc.interior = c.interior;
		lc.gx = c.gx;
		lc.gy = c.gy;
		lc.sea = c.sea;
		lc.noSleep = c.noSleep;
		lc.sky = c.sky;
		lc.region = c.region;
		if (!c.interior)
			gridIndex[((long long)c.gx << 32) ^ (unsigned)c.gy] = cells.size();
		cells.push_back(lc);
	}
	// Cells' objects are read when a cell is first needed (ensureRefs), not all at start
	for (auto& g : game.globals)
		globals[g.first] = g.second.value;
	// DaysPassed: vanilla Morrowind.esm doesn't define it, Tribunal and Bloodmoon start it at 1 (OpenMW's World adds
	// it so too): the day count from the first day, not one short
	globals.emplace("dayspassed", 1.0f);
	recomputeStats();
	logf("world: %d cells, game data in %llu ms", (int)cells.size(), osGetTime() - loadStart);
	int start = startCell.empty() ? -1 : cellIndex(startCell);
	if (!startCell.empty() && start < 0)
		logf("world: start cell %s is not in the level", startCell.c_str());
	if (start < 0)
		start = cellIndex(game.cellName);
	start = start >= 0 ? start : 0;
	// Outdoors the streaming follows the player: start them in the middle of their cell
	if (!cells[start].interior)
	{
		player.feet[0] = (cells[start].gx + 0.5f) * 8192.0f;
		player.feet[1] = (cells[start].gy + 0.5f) * 8192.0f;
	}
	return enterCell(start);
}

// Like findRef, but also reads the objects of cells the level's index says hold that id
bool World::deferEnable(const std::string& id, bool on)
{
	if (findRef(id) >= 0)
		return false;
	std::string l = lower(id);
	auto it = game.refCells.find(l);
	if (it == game.refCells.end())
		return false;
	for (int c : it->second)
		if (c >= 0 && c < (int)cells.size() && !cells[c].refsLoaded)
		{
			lazyEnabled[l] = on;
			return true;
		}
	return false;
}

int World::findRefAnywhere(const std::string& id)
{
	int r = findRef(id);
	if (r >= 0)
		return r;
	auto it = game.refCells.find(lower(id));
	if (it == game.refCells.end())
		return -1;
	bool read = false;
	for (int c : it->second)
		if (c >= 0 && c < (int)cells.size() && !cells[c].refsLoaded)
			read |= ensureRefs(c);
	return read ? findRef(id) : -1;
}

int World::findRef(const std::string& id) const
{
	// The first active reference with that id, else the first at all. By an index of the interned ids (Atoms are one copy
	// per text): a scan of every reference was milliseconds a call on an Old 3DS, and AI packages and scripts call it a frame
	// (ids are lowercase nearly always: a lowered copy only for one that isn't)
	bool upper = false;
	for (char ch : id)
		upper |= ch >= 'A' && ch <= 'Z';
	std::string l = upper ? lower(id) : std::string();
	const std::string& low = upper ? l : id;
	const char* key = low.empty() ? nullptr : Atom::existing(low.data(), low.size());
	if (!key)
		return -1;
	for (int pass = 0; pass < 2; pass++)
	{
		if (refIndexStale)
		{
			refIndex.clear();
			refIndex.reserve(refs.size());
			for (size_t i = 0; i < refs.size(); i++)
				if (!refs[i].idLower.empty())
					refIndex.push_back({ refs[i].idLower.c_str(), (int)i });
			std::sort(refIndex.begin(), refIndex.end());
			refIndexStale = false;
		}
		auto it = std::lower_bound(refIndex.begin(), refIndex.end(), std::make_pair(key, -1));
		int other = -1;
		bool stale = false;
		for (; it != refIndex.end() && it->first == key; ++it)
		{
			int i = it->second;
			if (i >= (int)refs.size() || refs[i].idLower.c_str() != key)
			{
				stale = true;              // (a change nobody reported: rebuilt once)
				break;
			}
			if (active(i))
				return i;
			if (other < 0)
				other = i;
		}
		if (!stale)
			return other;
		refIndexStale = true;
	}
	return -1;
}

int World::findActorRef(int actor) const
{
	for (size_t i = 0; i < refs.size(); i++)
		if (refs[i].actor == actor)
			return i;
	return -1;
}

// Hiding a reference turns its triangles into degenerate ones in the shared index buffers
void hideRanges(World& w, Ref& ref)
{
	// Cells that aren't loaded hide their references when they load
	LoadedCell* live = w.cells[ref.cell].live;
	if (live && ref.colCount)
		collisionHideTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
	if (!ref.savedIndices.empty() || !live)
		return;
	for (auto& r : ref.ranges)
	{
		if (r.batch < 0 || r.batch >= (int)live->cell.batches.size())
			continue;
		CellBatch& b = live->cell.batches[r.batch];
		if (!b.indices || r.first + r.count > b.numIndices)
			continue;
		u16* idx = b.indices + r.first;
		ref.savedIndices.insert(ref.savedIndices.end(), idx, idx + r.count);
		for (u32 i = 0; i < r.count; i++)
			idx[i] = idx[0];
		GSPGPU_FlushDataCache(idx, r.count * 2);
	}
	if (ref.ranges.empty())
		ref.savedIndices.push_back(0);   // marker: hidden
}

// A visible object whose collision went with its death group (World::collisionGone): only its collision hidden
void hideCollision(World& w, Ref& ref)
{
	LoadedCell* live = w.cells[ref.cell].live;
	if (live && ref.colCount && ref.savedCol.empty())
		collisionHideTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
}

void World::loseCollision(int ri)
{
	if (ri < 0 || ri >= (int)refs.size() || !refs[ri].colCount)
		return;
	refs[ri].colGone = true;
	hideCollision(*this, refs[ri]);
}

void World::regainCollision(int ri)
{
	if (ri < 0 || ri >= (int)refs.size() || !refs[ri].colGone)
		return;
	Ref& ref = refs[ri];
	ref.colGone = false;
	LoadedCell* live = cells[ref.cell].live;
	if (live && ref.visible())
		collisionShowTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
}

void showRanges(World& w, Ref& ref)
{
	LoadedCell* live = w.cells[ref.cell].live;
	if (!live)
		return;
	if (ref.colCount && !ref.colGone)
		collisionShowTris(live->cell.collision, ref.colFirst, ref.colCount, ref.savedCol);
	size_t pos = 0;
	for (auto& r : ref.ranges)
	{
		if (pos + r.count > ref.savedIndices.size())
			break;
		if (r.batch < 0 || r.batch >= (int)live->cell.batches.size())
			continue;
		const CellBatch& b = live->cell.batches[r.batch];
		if (!b.indices || r.first + r.count > b.numIndices)
			continue;
		u16* idx = b.indices + r.first;
		memcpy(idx, &ref.savedIndices[pos], r.count * 2);
		GSPGPU_FlushDataCache(idx, r.count * 2);
		pos += r.count;
	}
	ref.savedIndices.clear();
}

void World::setEnabled(int ri, bool on)
{
	if (ri < 0)
		return;
	Ref& ref = refs[ri];
	ref.enabled = on;
	if (ref.visible())
		showRanges(*this, ref);
	else
		hideRanges(*this, ref);
}

float World::distanceToPlayer(int ri) const
{
	const Ref& r = refs[ri];
	if (!active(ri) || (cells[placeOf(ri)].interior && placeOf(ri) != current))
		return 100000.0f;
	float dx = r.pos[0] - player.feet[0], dy = r.pos[1] - player.feet[1], dz = r.pos[2] - player.feet[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// A moved / turned object's own shape as boxes. OpenMW keeps an object's collision shape with it as a script moves or
// turns it (PhysicsSystem::updatePosition / updateRotation); here the converter keeps such an object out of the baked
// collision and only its drawn mesh is loaded, so the shape comes from that mesh, placed as the level placed it. Its
// triangles are laid on a grid in XY (32-unit squares, wider for a huge object), each square taking the height and the
// reach of the triangles over it, and squares in a row with the same heights join into one box. A ring wall stays a ring
// with its inside free (Tribunal's weather machine in Skybreak Gallery: the player lands inside it), a portcullis its
// bars. Worked out once per object (World::update keeps them, moved with the object), not every frame
static void moverBoxes(const Ref& ref, const DoorMesh& mesh, std::vector<CollisionBox>& out)
{
	// the drawn transform (RenderingManager): translate * X(-x) * Y(-y) * Z(-z) * scale, at the placed spot and angles
	float cx = cosf(-ref.startRot[0]), sx = sinf(-ref.startRot[0]);
	float cy = cosf(-ref.startRot[1]), sy = sinf(-ref.startRot[1]);
	float cz = cosf(-ref.start[3]), sz = sinf(-ref.start[3]);
	auto place = [&](const float* v, float* o)
	{
		float x = v[0] * ref.scale, y = v[1] * ref.scale, z = v[2] * ref.scale;
		float x1 = cz * x - sz * y, y1 = sz * x + cz * y;
		float x2 = cy * x1 + sy * z, z2 = -sy * x1 + cy * z;
		o[0] = ref.start[0] + x2;
		o[1] = ref.start[1] + cx * y1 - sx * z2;
		o[2] = ref.start[2] + sx * y1 + cx * z2;
	};
	std::vector<float> tris;     // 9 floats a triangle, in the world
	float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f };
	for (const CellBatch& b : mesh.batches)
	{
		if (!b.verts || !b.indices)
			continue;
		const u8* base = (const u8*)b.verts;
		for (u32 t = 0; t + 2 < b.numIndices; t += 3)
		{
			float tri[9];
			bool ok = true;
			for (int k = 0; k < 3 && ok; k++)
			{
				u32 vi = b.indices[t + k];
				ok = vi < b.numVerts;
				if (!ok)
					break;
				const u8* v = base + vi * b.stride;
				float local[3];
				if (b.stride == CELL_VERTEX_SIZE_PACKED)
					for (int a = 0; a < 3; a++)
						local[a] = b.posOffset[a] + ((const s16*)v)[a] * b.posScale[a];
				else
					memcpy(local, v, sizeof(local));
				place(local, tri + k * 3);
				for (int a = 0; a < 2; a++)
				{
					lo[a] = fminf(lo[a], tri[k * 3 + a]);
					hi[a] = fmaxf(hi[a], tri[k * 3 + a]);
				}
			}
			if (ok)
				tris.insert(tris.end(), tri, tri + 9);
		}
	}
	if (tris.empty())
		return;
	float cs = 32.0f;
	while (((hi[0] - lo[0]) / cs + 1.0f) * ((hi[1] - lo[1]) / cs + 1.0f) > 4096.0f)
		cs *= 2.0f;
	int nx = (int)((hi[0] - lo[0]) / cs) + 1, ny = (int)((hi[1] - lo[1]) / cs) + 1;
	// each square: the height range of the triangles over it, and their reach inside it (a thin wall stays thin)
	struct Square { float z0 = 1e30f, z1 = -1e30f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; };
	std::vector<Square> sq((size_t)nx * ny);
	for (size_t t = 0; t + 8 < tris.size(); t += 9)
	{
		const float* p = &tris[t];
		float tx0 = fminf(p[0], fminf(p[3], p[6])), tx1 = fmaxf(p[0], fmaxf(p[3], p[6]));
		float ty0 = fminf(p[1], fminf(p[4], p[7])), ty1 = fmaxf(p[1], fmaxf(p[4], p[7]));
		float tz0 = fminf(p[2], fminf(p[5], p[8])), tz1 = fmaxf(p[2], fmaxf(p[5], p[8]));
		int ix0 = (int)((tx0 - lo[0]) / cs), ix1 = std::min(nx - 1, (int)((tx1 - lo[0]) / cs));
		int iy0 = (int)((ty0 - lo[1]) / cs), iy1 = std::min(ny - 1, (int)((ty1 - lo[1]) / cs));
		for (int iy = iy0; iy <= iy1; iy++)
			for (int ix = ix0; ix <= ix1; ix++)
			{
				// the square and the triangle (seen from above) overlap unless one of the triangle's edges parts them
				float rx0 = lo[0] + ix * cs, ry0 = lo[1] + iy * cs, rx1 = rx0 + cs, ry1 = ry0 + cs;
				bool apart = false;
				for (int e = 0; e < 3 && !apart; e++)
				{
					const float* a = p + e * 3;
					const float* b = p + ((e + 1) % 3) * 3;
					float nxv = a[1] - b[1], nyv = b[0] - a[0];
					float t0 = 1e30f, t1 = -1e30f, r0 = 1e30f, r1 = -1e30f;
					for (int k = 0; k < 3; k++)
					{
						float d = nxv * p[k * 3] + nyv * p[k * 3 + 1];
						t0 = fminf(t0, d);
						t1 = fmaxf(t1, d);
					}
					for (int k = 0; k < 4; k++)
					{
						float d = nxv * (k & 1 ? rx1 : rx0) + nyv * (k & 2 ? ry1 : ry0);
						r0 = fminf(r0, d);
						r1 = fmaxf(r1, d);
					}
					apart = r1 < t0 || r0 > t1;
				}
				if (apart)
					continue;
				Square& s = sq[(size_t)iy * nx + ix];
				s.z0 = fminf(s.z0, tz0);
				s.z1 = fmaxf(s.z1, tz1);
				s.x0 = fminf(s.x0, fmaxf(tx0, rx0));
				s.x1 = fmaxf(s.x1, fminf(tx1, rx1));
				s.y0 = fminf(s.y0, fmaxf(ty0, ry0));
				s.y1 = fmaxf(s.y1, fminf(ty1, ry1));
			}
	}
	for (int iy = 0; iy < ny; iy++)
		for (int ix = 0; ix < nx;)
		{
			Square run = sq[(size_t)iy * nx + ix++];
			if (run.z0 > run.z1)
				continue;
			for (; ix < nx; ix++)
			{
				const Square& s = sq[(size_t)iy * nx + ix];
				if (s.z0 > s.z1 || fabsf(s.z0 - run.z0) > 8.0f || fabsf(s.z1 - run.z1) > 8.0f)
					break;
				run.z0 = fminf(run.z0, s.z0);
				run.z1 = fmaxf(run.z1, s.z1);
				run.x0 = fminf(run.x0, s.x0);
				run.x1 = fmaxf(run.x1, s.x1);
				run.y0 = fminf(run.y0, s.y0);
				run.y1 = fmaxf(run.y1, s.y1);
			}
			CollisionBox box;
			box.min[0] = run.x0; box.min[1] = run.y0; box.min[2] = run.z0;
			box.max[0] = run.x1; box.max[1] = run.y1; box.max[2] = run.z1;
			out.push_back(box);
		}
}

void World::update(float dt)
{
	time += dt;
	gameHour += dt / 3600.0f * timescale();
	syncTime();
	updateWeather(dt);
	// Pose only actors near enough to be drawn (the towns have many more than the office)
	// and pose the far ones less often (their re-skinning is rarer too, mwrender/renderingmanager.cpp)
	float reach = here().fogEnd > 0.0f && !(here().flags & CELL_INTERIOR) ? fminf(here().fogEnd * drawScale, kActorViewDistance) + 200.0f : 4200.0f;
	// (no farther than the console draws them: an Old 3DS posed people to 6200 and drew them to 4000)
	if (g_profile.actorViewLimit > 0.0f)
		reach = fminf(reach, g_profile.actorViewLimit + 200.0f);
	frame++;
	// Both consoles pose people by time (ConsoleProfile::poseHz), not every so many frames: near, middle and far rates,
	// the near one only for the few most in view (ahead of the eye, the nearer and the more central the higher); the
	// rest near take the middle's rate, and those out of view the far one. (A profile without rates poses every few
	// frames: actorSkinEvery / actorPoseEvery)
	bool byTime = g_profile.poseHz[0] > 0.0f;
	if (byTime)
	{
		static std::vector<std::pair<float, Actor*>> inView;
		inView.clear();
		float fwd[2] = { sinf(player.yaw), cosf(player.yaw) };
		for (LoadedCell* l : loaded)
			for (Actor& a : l->actors.actors)
			{
				float dx = a.place[3] - player.feet[0], dy = a.place[7] - player.feet[1];
				float d2 = dx * dx + dy * dy, d = sqrtf(d2);
				float ahead = d > 1.0f ? (dx * fwd[0] + dy * fwd[1]) / d : 1.0f;    // cosine off the eye's line
				bool seen = ahead > 0.6f || (d < 150.0f && ahead > 0.0f);    // (or right beside the eye)
				a.poseHz = !seen || d2 >= 3000.0f * 3000.0f ? g_profile.poseHz[2] : g_profile.poseHz[1];
				if (seen && d2 < 1600.0f * 1600.0f)
					inView.push_back({ ahead / fmaxf(d, 100.0f), &a });
			}
		size_t full = std::min(inView.size(), (size_t)g_profile.poseFull);
		std::partial_sort(inView.begin(), inView.begin() + full, inView.end(),
			[](const std::pair<float, Actor*>& x, const std::pair<float, Actor*>& y) { return x.first > y.first; });
		for (size_t i = 0; i < full; i++)
			inView[i].second->poseHz = g_profile.poseHz[0];
	}
	for (LoadedCell* l : loaded)
	{
		for (size_t k = 0; k < l->actors.actors.size(); k++)
		{
			Actor& a = l->actors.actors[k];
			float dx = a.place[3] - player.feet[0], dy = a.place[7] - player.feet[1];
			float d2 = dx * dx + dy * dy;
			// A body whose fall stops short of its end, or that plays something other than a death: once, with what it
			// was doing (a person stayed frozen upright on a New 3DS in a fight in Shishi, never reproduced: the device's
			// log should say what held it). Not while the world stands still under a menu, nor past the reach posed (those
			// wait on purpose for the player to come near)
			if (a.ref >= 0 && refs[a.ref].dead && dt > 0.0f && !a.frozen && refs[a.ref].visible() && d2 < reach * reach)
			{
				const Skeleton& sk = actorSkeleton(l->actors, a.skeleton);
				const AnimGroup* g = a.group >= 0 && a.group < (int)sk.groups.size() ? &sk.groups[a.group] : nullptr;
				const char* n = g ? g->name : "";
				bool death = strncasecmp(n + (strncasecmp(n, "Swim", 4) == 0 ? 4 : 0), "Death", 5) == 0;
				if (g && (!death || a.time < g->stop - 0.05f))
				{
					a.fallFor += dt;
					if (a.fallFor > 3.0f * fmaxf(1.0f, g->stop - g->start))
						monitorOnce(("fallstuck:" + refs[a.ref].id).c_str(),
							"%s's death stuck: group %.*s (wanted %s), time %.2f of %.2f..%.2f, mode %d, poses %u, owed %.2f, "
							"%.0f away", refs[a.ref].id.c_str(), (int)sizeof(g->name), n, refs[a.ref].deathGroup, a.time, g->start,
							g->stop, (int)a.mode, a.poses, a.pendingDt, sqrtf(d2));
				}
				else
					a.fallFor = 0.0f;
			}
			if (a.ref < 0 || !refs[a.ref].visible() || d2 >= reach * reach)
				continue;
			// (paralyzed, alive: it stands frozen mid-move, its animation's time held: OpenMW's skipAnim)
			if (!refs[a.ref].dead && !refs[a.ref].effects.empty() && actorEffect(a.ref, 45) > 0.0f)
				continue;
			a.pendingDt += dt;
			if (byTime)
			{
				// (the time left over carries on, so 20 a second at 30 fps is every frame and a half; the first due time
				// is staggered, so a crowd doesn't all come due in the same frame)
				float interval = 1.0f / a.poseHz;
				if (a.poseDue < 0.0f)
					a.poseDue = (k % 4) * 0.25f * interval;
				a.poseDue += dt;
				if (a.poseDue + 1e-4f >= interval)
				{
					a.poseDue = fminf(a.poseDue - interval, interval);
					actorAnimate(l->actors, a, a.pendingDt);
					a.pendingDt = 0.0f;
					a.poses++;
				}
				continue;
			}
			// One with skins shows a pose only when the renderer re-skins it (its rigid parts ride that pose too), so it is
			// posed on the re-skin's cadence, and the renderer re-skins each new pose (the skipped frames' time is owed)
			u32 every = a.skinned ? actorSkinEvery(d2) : actorPoseEvery(d2);
			if ((frame + k) % every == 0)
			{
				actorAnimate(l->actors, a, a.pendingDt);
				a.pendingDt = 0.0f;
				a.poses++;
			}
		}
		l->cell.blockers.clear();
	}
	for (int i : loadedDoors)
	{
		Ref& ref = refs[i];
		Cell& cell = cells[ref.cell].live->cell;
		if (ref.recType != REC_DOOR)
		{
			// Moved / turned by scripts (gates, Dwemer doors, platforms, a ring wall): its own shape as boxes (moverBoxes),
			// gone where it went, blocking while it's turned less than ~17 degrees from where it started (the turn
			// itself is left out, as for its box before). A ring wall used to be one box of all of it, holding a player
			// who stood inside it (Skybreak Gallery)
			float turned = fmaxf(fabsf(angleDelta(ref.rot[2], ref.start[3])),
				fmaxf(fabsf(angleDelta(ref.rot[0], ref.startRot[0])), fabsf(angleDelta(ref.rot[1], ref.startRot[1]))));
			ref.fitBox();
			if (ref.visible() && turned < 0.3f && ref.doorMesh < (int)cell.doors.size() && cell.doors[ref.doorMesh].ref == i)
			{
				MoverShape& ms = moverShapes[i];
				if (ms.cell != ref.cell || ms.mesh != ref.doorMesh || ms.scale != ref.scale)
				{
					ms.cell = ref.cell;
					ms.mesh = ref.doorMesh;
					ms.scale = ref.scale;
					ms.boxes.clear();
					moverBoxes(ref, cell.doors[ref.doorMesh], ms.boxes);
				}
				float d[3] = { ref.pos[0] - ref.start[0], ref.pos[1] - ref.start[1], ref.pos[2] - ref.start[2] };
				for (const CollisionBox& b : ms.boxes)
				{
					CollisionBox box = b;
					for (int k = 0; k < 3; k++)
					{
						box.min[k] += d[k];
						box.max[k] += d[k];
					}
					cell.blockers.push_back(box);
				}
			}
			continue;
		}
		{
			float step = 1.6f * dt;    // ~1 s to swing open
			if (ref.doorAngle < ref.doorTarget) ref.doorAngle = fminf(ref.doorTarget, ref.doorAngle + step);
			if (ref.doorAngle > ref.doorTarget) ref.doorAngle = fmaxf(ref.doorTarget, ref.doorAngle - step);
			if (ref.visible() && ref.hasBox && fabsf(ref.doorAngle) < 0.3f)
			{
				CollisionBox box;
				memcpy(box.min, ref.boxMin, sizeof(box.min));
				memcpy(box.max, ref.boxMax, sizeof(box.max));
				box.door = i;
				cell.blockers.push_back(box);
			}
			// Open and at rest, the leaf blocks where it is drawn (OpenMW's door collision turns with the door): the closed
			// box's corners turned about the door's origin as the renderer turns it. It walked through before. Not while it
			// swings, nor round someone standing where it came to rest (they walk out of it)
			else if (ref.visible() && ref.hasBox && ref.doorAngle == ref.doorTarget)
			{
				CollisionBox box;
				for (int k = 0; k < 3; k++)
				{
					box.min[k] = 1e9f;
					box.max[k] = -1e9f;
				}
				for (int c = 0; c < 8; c++)
				{
					float rel[3] = { ((c & 1) ? ref.boxMax[0] : ref.boxMin[0]) - ref.pos[0],
						((c & 2) ? ref.boxMax[1] : ref.boxMin[1]) - ref.pos[1], ((c & 4) ? ref.boxMax[2] : ref.boxMin[2]) - ref.pos[2] };
					float out[3];
					doorSwing(ref, -ref.doorAngle, rel, out);
					for (int k = 0; k < 3; k++)
					{
						box.min[k] = fminf(box.min[k], out[k] + ref.pos[k]);
						box.max[k] = fmaxf(box.max[k], out[k] + ref.pos[k]);
					}
				}
				const float* f = player.feet;
				bool inside = f[0] > box.min[0] - 22.0f && f[0] < box.max[0] + 22.0f && f[1] > box.min[1] - 22.0f
					&& f[1] < box.max[1] + 22.0f && f[2] + 128.0f > box.min[2] && f[2] < box.max[2];
				if (!inside)
					cell.blockers.push_back(box);
			}
		}
	}
	for (int i : loadedActors)
	{
		if (refs[i].helloTimer > 0.0f)
			refs[i].helloTimer -= dt;
		refs[i].fitBox();          // however it moved (walking, scripts, a cell's state, a save)
	}
}

static bool isActivatable(const Ref& r);

// OpenMW keeps, for each actor, the object its ground trace last hit (MovementSolver; Actor::getStandingOnPtr), and
// GetStandingPC / GetStandingActor ask whether that is this object. An object scripts move has no baked collision here,
// only its boxes (moverBoxes): feet over one of them, from its bottom to a little over its top, are on it. A floor stone
// sunk into a sloping floor (Tribunal's act_plague_rock3 in Gedna Relvel's tomb: drawn top 50 over the floor that holds
// the feet) has the feet inside its shape, where OpenMW's body rests on its top. Any other object: feet over its box,
// at its top
bool World::feetOnObject(int ri, const float* f) const
{
	const Ref& r = refs[ri];
	if (!r.hasBox || !r.visible())
		return false;
	auto ms = moverShapes.find(ri);
	if (ms != moverShapes.end() && !ms->second.boxes.empty())
	{
		float d[3] = { r.pos[0] - r.start[0], r.pos[1] - r.start[1], r.pos[2] - r.start[2] };
		for (const CollisionBox& b : ms->second.boxes)
			if (f[0] >= b.min[0] + d[0] && f[0] <= b.max[0] + d[0] && f[1] >= b.min[1] + d[1] && f[1] <= b.max[1] + d[1]
				&& f[2] >= b.min[2] + d[2] - 2.0f && f[2] <= b.max[2] + d[2] + 30.0f)
				return true;
		return false;
	}
	return f[0] >= r.boxMin[0] && f[0] <= r.boxMax[0] && f[1] >= r.boxMin[1] && f[1] <= r.boxMax[1]
		&& fabsf(f[2] - r.boxMax[2]) < 30.0f;
}

float angleDelta(float a, float b)
{
	float d = fmodf(a - b, 6.2831853f);
	if (d > 3.14159265f) d -= 6.2831853f;
	if (d < -3.14159265f) d += 6.2831853f;
	return d;
}

void World::rebuildLoadedLists()
{
	loadedActors.clear();
	loadedDoors.clear();
	loadedPickables.clear();
	for (auto it = moverShapes.begin(); it != moverShapes.end();)
	{
		bool keep = it->first < (int)refs.size() && refs[it->first].cell >= 0 && cells[refs[it->first].cell].live;
		it = keep ? std::next(it) : moverShapes.erase(it);
	}
	forLoadedRefs([&](int i) {
		const Ref& r = refs[i];
		if (r.actor >= 0)
			loadedActors.push_back(i);
		if (r.doorMesh >= 0)
			loadedDoors.push_back(i);
		if (r.hasBox && r.obj)
			loadedPickables.push_back(i);        // whether it's usable right now is checked when picking
	});
}

static bool isActivatable(const Ref& r)
{
	if (!r.visible() || !r.hasBox || !r.obj)
		return false;
	const Atom& t = r.type;
	if (t == "ACTI" && r.obj->name.empty())
		return false;                      // nameless activators are script triggers ("chargen stuff room")
	if (t == "NPC_" || t == "DOOR" || t == "CONT" || t == "ACTI")
		return true;
	// a creature, alive or dead, is something to point at (OpenMW's focus): the body to search, or one with words of its own
	// to talk to (Krazzt at the Puzzle Canal, Creeper); one with nothing to say does nothing (Session::activate)
	if (t == "CREA")
		return true;
	if (t == "LIGH")
		return (r.obj->flags & 2) != 0;    // only carryable lights
	return isItemType(t);
}

bool actorAimable(const World& w, int ri)
{
	const Ref& r = w.refs[ri];
	if (!isActivatable(r))
		return false;
	float mid = fminf(64.0f, (r.boxMax[2] - r.boxMin[2]) * 0.5f);
	float at[3] = { r.pos[0], r.pos[1], r.pos[2] + mid };
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float d = sqrtf(dx * dx + dy * dy);
	float dir[3] = { d > 1.0f ? dx / d : 0.0f, d > 1.0f ? dy / d : 1.0f, 0.0f };
	// its own box only (what stands in front of it is another matter): the slab test, from 100 units off
	float tmin = 0.0f, tmax = 200.0f;
	for (int k = 0; k < 3; k++)
	{
		float e = at[k] - dir[k] * 100.0f;
		if (fabsf(dir[k]) < 1e-6f)
		{
			if (e < r.boxMin[k] || e > r.boxMax[k])
				return false;
			continue;
		}
		float t1 = (r.boxMin[k] - e) / dir[k], t2 = (r.boxMax[k] - e) / dir[k];
		tmin = fmaxf(tmin, fminf(t1, t2));
		tmax = fminf(tmax, fmaxf(t1, t2));
	}
	return tmin <= tmax;
}

// The ray's entry distance into a reference's world bounds (slightly padded for thin items), or -1 for a miss
// (top: the box cut off there, a body lying in a standing one)
static float rayBoxEntry(const Ref& r, const float eye[3], const float dir[3], float maxT, float top = 1e9f)
{
	float tmin = 0.0f, tmax = maxT;
	for (int k = 0; k < 3; k++)
	{
		float lo = r.boxMin[k] - 2.0f, hi = k == 2 ? fminf(r.boxMax[k], top) + 2.0f : r.boxMax[k] + 2.0f;
		if (fabsf(dir[k]) < 1e-6f)
		{
			if (eye[k] < lo || eye[k] > hi)
				return -1.0f;
			continue;
		}
		float t1 = (lo - eye[k]) / dir[k], t2 = (hi - eye[k]) / dir[k];
		if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
		tmin = fmaxf(tmin, t1);
		tmax = fminf(tmax, t2);
		if (tmin > tmax)
			return -1.0f;
	}
	return tmin;
}

// Whether the eye is outside a reference's box but within the reach of it
static bool boxInReach(const Ref& r, const float eye[3], float reach)
{
	float d2 = 0.0f;
	for (int k = 0; k < 3; k++)
	{
		float c = fmaxf(r.boxMin[k], fminf(eye[k], r.boxMax[k])) - eye[k];
		d2 += c * c;
	}
	// An eye inside the box is inside the thing (the prison ship's model is one activator 1800 units wide, and its
	// top deck is inside it): nothing to point at from in there
	return d2 <= reach * reach && d2 != 0.0f;
}

// Whether the eye is inside a reference's box
static bool eyeInsideBox(const Ref& r, const float eye[3])
{
	for (int k = 0; k < 3; k++)
		if (eye[k] < r.boxMin[k] || eye[k] > r.boxMax[k])
			return false;
	return true;
}

// The ray's entry into the middle of a reference's box (the inner 40% of it across, all of it in height), or -1
static float rayCoreEntry(const Ref& r, const float eye[3], const float dir[3], float maxT)
{
	float tmin = 0.0f, tmax = maxT;
	for (int k = 0; k < 3; k++)
	{
		float lo = r.boxMin[k], hi = r.boxMax[k];
		if (k < 2)
		{
			float mid = (lo + hi) * 0.5f, half = (hi - lo) * 0.2f;
			lo = mid - half;
			hi = mid + half;
		}
		if (fabsf(dir[k]) < 1e-6f)
		{
			if (eye[k] < lo || eye[k] > hi)
				return -1.0f;
			continue;
		}
		float t1 = (lo - eye[k]) / dir[k], t2 = (hi - eye[k]) / dir[k];
		if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
		tmin = fmaxf(tmin, t1);
		tmax = fminf(tmax, t2);
		if (tmin > tmax)
			return -1.0f;
	}
	return tmin;
}

// The renderer places a door by Rx(-rot x) Ry(-rot y) Rz(-rot z) and swings it by Rz(-doorAngle) after those, so the swing
// axis is the door's own up turned by its x and y rotation: upright for most doors, flipped for one laid upside down
// (Nchurdamz's Dwemer doors, trapdoor lids) and level for a slave pod's lid. Rodrigues' turn about it
void doorSwing(const Ref& r, float angle, const float in[3], float out[3])
{
	float k[3] = { -sinf(r.rot[1]), sinf(r.rot[0]) * cosf(r.rot[1]), cosf(r.rot[0]) * cosf(r.rot[1]) };
	float c = cosf(angle), s = sinf(angle);
	float kv = k[0] * in[0] + k[1] * in[1] + k[2] * in[2];
	float kx[3] = { k[1] * in[2] - k[2] * in[1], k[2] * in[0] - k[0] * in[2], k[0] * in[1] - k[1] * in[0] };
	for (int i = 0; i < 3; i++)
		out[i] = in[i] * c + kx[i] * s + k[i] * kv * (1.0f - c);
}

int worldPick(const World& w, const float eye[3], const float dir[3], float reach)
{
	int best = -1;
	float bestT = reach;
	int shut = -1;           // the nearest shut door on the ray
	float shutT = reach;
	for (int i : w.loadedPickables)
	{
		const Ref& r = w.refs[i];
		// Out of reach first (most of the nine cells' things): the eye's distance to the box. A door turned open is
		// tested against its turned box below instead
		if (r.doorAngle == 0.0f)
		{
			float d2 = 0.0f;
			for (int k = 0; k < 3; k++)
			{
				float c = fmaxf(r.boxMin[k], fminf(eye[k], r.boxMax[k])) - eye[k];
				d2 += c * c;
			}
			if (d2 > reach * reach)
				continue;
		}
		if (!isActivatable(r))
			continue;
		// A hatch in the ceiling (Sorkvild's, over the hall at Dagon Fel): its box reaches down round the player, so the eye is
		// inside it. OpenMW picks by the mesh, which looks overhead: looking steeply up at a door whose origin is above the eye
		// is that hatch, as near as the origin is
		if (r.recType == REC_DOOR && r.pos[2] > eye[2] && dir[2] > 0.7f && eye[2] > r.boxMin[2] && eye[2] < r.boxMax[2]
			&& eye[0] >= r.boxMin[0] && eye[0] <= r.boxMax[0] && eye[1] >= r.boxMin[1] && eye[1] <= r.boxMax[1])
		{
			float t = r.pos[2] - eye[2];
			if (t < bestT && t <= reach)
			{
				bestT = t;
				best = i;
			}
			continue;
		}
		// A big statue (Molag Bal's at Bal Ur: a box 580 x 975 round its plinth and steps) is stood beside, with the eye
		// inside its box. OpenMW picks by the mesh, which is only the statue: point at the middle part of the box then
		if (r.recType == REC_ACTI && eyeInsideBox(r, eye) && r.boxMax[0] - r.boxMin[0] <= 1200.0f && r.boxMax[1] - r.boxMin[1] <= 1200.0f)
		{
			float t = rayCoreEntry(r, eye, dir, bestT);
			if (t >= 0.0f && t < bestT && t <= reach)
			{
				bestT = t;
				best = i;
			}
			continue;
		}
		// An animated door turned open is where it is drawn, not where its box stands closed: the ray is turned back
		// about the door's origin by the door's own turn (the renderer turns the door by -doorAngle about its own up
		// axis), which leaves the distance along it unchanged. Aiming at the empty doorway then picks nothing
		const float* e = eye;
		const float* d = dir;
		float eyeDoor[3], dirDoor[3];
		if (r.recType == REC_DOOR && r.doorAngle != 0.0f)
		{
			float rel[3] = { eye[0] - r.pos[0], eye[1] - r.pos[1], eye[2] - r.pos[2] };
			doorSwing(r, r.doorAngle, rel, eyeDoor);
			for (int k = 0; k < 3; k++)
				eyeDoor[k] += r.pos[k];
			doorSwing(r, r.doorAngle, dir, dirDoor);
			e = eyeDoor;
			d = dirDoor;
		}
		if (!boxInReach(r, e, reach))
			continue;
		// A body lies on the floor in its standing box: OpenMW's pick meets the mesh, low down, and a look over the body
		// goes on to what is behind it (a skeleton killed before the door at Arvs-Drelen took the press meant for the door)
		float top = r.actor >= 0 && r.dead ? r.boxMin[2] + kCorpsePickHeight : 1e9f;
		float t = rayBoxEntry(r, e, d, bestT, top);
		if (t >= 0.0f && t < bestT)
		{
			bestT = t;
			best = i;
		}
		if (r.recType == REC_DOOR && r.doorAngle == 0.0f)
		{
			float tDoor = rayBoxEntry(r, e, d, reach, top);
			if (tDoor >= 0.0f && tDoor < shutT)
			{
				shutT = tDoor;
				shut = i;
			}
		}
	}
	// Someone pressed against the far side of a shut door (the Nix-Hound in its cage at Rotheran): their box pokes
	// through it and the ray met it first. OpenMW's pick meets the door's mesh: one whose middle is past the door's face
	// is behind it, and the door is what the crosshair means (an Open spell aimed at it landed on the creature)
	if (best >= 0 && shut >= 0 && best != shut && w.refs[best].actor >= 0)
	{
		const Ref& r = w.refs[best];
		float midT = (r.pos[0] - eye[0]) * dir[0] + (r.pos[1] - eye[1]) * dir[1] + (r.pos[2] - eye[2]) * dir[2];
		if (midT > shutT)
			best = shut;
	}
	// A person standing before such a statue, within its middle part (Derar Hlervu in front of Molag Bal's at Bal Ur): the
	// statue's mesh is behind him, so the ray meets him first, as OpenMW's mesh pick does. Someone on the ray short of the
	// statue's origin is what the crosshair means
	if (best >= 0 && w.refs[best].recType == REC_ACTI && eyeInsideBox(w.refs[best], eye))
	{
		const Ref& statue = w.refs[best];
		float originT = (statue.pos[0] - eye[0]) * dir[0] + (statue.pos[1] - eye[1]) * dir[1] + (statue.pos[2] - eye[2]) * dir[2];
		float actorT = fminf(reach, originT);
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor < 0 || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			float t = rayBoxEntry(r, eye, dir, actorT);
			if (t >= 0.0f && t < actorT)
			{
				actorT = t;
				best = i;
			}
		}
	}
	// A body lying among a plant's leaves (Ralyn Othravel fallen in the Tower of Dawn's scathecraw bed): the plant's box
	// is far bigger than its leaves and took the ray from every side. OpenMW's pick meets the meshes: a body the ray
	// reaches inside the container's box is what the crosshair means
	if (best >= 0 && w.refs[best].recType == REC_CONT)
	{
		const Ref& plant = w.refs[best];
		float bodyT = reach;
		int body = -1;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor < 0 || !r.dead || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			float t = rayBoxEntry(r, eye, dir, bodyT, r.boxMin[2] + kCorpsePickHeight);
			if (t < 0.0f || t >= bodyT)
				continue;
			bool inside = true;
			for (int k = 0; k < 3 && inside; k++)
			{
				float at = eye[k] + dir[k] * t;
				inside = at >= plant.boxMin[k] - 1.0f && at <= plant.boxMax[k] + 1.0f;
			}
			if (inside)
			{
				bodyT = t;
				body = i;
			}
		}
		if (body >= 0)
			best = body;
	}
	// A body's box is a crude one (the Journal of Tarhiel lands where he does): an item whose box lies within the
	// body's and is on the ray is what the crosshair means, as the body's mesh would let the ray through to it
	if (best >= 0 && w.refs[best].actor >= 0 && w.refs[best].dead)
	{
		const Ref& body = w.refs[best];
		int item = -1;
		float itemT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			bool inside = true;
			for (int k = 0; k < 3 && inside; k++)
				inside = r.boxMax[k] >= body.boxMin[k] && r.boxMin[k] <= body.boxMax[k];
			float t = inside ? rayBoxEntry(r, eye, dir, itemT) : -1.0f;
			if (t >= 0.0f && t < itemT)
			{
				itemT = t;
				item = i;
			}
		}
		if (item >= 0)
			best = item;
	}
	// A container's box reaches well past it (a basket's, a barrel's, turned: OpenMW picks by the meshes' collision
	// shapes instead): an item the ray reaches is what the crosshair means unless the ray crosses the container's middle
	// on the way to it. The middle: a column 0.7 of the box's narrower half-width round its centre (a round thing turned
	// fills about 0.76 of it). Ajira's report lies between two baskets in the Balmora guild
	if (best >= 0 && w.refs[best].recType == REC_CONT)
	{
		const Ref& box = w.refs[best];
		int item = -1;
		float itemT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			float t = rayBoxEntry(r, eye, dir, itemT);
			if (t >= 0.0f && t < itemT)
			{
				itemT = t;
				item = i;
			}
		}
		if (item >= 0)
		{
			// An item lying on the container (the ash statue on the crates in Morvayn Manor): its box is in the top half of the
			// container's and over its middle, so the ray reaches it first on the mesh's top, as OpenMW's nearest hit would
			const Ref& it = w.refs[item];
			float midZ = (box.boxMin[2] + box.boxMax[2]) * 0.5f;
			bool onTop = it.boxMin[2] >= midZ && it.boxMax[2] <= box.boxMax[2] + 40.0f;
			for (int k = 0; k < 2 && onTop; k++)
			{
				float c = (it.boxMin[k] + it.boxMax[k]) * 0.5f;
				onTop = c >= box.boxMin[k] && c <= box.boxMax[k];
			}
			if (onTop)
				return item;
			float cx = (box.boxMin[0] + box.boxMax[0]) * 0.5f, cy = (box.boxMin[1] + box.boxMax[1]) * 0.5f;
			float rc = 0.7f * 0.5f * fminf(box.boxMax[0] - box.boxMin[0], box.boxMax[1] - box.boxMin[1]);
			// the ray's nearest point to the column's axis before it reaches the item
			float d2 = dir[0] * dir[0] + dir[1] * dir[1];
			float t = d2 > 1e-6f ? ((cx - eye[0]) * dir[0] + (cy - eye[1]) * dir[1]) / d2 : 0.0f;
			t = fmaxf(0.0f, fminf(itemT, t));
			float px = eye[0] + dir[0] * t - cx, py = eye[1] + dir[1] * t - cy, pz = eye[2] + dir[2] * t;
			bool crosses = px * px + py * py <= rc * rc && pz >= box.boxMin[2] && pz <= box.boxMax[2];
			if (!crosses)
				best = item;
		}
	}
	// An activator is the same (the darts lying on the bed in the Llethri guard quarters): its box covers what lies on it,
	// where OpenMW picks by the mesh's collision shape. An item whose box lies within the activator's and is on the ray
	// is what the crosshair means
	if (best >= 0 && w.refs[best].recType == REC_ACTI)
	{
		const Ref& box = w.refs[best];
		int item = -1;
		float itemT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			bool inside = true;
			for (int k = 0; k < 3 && inside; k++)
				inside = r.boxMax[k] >= box.boxMin[k] && r.boxMin[k] <= box.boxMax[k];
			float t = inside ? rayBoxEntry(r, eye, dir, itemT) : -1.0f;
			if (t >= 0.0f && t < itemT)
			{
				itemT = t;
				item = i;
			}
		}
		if (item >= 0)
			best = item;
	}
	// An item lying on another item (the Gambolpuddy on a pillow in Ald Daedroth): the lower item's box covers the upper
	// one, and the ray reaches the upper first on the mesh's top, as OpenMW's nearest hit would. An item whose box starts
	// in the top half of the picked item's, centred over it, and is on the ray, is what the crosshair means
	if (best >= 0 && w.refs[best].actor < 0 && isItemType(w.refs[best].type))
	{
		const Ref& low = w.refs[best];
		float midZ = (low.boxMin[2] + low.boxMax[2]) * 0.5f;
		int upper = -1;
		float upperT = reach;
		for (int i : w.loadedPickables)
		{
			const Ref& r = w.refs[i];
			if (i == best || r.actor >= 0 || !isItemType(r.type) || !isActivatable(r) || !boxInReach(r, eye, reach))
				continue;
			bool over = r.boxMin[2] >= midZ && r.boxMax[2] > low.boxMax[2];
			for (int k = 0; k < 2 && over; k++)
			{
				float c = (r.boxMin[k] + r.boxMax[k]) * 0.5f;
				over = c >= low.boxMin[k] && c <= low.boxMax[k];
			}
			// or one lying within it (the ring in the pillow's box: a pillow's box is mostly its cushion's dip)
			bool within = true;
			for (int k = 0; k < 3 && within; k++)
				within = r.boxMin[k] >= low.boxMin[k] - 2.0f && r.boxMax[k] <= low.boxMax[k] + 2.0f;
			// (only a small one in a big misc thing's box, twenty times less: a key lying on a note is its own pick)
			if (within)
			{
				float vl = 1.0f, vr = 1.0f;
				for (int k = 0; k < 3; k++)
				{
					vl *= fmaxf(low.boxMax[k] - low.boxMin[k], 1.0f);
					vr *= fmaxf(r.boxMax[k] - r.boxMin[k], 1.0f);
				}
				within = vl >= 20.0f * vr && low.recType == REC_MISC;
			}
			float t = within ? rayBoxEntry(r, eye, dir, upperT) : -1.0f;
			if (t >= 0.0f && t < upperT)
			{
				upperT = t;
				upper = i;
			}
		}
		if (upper >= 0)
			best = upper;
	}
	return best;
}
