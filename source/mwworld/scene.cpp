// The loaded cells: entering, streaming exterior cells, memory, eviction
#include "mwworld/worldimp.hpp"
#include "components/files/datapath.hpp"

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform/linear.hpp"
extern "C" u32 __ctru_heap_size;
#include "platform/memstat.hpp"
#include <malloc.h>
#include "components/debug/debuglog.hpp"
#include "mwworld/worldhelpers.hpp"
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"
#include "mwsound/soundmanagerimp.hpp"

int World::refsInMemory() const
{
	int n = 0;
	for (auto& c : cells)
		if (c.refsLoaded)
			n += c.refCount;
	return n;
}

void World::rebuildScriptIndex()
{
	for (auto& r : refs)
		r.script = -1;
	for (size_t k = 0; k < scripts.size(); k++)
		if (scripts[k].ref >= 0 && scripts[k].item.empty())
			refs[scripts[k].ref].script = k;
}

void World::evictCell(int c)
{
	LevelCell& lc = cells[c];
	std::string state = cellStateText(c);
	if (!state.empty())
		storeCellState(lc.file, state);
	// References placed while playing in this cell go (they aren't kept)
	for (size_t k = 0; k < spawned.size();)
	{
		int i = spawned[k];
		if (refs[i].cell != c || !refs[i].spawnedRef)
		{
			k++;
			continue;
		}
		if (refs[i].dropped)
		{
			if (!refs[i].pickedUp)
			{
				DroppedItem d;
				d.item = { refs[i].idLower, refs[i].count, false, refs[i].dropState.condition, refs[i].dropState.soul,
					refs[i].dropState.charge };
				d.cell = lc.file;
				memcpy(d.pos, refs[i].pos, sizeof(d.pos));
				d.yaw = refs[i].rot[2];
				droppedWaiting.push_back(d);
			}
		}
		else if (!refs[i].ally)
		{
			PendingSpawn p = { refs[i].idLower, lc.file, { refs[i].pos[0], refs[i].pos[1], refs[i].pos[2] },
				refs[i].rot[2], refs[i].health, refs[i].dead };
			pendingSpawns.push_back(p);
			logf("world: placed %s waits for %s", p.id.c_str(), p.cell.c_str());
		}
		for (size_t s = 0; s < scripts.size(); s++)
			if (scripts[s].ref == i && scripts[s].item.empty())
			{
				scripts.erase(scripts.begin() + s);
				break;
			}
		refs[i] = Ref();
		refs[i].cell = -1;
		freeRefSlots(*this, i, 1);
		spawned.erase(spawned.begin() + k);
	}
	int first = lc.refBase, last = lc.refBase + lc.refCount;
	// Their scripts stop (the locals went into the state); carried items' scripts carry on alone.
	// Compacted in place: a fresh full-size vector per eviction moved the whole block across the heap
	// every few seconds, leaving a same-sized hole behind each move (fragmentation the Old 3DS can't afford)
	size_t keep = 0;
	for (size_t si = 0; si < scripts.size(); si++)
	{
		ScriptInstance& s = scripts[si];
		// a global script started on one of them (ref->StartScript) keeps it by cell file + index: the slot is freed
		// and reused by other cells, so the number alone would point at another object or at nothing
		if (s.target >= first && s.target < last)
		{
			s.targetCell = lc.file;
			s.targetIndex = s.target - first;
			s.target = -1;
		}
		if (s.ref >= first && s.ref < last)
		{
			if (s.item.empty())
				continue;
			s.ref = -1;
		}
		if (keep != si)
			scripts[keep] = std::move(s);
		keep++;
	}
	scripts.resize(keep);
	for (int i = first; i < last; i++)
	{
		refs[i] = Ref();
		refs[i].cell = -1;
	}
	freeRefSlots(*this, first, lc.refCount);
	for (int i = lc.pathBase; i < lc.pathBase + lc.pathCount; i++)
	{
		// the neighbours' links into these points go too (the slots get reused)
		for (int nb : pathPoints[i].links)
			if (nb < lc.pathBase || nb >= lc.pathBase + lc.pathCount)
			{
				auto& l = pathPoints[nb].links;
				l.erase(std::remove_if(l.begin(), l.end(), [&](int x) { return x >= lc.pathBase && x < lc.pathBase + lc.pathCount; }),
					l.end());
			}
		pathPoints[i].links.clear();
		pathPoints[i].cell = -1;
	}
	if (lc.pathCount)
		freeSlots(freePaths, lc.pathBase, lc.pathCount);
	logf("world: %s objects out of memory (%d, %s)", lc.file.c_str(), lc.refCount,
		state.empty() ? "unchanged" : "changes kept");
	lc.refsLoaded = false;
	lc.refBase = lc.refCount = lc.pathBase = lc.pathCount = 0;
	lc.droppedAt = time;
	rebuildScriptIndex();
}

int World::evictCells(int budget, const std::function<bool(int)>& inUse, bool now_)
{
	int total = refsInMemory(), evicted = 0;
	while (total > budget)
	{
		int best = -1;
		for (size_t c = 0; c < cells.size(); c++)
		{
			const LevelCell& lc = cells[c];
			if (!lc.refsLoaded || lc.live || (int)c == current || (best >= 0 && lc.lastUsed >= cells[best].lastUsed))
				continue;
			// ... nor one a script asked for in the last seconds: it would be read straight back. On an Old 3DS
			// out of heap, a quest script's remote ref read Caius Cosades's house back every few frames as fast
			// as it went (heap churn on the edge of running out). Game seconds, not the machine's: a busy PC ran fewer frames
			// in 10 of its own, kept other cells, and a test's later fights went another way (uber-main-quest in a full suite)
			// One read back within a minute of going (a global script asking after someone far away every 15 to 20
			// seconds: Carius in Fort Frostmoth, the Cavern of the Incarnate, Therana's chamber) stays two minutes after
			// each ask: dropped at 10, an Old 3DS read four such cells back every 17 seconds, 75 to 400 ms each, and
			// the heap went to holes no cell fitted. Not while the cells near the player wait for room in the heap:
			// they come first
			if (time - lc.askedAt < (lc.askedBack && !heapShort ? 120.0f : 10.0f) && !now_)
				continue;
			bool busy = false;
			for (int i = lc.refBase; i < lc.refBase + lc.refCount && !busy; i++)
				busy = inUse(i);
			// ... or one of its references stands in another cell now (PositionCell)
			for (int s : spawned)
				busy |= refs[s].cell == (int)c && refs[s].at >= 0;
			// ... or a path someone in a loaded cell is walking
			for (size_t i = 0; i < refs.size() && !busy; i++)
				if (refs[i].cell >= 0 && active(i))
					for (int pt : refs[i].path)
						busy |= pt >= lc.pathBase && pt < lc.pathBase + lc.pathCount;
			if (!busy)
				best = c;
		}
		if (best < 0)
			break;
		total -= cells[best].refCount;
		evictCell(best);
		evicted++;
	}
	return evicted;
}

// What character creation's scripts leave behind that later play needs: the Census and Excise Office's inner door, which the
// guard's CharGenDoorGuardTalker unlocks once the player has the papers (Socucius Ergalla is behind it), and the scripts as they
// end: the people's "state" at -1 (CharGenClassNPC then lets Socucius Ergalla talk, at 0 an A on him started the class menu over),
// the one-shot doors and messages "done"
void World::postChargenRef(Ref& r)
{
	static const char* const kDone[][2] = { { "chargen class", "state" }, { "chargen name", "state" }, { "chargen dock guard", "state" },
		{ "chargen boat guard 2", "state" }, { "chargen captain", "state" }, { "chargen door guard", "done" }, { "chargen captain", "done" },
		{ "chargen exit door", "done" }, { "chargen door captain", "done" }, { "chargen_shipdoor", "done" }, { "chargendoorjournal", "done" },
		{ "chargen_bed", "done" }, { "chargen barrel fatigue", "done" }, { "chargen dagger", "done" } };
	if (r.idLower.compare(0, 7, "chargen") != 0)
		return;
	if (r.idLower == "chargen door hall" && r.lockLevel > 0)
		r.lockLevel = -r.lockLevel;
	for (const auto& d : kDone)
		if (r.idLower == d[0])
		{
			float* v = r.script >= 0 ? scripts[r.script].local(d[1]) : nullptr;
			if (v)
				*v = strcmp(d[1], "state") == 0 ? -1.0f : 1.0f;
		}
}

bool World::cellExits(int ci, std::vector<DoorExit>& out)
{
	out.clear();
	if (ci < 0)
		return false;
	auto take = [&](const Ref& d) {
		if (d.recType != REC_DOOR || !d.hasDest || d.destUnconverted)
			return;
		DoorExit e;
		e.outdoors = d.destHasGrid;
		memcpy(e.pos, d.destPos, sizeof(e.pos));
		e.cell = d.destHasGrid ? -1 : cellIndex(d.destCell);
		out.push_back(e);
	};
	if (cells[ci].refsLoaded)
	{
		for (int i = cells[ci].refBase; i < cells[ci].refBase + cells[ci].refCount; i++)
			take(refs[i]);
		return true;
	}
	// (parsed and dropped: from Tribunal's cells, whose doors never lead outdoors, an Intervention's search read the
	// whole of Mournhold, Old Mournhold, Bamz-Amschend and Norenen-dur into memory, about 12000 objects of 832 bytes,
	// and the Old 3DS ran out of heap at the next town)
	ParsedRefs p;
	if (!parseRefs(*this, ci, p))
		return false;
	for (const Ref& d : p.refs)
		take(d);
	return true;
}

bool World::ensureRefs(int cellIndex)
{
	if (cellIndex >= 0)
		cells[cellIndex].askedAt = time;
	if (cellIndex < 0 || cells[cellIndex].refsLoaded)
		return cellIndex >= 0;
	ParsedRefs p;
	u64 start = osGetTime();
	if (!parseRefs(*this, cellIndex, p))
		return false;
	appendRefs(*this, cellIndex, p);
	LevelCell& lc = cells[cellIndex];
	lc.reads++;
	lc.askedBack |= time - lc.droppedAt < 60.0f;
	logf("world: read %s objects (%d) in %llu ms%s", lc.file.c_str(), lc.refCount, osGetTime() - start,
		lc.askedBack ? ", kept longer now (asked back soon after it went)" : "");
	return true;
}

Actor* World::actorOf(int ri)
{
	ActorSet* set = actorsOf(ri);
	return set && refs[ri].anim >= 0 ? &set->actors[refs[ri].anim] : nullptr;
}

// Puts an NPC's mesh and collision where its reference is (after moving) and lays the dead down
void World::syncActor(int ri)
{
	Ref& r = refs[ri];
	r.fitBox();
	Cell* c = cellOf(ri);
	if (c && r.cellActor >= 0)
	{
		CellActor& ca = c->actors[r.cellActor];
		memcpy(ca.pos, r.pos, sizeof(ca.pos));
		ca.radius = r.dead ? 0.0f : 28.0f * r.scaleFactor;     // (SetScale: the cylinder grows with them)
	}
	Actor* a = actorOf(ri);
	if (!a)
		return;
	if (a->scaleFactor != r.scaleFactor)
	{
		// SetScale since its place was made (a cell read again starts from the level's size): its size, not its spot
		float k = r.scaleFactor / a->scaleFactor;
		for (int i = 0; i < 12; i++)
			if (i % 4 != 3)
				a->place[i] *= k;
		a->scaleFactor = r.scaleFactor;
	}
	if (r.moved)
		actorSetPlacement(*a, r.pos[0], r.pos[1], r.pos[2] + r.boxLift, r.rot[2]);
	ActorSet& set = *actorsOf(ri);
	// a corpse that was already lying there is laid down at once; one that has just died keeps playing its fall
	int group = a->group;
	AnimMode mode = a->mode;
	if (r.dead && actorPlay(set, *a, r.deathGroup, ANIM_HOLD) && (a->group != group || a->mode != mode))
		a->time = actorSkeleton(set, a->skeleton).groups[a->group].stop;
}

void World::refreshHidden()
{
	for (LoadedCell* l : loaded)
	{
		const LevelCell& lc = cells[l->index];
		for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
			if (refs[i].visible())
			{
				showRanges(*this, refs[i]);
				if (refs[i].colGone)
					hideCollision(*this, refs[i]);
			}
			else
				hideRanges(*this, refs[i]);
	}
}

int World::cellIndex(const std::string& name) const
{
	std::string l = lower(name);
	for (size_t i = 0; i < cells.size(); i++)
		if (lower(cells[i].name) == l || cells[i].file == l)
			return i;
	return -1;
}

int World::gridCell(int gx, int gy) const
{
	auto it = gridIndex.find(((long long)gx << 32) ^ (unsigned)gy);
	return it == gridIndex.end() ? -1 : it->second;
}

bool World::isStartCell() const
{
	std::string l = lower(game.cellName);
	return lower(cells[current].name) == l || cells[current].file == l;
}

bool World::sameSpace(int a, int b) const
{
	const LevelCell& ca = cells[placeOf(a)];
	const LevelCell& cb = cells[placeOf(b)];
	return placeOf(a) == placeOf(b) || (!ca.interior && !cb.interior);
}

Scene World::scene()
{
	Scene s;
	for (LoadedCell* l : loaded)
		s.cells.push_back(&l->cell);
	s.here = current >= 0 && cells[current].live ? &cells[current].live->cell : nullptr;
	// Outdoors, whether the cell under the player's feet is loaded (a neighbour still streaming in: the feet are held
	// where they are until it is, playerUpdate). Off the level's cells (the sea past the island) counts as loaded: as ever
	if (current >= 0 && !cells[current].interior)
	{
		int under = gridCell((int)floorf(player.feet[0] / 8192.0f), (int)floorf(player.feet[1] / 8192.0f));
		s.groundThere = under < 0 || cells[under].live != nullptr;
	}
	return s;
}

// Makes a freshly loaded cell part of the world: its references find their meshes
static void integrateLevelCell(World& w, int index, LoadedCell* l)
{
	LevelCell& lc = w.cells[index];
	w.respawnCell(index);               // before it is live: revived actors come in standing
	// (water a script moved stays where it was put)
	if (lc.interior)
	{
		auto wl = w.waterLevels.find(lower(lc.name.str()));
		if (wl != w.waterLevels.end())
			moveCellWater(l->cell, wl->second);
	}
	lc.live = l;
	lc.loadedAt = w.time;
	w.loaded.push_back(l);
	char map[256];
	if (heapMapLine(map, sizeof(map)))
		logf("heapmap: %s in, %d KB used, arena %u KB used (biggest free %u KB): %s", lc.file.c_str(), mallinfo().uordblks / 1024,
			(unsigned)(cellArenaUsed() / 1024), (unsigned)(cellArenaLargest() / 1024), map);
	// A cell that comes back within a few seconds of being freed: the border between two cells thrashes it
	if (!lc.interior && w.time - lc.freedAt < 3.0f)
		monitorOnce(("cellthrash:" + lc.file).c_str(), "%s was freed and loaded again within %.1f s", lc.file.c_str(),
			w.time - lc.freedAt);
	// Cell files number references within the cell
	for (auto& a : l->actors.actors)
		if (a.ref >= 0)
			a.ref += lc.refBase;
	for (auto& d : l->cell.doors)
		d.ref += lc.refBase;
	for (auto& p : l->cell.places)
		p.ref += lc.refBase;
	// (one that stands in another cell now keeps its mesh and place there: its entries here go unused, and
	// that cell's, when it is loaded too, are left alone)
	for (size_t k = 0; k < l->actors.actors.size(); k++)
		if (l->actors.actors[k].ref >= 0)
		{
			if (w.refs[l->actors.actors[k].ref].at >= 0)
				l->actors.actors[k].ref = -1;
			else
				w.refs[l->actors.actors[k].ref].anim = k;
		}
	// Cell::actors lists the cell's NPCs in reference order
	int cylinder = 0;
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		if (w.refs[i].actor >= 0 && cylinder < (int)l->cell.actors.size())
		{
			if (w.refs[i].at >= 0)
				l->cell.actors[cylinder].radius = 0.0f;
			else
				w.refs[i].cellActor = cylinder;
			cylinder++;
		}
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
	{
		if (w.refs[i].at >= 0)
		{
			if (!w.cells[w.refs[i].at].live)
				w.refs[i].anim = w.refs[i].cellActor = -1;
			continue;                      // it stands in another cell now
		}
		if (w.refs[i].relib)
		{
			w.detachActor(i);              // a leveled pick: drawn from its own library, not the cell's
			w.attachSpawned(i);
			continue;
		}
		w.syncActor(i);
		w.refs[i].humanAnim = -1;
		if (w.refs[i].werewolf)
			w.werewolfBody(i);             // still in wolf form from before
		if (!w.refs[i].visible())
			hideRanges(w, w.refs[i]);
		else if (w.refs[i].colGone)
			hideCollision(w, w.refs[i]);
		// enchanted things lying about shimmer: listed once here, not looked for every frame (mwrender/renderingmanager.cpp)
		const Ref& r = w.refs[i];
		if (r.obj && !r.obj->ench.empty() && !r.ranges.empty())
			if (u32 glow = w.game.enchantGlow(r.id))
				l->glowRefs.push_back({ i, glow });
		// (lights, listed once too: Session lights people by those near where they stand now)
		if (r.obj && r.obj->recType == REC_LIGH && r.obj->lightRadius > 0 && !(r.obj->flags & 0x20))     // (LHDT 0x20: off by default)
			l->lightRefs.push_back(i);
	}
	lc.lastUsed = ++w.useTick;
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
			w.attachSpawned(i);
	w.rebuildLoadedLists();
}

// The single free block of heap a cell read wants: 1.5 MB for its collision's arrays, the biggest it takes; 512 KB while
// the cells' arena has a megabyte for them (they come from there: platform/cellarena.hpp), the rest of a cell being
// smaller blocks, its actors' through tryResize. (Kept at 1.5 MB, Skaal Village's neighbours never came with the arena
// all but empty and the heap's biggest block 1.1 MB)
static size_t cellHeapBlockNeeded()
{
	return g_cellArena.largest() >= (1u << 20) ? 512u * 1024u : 1536u * 1024u;
}

// Room for a cell's load: a block of what it asks of the heap, in the heap; or in the cells' arena when it has one
// for the cell and the ask both (what the heap can't give comes from the arena's room: arenaOverflowAlloc). Asked of
// the heap alone, an Old 3DS's forest cells waited with the heap in holes of 300 KB and the arena mostly empty
static bool cellLoadHasRoom()
{
	return heapInUse() + (5u << 20) < __ctru_heap_size
		&& (heapLargestBlock() >= cellHeapBlockNeeded() || g_cellArena.largest() >= (3u << 20));
}

// Reads a cell's files (safe on the streaming thread: touches nothing shared but the locked
// linear heap and texture cache)
static bool readLevelCell(const char* dataDir, const std::string& file, TextureCache& cache, LoadedCell* l)
{
	g_heapProbeFailed = false;
	CellScope scope;                  // (all it builds goes with the cell: the cells' arena, platform/cellarena.hpp)
	if (!cellLoad(l->cell, dataDir, (file + ".cel").c_str(), cache) || (g_streamCancel && threadGetCurrent()))
	{
		cellFree(l->cell, cache);
		return false;
	}
	// A missing .act (actors.empty()) still loads the cell, as ever. A read that broke off midway
	// (linear or heap memory short: readMesh / tryResize said no) frees the partials and fails the
	// cell cleanly — it waits and is streamed again when there is room. Partial actor sets used to be
	// kept, and a failed cell's textures and meshes used to leak with the job
	if (!actorsLoad(l->actors, cellPath(dataDir, file, ".act").c_str(), dataDir, l->cell.textureNames)
		&& !(l->actors.actors.empty() && l->actors.skeletons.empty()))
	{
		actorsFree(l->actors);
		cellFree(l->cell, cache);
		return false;
	}
	if (threadGetCurrent())
		g_workerAt = "readLevelCell: actors read";
	return true;
}

static bool loadLevelCell(World& w, int index)
{
	MARK("loadLevelCell");
	LevelCell& lc = w.cells[index];
	if (lc.live)
		return true;
	if (!w.ensureRefs(index))
		return false;
	LoadedCell* l = new LoadedCell();
	l->index = index;
	logf("world: loading %s", lc.file.c_str());
	if (!readLevelCell(w.dataDir, lc.file, w.textures, l))
	{
		delete l;
		return false;
	}
	integrateLevelCell(w, index, l);
	return true;
}

// ---- Streaming thread: one exterior cell at a time, while the game keeps running

struct StreamJob
{
	int index;
	LoadedCell* cell;
	std::string file;
	const char* dataDir;
	TextureCache* cache;
	const World* world;
	bool parseObjects = false;        // the cell's objects weren't read yet: read them on the thread too
	ParsedRefs objects;
	bool objectsOk = false;
	Thread thread = nullptr;
	volatile bool done = false;
	bool ok = false;
	u64 started = 0;
};

static void streamWorker(void* arg)
{
	StreamJob* j = (StreamJob*)arg;
	crashHandlerInstall(1);
	g_workerAt = "parseRefs";
	if (j->parseObjects)
		j->objectsOk = parseRefs(*j->world, j->index, j->objects);
	g_workerAt = "readLevelCell";
	j->ok = readLevelCell(j->dataDir, j->file, *j->cache, j->cell);
	g_workerAt = "done";
	__sync_synchronize();
	j->done = true;
}

// Takes in a finished (or, with wait, the running) job: integrates the cell if it's still wanted
static void finishStreamJob(World& w, bool wait)
{
	StreamJob* j = w.streamJob;
	if (!j || (!wait && !j->done))
		return;
	MARK("finishStreamJob: join");
	// (in steps: the streaming thread that runs out of memory waits for the main thread to give some back,
	// Session::relieveMemory, instead of the two waiting on each other for good)
	while (threadJoin(j->thread, 100000000ULL) != 0)          // (a timeout is not an R_FAILED result)
		if (g_memoryLow && w.memoryLowHook)
			w.memoryLowHook(false);
	MARK("finishStreamJob: integrate");
	threadFree(j->thread);
	w.streamJob = nullptr;
	if (j->parseObjects && j->objectsOk)
		appendRefs(w, j->index, j->objects);            // no-op if they were read meanwhile
	const LevelCell& c = w.cells[j->index];
	const LevelCell* here = w.current >= 0 ? &w.cells[w.current] : nullptr;
	bool wanted = here && !here->interior && abs(c.gx - here->gx) <= 1 && abs(c.gy - here->gy) <= 1 && !c.live
		&& c.refsLoaded;
	if (j->ok && wanted)
	{
		integrateLevelCell(w, j->index, j->cell);
		logf("world: streamed %s in %llu ms (in the background), textures %lu KB, linear free %lu KB, heap used %d KB",
			j->file.c_str(), osGetTime() - j->started, w.textures.bytes / 1024, linearSpaceFree() / 1024, mallinfo().uordblks / 1024);
	}
	else
	{
		if (j->ok)
		{
			actorsFree(j->cell->actors);
			cellFree(j->cell->cell, w.textures);
		}
		else if (g_streamCancel)
			logf("world: stopped reading %s (the world is let go of)", j->file.c_str());
		else
		{
			logf("world: failed to load %s", j->file.c_str());
			w.streamRetryAt = osGetTime() + 1500;       // (not the same read again every frame: it failed on memory)
		}
		delete j->cell;
	}
	delete j;
}

static void startStreamJob(World& w, int index)
{
	StreamJob* j = new StreamJob();
	j->index = index;
	j->cell = new LoadedCell();
	j->cell->index = index;
	j->file = w.cells[index].file;
	j->dataDir = w.dataDir;
	j->cache = &w.textures;
	j->world = &w;
	j->parseObjects = !w.cells[index].refsLoaded;
	j->started = osGetTime();
	// Below the main thread's priority: it runs while the main thread waits for the GPU. An Old 3DS has one core for
	// the game, so there it goes to the system core (up to half of it, APT_SetAppCpuTimeLimit in engine.cpp) and reads in
	// parallel; on the game's core if that can't be had
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	j->thread = g_sysCoreThreads ? threadCreate(streamWorker, j, 64 * 1024, prio + 1, 1, false) : nullptr;
	if (!j->thread)
		j->thread = threadCreate(streamWorker, j, 64 * 1024, prio + 1, -2, false);
	if (j->thread)
		w.streamThreaded = true;
	if (!j->thread)
	{
		// No thread could be made: load it right here instead
		logf("world: no streaming thread, loading %s now", j->file.c_str());
		if (w.ensureRefs(index) && readLevelCell(w.dataDir, j->file, w.textures, j->cell))
			integrateLevelCell(w, index, j->cell);
		else
			delete j->cell;
		delete j;
		return;
	}
	w.streamJob = j;
}

static void unloadLevelCell(World& w, int index)
{
	MARK("unloadLevelCell");
	LevelCell& lc = w.cells[index];
	if (!lc.live)
		return;
	// The index buffers holding hidden references go away with the geometry
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
	{
		// Capacity given back, not kept: a resident ref's empty-but-allocated buffer is a small pinned
		// block, and thousands of them cut the Old 3DS heap into holes no cell file fits
		w.refs[i].savedIndices = std::vector<u16>();
		w.refs[i].savedCol = std::vector<u32>();     // the collision goes too; hidden again when it loads
		if (w.refs[i].at >= 0)
			continue;                        // standing in another cell: drawn there
		w.refs[i].anim = w.refs[i].cellActor = -1;
		w.refs[i].ai = AI_IDLE;
	}
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
		{
			w.refs[i].anim = w.refs[i].cellActor = -1;
			w.refs[i].ai = AI_IDLE;
		}
	actorsFree(lc.live->actors);
	cellFree(lc.live->cell, w.textures);
	for (size_t i = 0; i < w.loaded.size(); i++)
		if (w.loaded[i] == lc.live)
		{
			w.loaded.erase(w.loaded.begin() + i);
			break;
		}
	delete lc.live;
	lc.live = nullptr;
	lc.freedAt = w.time;
	w.rebuildLoadedLists();
}

void World::waitForStream()
{
	finishStreamJob(*this, true);
}

bool World::dropFarthestLiveNeighbour()
{
	if (current < 0 || cells[current].interior)
		return false;
	int gx = cells[current].gx, gy = cells[current].gy;
	int best = -1, bestDist = 0;
	for (LoadedCell* l : loaded)
	{
		if (l->index == current)
			continue;
		const LevelCell& lc = cells[l->index];
		int d = abs(lc.gx - gx) + abs(lc.gy - gy);
		if (d > bestDist)
		{
			bestDist = d;
			best = l->index;
		}
	}
	if (best < 0)
		return false;
	logf("memory: unloading %s for its memory (it streams back when there is room)", cells[best].name.c_str());
	unloadLevelCell(*this, best);
	return true;
}

void World::freeAll()
{
	u64 t0 = osGetTime();
	bool streaming = streamJob != nullptr;
	const char* streamAt = g_workerAt;
	g_streamCancel = true;
	finishStreamJob(*this, true);
	g_streamCancel = false;
	u64 t1 = osGetTime();
	int n = (int)loaded.size();
	while (!loaded.empty())
	{
		LoadedCell* l = loaded.back();
		unloadLevelCell(*this, l->index);
		// (a loaded cell its LevelCell doesn't point at isn't unloaded there: this loop would spin, at the main thread's
		// priority, with the watchdog under it never running to say so)
		if (!loaded.empty() && loaded.back() == l)
		{
			monitorOnce("freeall", "cell %d is in the loaded list but not live: dropped", l->index);
			loaded.pop_back();
		}
	}
	logf("world: freed %d cells in %llu ms (%s %llu ms%s%s)", n, osGetTime() - t1, streaming ? "the cell streaming in stopped in"
		: "nothing streaming,", t1 - t0, streaming ? ", it was at " : "", streaming ? streamAt : "");
}

bool World::enterCell(int index)
{
	current = index;
	if (!cells[index].interior)
	{
		streamExterior(true);
		// The neighbours whose ground is in clear sight of the arrival (nearer than the fog's start) load now too, behind
		// the loading screen; the rest stream in, inside the fog. With the player's own cell alone, a silt strider stop or
		// a door near a cell's edge showed the bare distant land beside the player until the next one came
		if (cells[index].live)
			loadCellsNear(player.feet, kExteriorFogStart);
		logf("world: outside in %s (%d cells loaded), linear free %lu KB", cells[index].name.c_str(),
			(int)loaded.size(), linearSpaceFree() / 1024);
		return cells[index].live != nullptr;
	}
	finishStreamJob(*this, true);
	bool freed = false;
	for (int i = (int)loaded.size() - 1; i >= 0; i--)
		if (loaded[i]->index != index)
		{
			unloadLevelCell(*this, loaded[i]->index);
			freed = true;
		}
	// The cells just left give their memory back only once the GPU is done with them: now, before this one is
	// read (a busy town left for a shop had the shop's first textures fail, drawn plain until it was entered again)
	if (freed)
		linearReclaim();
	bool ok = loadLevelCell(*this, index);
	logf("world: entered %s (%d refs), textures %lu KB, linear free %lu KB", cells[index].name.c_str(),
		cells[index].refCount, textures.bytes / 1024, linearSpaceFree() / 1024);
	return ok;
}

// Outdoors an actor on its way somewhere (following, escorting, travelling) who walked over into a cell that
// stays loaded goes on in that one when its own is freed. OpenMW's World::moveObject puts an actor in the
// exterior cell under it at every step; here only as the old cell goes, so pacing at a border loads nothing.
// A follower vanished with the cell it started in, two cells behind the player
static void handOverWalkers(World& w, int index, const LevelCell& here)
{
	const LevelCell& lc = w.cells[index];
	std::vector<int> walkers;
	for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
		if (w.refs[i].at < 0)
			walkers.push_back(i);
	for (int i : w.spawned)
		if (w.placeOf(i) == index)
			walkers.push_back(i);
	for (int i : walkers)
	{
		Ref& r = w.refs[i];
		if (r.cell < 0 || r.actor < 0 || r.dead
			|| (r.aiPackage != AIPKG_FOLLOW && r.aiPackage != AIPKG_ESCORT && r.aiPackage != AIPKG_TRAVEL))
			continue;
		int g = w.gridCell((int)floorf(r.pos[0] / 8192.0f), (int)floorf(r.pos[1] / 8192.0f));
		if (g < 0 || g == index || !w.cells[g].live || abs(w.cells[g].gx - here.gx) > 1 || abs(w.cells[g].gy - here.gy) > 1)
			continue;
		// (where it stands and its home stay as they are)
		float pos[3] = { r.pos[0], r.pos[1], r.pos[2] }, home[3] = { r.home[0], r.home[1], r.home[2] };
		w.relocate(i, g, pos, r.rot[2]);
		memcpy(r.home, home, sizeof(home));
	}
}

void World::streamExterior(bool all)
{
	MARK("streamExterior");
	if (current < 0 || cells[current].interior)
		return;
	// Walked into another grid cell?
	int gx = (int)floorf(player.feet[0] / 8192.0f), gy = (int)floorf(player.feet[1] / 8192.0f);
	int under = gridCell(gx, gy);
	// Just over the line into a next-door cell, the cell the player was in stays "current": a player pacing on a border
	// freed the row of cells behind and loaded the row ahead on every crossing (uber bug 39). Past 512 units, or
	// farther than a neighbour, it changes. Not, with the cells streaming in on a thread, when the cell under the player
	// isn't there (the console reads one in 1 to 5 s, and after a door only the player's own is loaded): it loads now,
	// below. Kept as current, it stayed unloaded for those 512 units, there was no ground under the player, and the body
	// fell and was under the land when the cell came. (Without a thread, the PC build of the tests, a neighbour is read
	// on the spot a frame after it is asked for: the rule as it was, so the quest runs keep their timing. Not for an Old 3DS's
	// profile: it holds neighbours back for room, thread or none, and the console's own rule is the one that holds)
	bool nearEdge = false;
	if (under >= 0 && under != current)
	{
		const LevelCell& cur = cells[current];
		float ox = fmaxf(fmaxf(cur.gx * 8192.0f - player.feet[0], player.feet[0] - (cur.gx + 1) * 8192.0f), 0.0f);
		float oy = fmaxf(fmaxf(cur.gy * 8192.0f - player.feet[1], player.feet[1] - (cur.gy + 1) * 8192.0f), 0.0f);
		nearEdge = abs(cells[under].gx - cur.gx) <= 1 && abs(cells[under].gy - cur.gy) <= 1 && ox < 512.0f && oy < 512.0f
			&& cur.live && (cells[under].live || (!streamThreaded && !g_profile.streamWaitsForBlock));
	}
	if (under >= 0 && under != current && !nearEdge)
	{
		current = under;
		logf("world: outside in %s (%d %d)", cells[current].name.c_str(), gx, gy);
	}
	const LevelCell& c = cells[current];
	// Free what is no longer next to the player (and any interior)
	bool freed = false;
	for (int i = (int)loaded.size() - 1; i >= 0; i--)
	{
		const LevelCell& l = cells[loaded[i]->index];
		if (l.interior || abs(l.gx - c.gx) > 1 || abs(l.gy - c.gy) > 1)
		{
			if (!l.interior)
				handOverWalkers(*this, loaded[i]->index, c);
			unloadLevelCell(*this, loaded[i]->index);
			freed = true;
		}
	}
	// Where the player is heading: the feet's way since the last look, eased over about half a second (a door or a
	// teleport, a jump of more than a cell's half, starts it over)
	float dt = time - streamFeetAt;
	if (streamFeetAt >= 0.0f && dt > 0.0f)
	{
		float mx = player.feet[0] - streamFeet[0], my = player.feet[1] - streamFeet[1];
		if (mx * mx + my * my > 4096.0f * 4096.0f)
			streamHeading[0] = streamHeading[1] = 0.0f;
		else
		{
			float k = fminf(1.0f, dt * 2.0f);
			streamHeading[0] += (mx / dt - streamHeading[0]) * k;
			streamHeading[1] += (my / dt - streamHeading[1]) * k;
		}
	}
	streamFeet[0] = player.feet[0];
	streamFeet[1] = player.feet[1];
	streamFeetAt = time;
	// Load the missing neighbours, the player's own cell first: all now (behind a loading screen),
	// or one at a time on the streaming thread
	finishStreamJob(*this, all);
	int own = gridCell(c.gx, c.gy);
	if (own >= 0 && !cells[own].live)
	{
		// Never walk around in a cell that isn't there: the player's own cell loads now,
		// the neighbours follow on the streaming thread (they start past the fog anyway)
		finishStreamJob(*this, true);
		if (freed && !cells[own].live)
			linearReclaim();              // (the cells freed above: their memory, before this one is read)
		for (int tries = 0; !cells[own].live && !loadLevelCell(*this, own); tries++)
		{
			// No free block of heap for it: memory given back (far cells, skeletons, a neighbour's geometry), then again
			if (!g_heapProbeFailed || tries >= 3 || !memoryLowHook)
			{
				logf("world: failed to load %s", cells[own].file.c_str());
				break;
			}
			logf("world: %s waits for heap: giving memory back (try %d)", cells[own].file.c_str(), tries + 1);
			memoryLowHook(true);
		}
	}
	// The neighbours, the one that matters most first (an Old 3DS: room for two or three of them): the nearest edge, the
	// one ahead counting up to 1500 nearer (at a run). A fixed order (north, east, south, west, then the corners) gave an Old 3DS's room for two or three
	// neighbours to whichever came first in it, often behind the player, and the cell ahead came only once walked into
	float speed = sqrtf(streamHeading[0] * streamHeading[0] + streamHeading[1] * streamHeading[1]);
	auto score = [&](int ci, float* edge) {
		float x0 = cells[ci].gx * 8192.0f, y0 = cells[ci].gy * 8192.0f;
		float ox = fmaxf(fmaxf(x0 - player.feet[0], player.feet[0] - (x0 + 8192.0f)), 0.0f);
		float oy = fmaxf(fmaxf(y0 - player.feet[1], player.feet[1] - (y0 + 8192.0f)), 0.0f);
		*edge = sqrtf(ox * ox + oy * oy);
		float s = *edge;
		if (speed > 50.0f)
		{
			float dx = x0 + 4096.0f - player.feet[0], dy = y0 + 4096.0f - player.feet[1], d = sqrtf(dx * dx + dy * dy);
			if (d > 1.0f)
				s -= 1500.0f * fminf(1.0f, speed / 300.0f) * (dx * streamHeading[0] + dy * streamHeading[1]) / (d * speed);
		}
		return s;
	};
	int best = -1, nearest = -1;
	float bestScore = 1e9f, nearestEdge = 1e9f;
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
		{
			int ci = gridCell(c.gx + dx, c.gy + dy);
			if ((dx == 0 && dy == 0) || ci < 0 || cells[ci].live)
				continue;
			float e, s = score(ci, &e);
			if (e < nearestEdge)
			{
				nearestEdge = e;
				nearest = ci;
			}
			// (the fixed order the quest runs were tuned with, unless streamAhead: in the Hlaalu questline the other order
			// moved the timing so that the player fell through the Council Manor's floor, to be looked into)
			if (!streamAhead || !g_profile.streamWaitsForBlock)
			{
				static const int ord[3][3] = { { 7, 2, 6 }, { 3, 9, 1 }, { 5, 0, 4 } };   // [dy+1][dx+1]: N E S W, then NE NW SE SW
				s = (float)ord[dy + 1][dx + 1];
			}
			if (s < bestScore)
			{
				bestScore = s;
				best = ci;
			}
		}
	if (best < 0)
	{
		streamWanted = -1;
		return;
	}
	int ci = best;
	// The cell room was made for (below) comes before the order, while it is still a missing neighbour
	if (streamWanted >= 0)
	{
		const LevelCell& wc = cells[streamWanted];
		if (wc.live || wc.interior || abs(wc.gx - c.gx) > 1 || abs(wc.gy - c.gy) > 1)
			streamWanted = -1;
		else
			ci = streamWanted;
	}
	// (not in the update that freed cells: their memory comes back after the next frame begins)
	// An Old 3DS leaves a neighbour unloaded rather than start one with under 5 MB of heap left: a cell read that
	// runs the heap dry stops the game (the distant land covers the view until memory comes back)
	static bool noRoomNoted = false;
	// ... or with no free block of 1.5 MB (a heap with room in total but only in small holes: a cell's collision
	// and object lists are single blocks of several hundred KB). Probed twice a second at most, and again once a cell
	// or its objects came in since (useTick): cells that came in quicker than that (small ones, a fast card, the native
	// build) each started on the last probe's word, and Solstheim's Skaal Village ran the heap to its last 600 KB.
	// By the game's clock, not the wall's: the PC build runs many game seconds in a real one, and its probe went stale
	static float probedAt = -1e9f;
	static u32 probedTick = 0;
	static bool probedRoom = true;
	bool probedNow = false;
	// (only between jobs: the probe holds the biggest free block for a moment, and the streaming
	// thread allocating right then failed for nothing — no job starts while one runs anyway)
	if (g_profile.streamWaitsForBlock && !streamJob && (time - probedAt > 0.5f || time < probedAt || probedTick != useTick))
	{
		probedAt = time;
		probedNow = true;
		probedTick = useTick;
		probedRoom = cellLoadHasRoom();
	}
	bool room = !g_profile.streamWaitsForBlock || probedRoom;
	heapShort = !room;
	if (!room && !noRoomNoted)
	{
		noRoomNoted = true;
		logf("world: %d KB of heap used, biggest free block %u KB, the cells' arena %u KB used with %u KB in its biggest: the neighbouring cells wait for room",
			mallinfo().uordblks / 1024, (unsigned)(heapLargestBlock() / 1024), (unsigned)(cellArenaUsed() / 1024),
			(unsigned)(cellArenaLargest() / 1024));
	}
	// ... nor with under 5 MB of linear memory left (a cell takes 1 to 3 MB of it, its actors and sounds more): the 22 MB of an Old 3DS ran dry
	// with Seyda Neen's nine cells in, and the actors, the cells' last batches (white walls), the item icons and a
	// book's pages had none. The neighbours past it wait (the distant land stands in), as for the heap
	static bool noLinearNoted = false;
	bool linearRoom = !g_profile.streamWaitsForBlock || linearSpaceFree() >= (5u << 20);
	if (!linearRoom && !noLinearNoted)
		logf("world: %lu KB of linear memory left: the neighbouring cells wait for room", (unsigned long)(linearSpaceFree() / 1024));
	noLinearNoted = !linearRoom;
	room = room && linearRoom;
	if (room)
		noRoomNoted = false;
	// No room (an Old 3DS), and the player within 2000 of a missing neighbour's edge: the loaded neighbour farthest off goes
	// for it, if it is in the fog (past 3000) and 3000 farther than the one wanted, and has been in for 5 s (a player on a
	// corner doesn't swap two cells back and forth); the one wanted streams in next, whatever the order says. Its walkers go
	// on in the cells that stay, as when a cell is left behind. One at a time, each on a fresh probe's word (the last
	// probe's word stood for half a second, and three went at once). In Skaal Village late in the game the heap had room
	// for four of the eight, the order filled them with the cells to the north and east, and Rolf Long-Tooth's cell, at
	// the player's feet, waited until the player stood in it
	// (2 s after one goes before the next: a heap short of one big enough block stayed short however many went, their
	// memory coming back as holes between what stays, and four went in a moment for one cell)
	static float droppedAt = -1e9f;
	if (!room && probedNow && !streamJob && !freed && !streamPaused && nearest >= 0 && nearestEdge < 2000.0f
		&& (time - droppedAt > 2.0f || time < droppedAt))
	{
		int drop = -1;
		float dropEdge = 0.0f;
		for (LoadedCell* l : loaded)
		{
			const LevelCell& lc = cells[l->index];
			if (l->index == current || lc.interior || time - lc.loadedAt < 5.0f)
				continue;
			float e;
			score(l->index, &e);
			if (e > kExteriorFogStart && e > nearestEdge + 3000.0f && e > dropEdge)
			{
				dropEdge = e;
				drop = l->index;
			}
		}
		if (drop >= 0)
		{
			logf("world: %s let go for %s (%.0f units to its edge)", cells[drop].file.c_str(), cells[nearest].file.c_str(), nearestEdge);
			handOverWalkers(*this, drop, c);
			unloadLevelCell(*this, drop);
			streamWanted = nearest;
			droppedAt = time;
			probedAt = -1e9f;              // (probed again as soon as its memory is back)
			return;
		}
	}
	if (!streamJob && !freed && room && !streamPaused && osGetTime() >= streamRetryAt)
		startStreamJob(*this, ci);
}

// The neighbouring exterior cells within `reach` of a point, loaded now rather than on the streaming thread; returns
// how many. For an arrival behind the loading screen whose ground is another cell's: the Gnisis silt strider stop
// stands 650 units from the cell to the north, whose rock the player lands on, and on the console that cell streamed
// in a moment after the player had fallen through to the gully below
int World::loadCellsNear(const float pos[3], float reach)
{
	if (current < 0 || cells[current].interior)
		return 0;
	const LevelCell& c = cells[current];
	int n = 0;
	for (int dy = -1; dy <= 1; dy++)
		for (int dx = -1; dx <= 1; dx++)
		{
			int ci = gridCell(c.gx + dx, c.gy + dy);
			if (ci < 0 || cells[ci].live)
				continue;
			float x0 = cells[ci].gx * 8192.0f, y0 = cells[ci].gy * 8192.0f;
			float ox = fmaxf(fmaxf(x0 - pos[0], pos[0] - (x0 + 8192.0f)), 0.0f);
			float oy = fmaxf(fmaxf(y0 - pos[1], pos[1] - (y0 + 8192.0f)), 0.0f);
			if (ox * ox + oy * oy > reach * reach)
				continue;
			// (an Old 3DS reads one only with room for it, as streamExterior: a cell read that runs the heap dry stops the
			// game; it streams in once there is room)
			if (g_profile.streamWaitsForBlock
				&& !cellLoadHasRoom())
				continue;
			finishStreamJob(*this, true);
			if (!cells[ci].live && loadLevelCell(*this, ci))
			{
				logf("world: %s loaded with the arrival (its ground is %.0f units away)", cells[ci].file.c_str(), sqrtf(ox * ox + oy * oy));
				n++;
			}
		}
	return n;
}

void World::retryMissingTextures(float dt)
{
	// Textures a loaded cell couldn't read (linear memory full when it loaded) are tried again every few seconds:
	// one stayed plain, in its vertex colours, until the cell was entered again
	retryTimer += dt;
	if (retryTimer < 3.0f)
		return;
	retryTimer = 0.0f;
	for (LoadedCell* l : loaded)
		for (size_t i = 0; i < l->cell.textures.size(); i++)
			if (!l->cell.textures[i] && !l->cell.textureNames[i].empty())
				l->cell.textures[i] = textures.retry(dataDir, l->cell.textureNames[i]);
}

bool Session::refInUse(int ri) const
{
	if (ri == target || ri == enemyRef || ri == repairRef || ri == barterRef || ri == bookRef || ri == containerRef
		|| ri == arrestingGuard || ri == dlg.ref || w.followsPlayer(ri))
		return true;
	// one fighting the player: their cell's objects stay (Recall out of a fight, an assassin going for the player)
	const Ref& r = w.refs[ri];
	if (r.ai == AI_COMBAT && r.duelFoe < 0 && !r.dead && r.actor >= 0)
		return true;
	// speaking now (an entry stayed for everyone who ever said a line: a cell where someone once greeted the player was
	// never let go of, so memory filled with them and saves carried them whole)
	auto say = sayUntil.find(ri);
	if (say != sayUntil.end() && w.time < say->second)
		return true;
	for (auto& p : projectiles)
		if (p.owner == ri)
			return true;
	return false;
}

extern "C" u32 __ctru_heap_size;

int Session::refBudget() const
{
	int budget = g_profile.refBudget;
	// The heap filling up (past four fifths): fewer still, whatever the console. The heap outside the arenas: objects live
	// there, not in a cell's arena room (counted as free, it kept an Old 3DS 5000 objects where it had kept 2000, and the
	// heap the rest of the game has was cut into small pieces)
	size_t arenas = g_cellArena.size() + g_scriptArena.size(), used = mallinfo().uordblks;
	if (used > arenas && used - arenas > (__ctru_heap_size - arenas) / 5 * 4)
		budget = std::min(budget, w.refsInMemory() * 3 / 4);
	return budget;
}

// The skeletons no loaded actor uses go (read again when met). Returns the KB freed
int Session::trimSkeletons()
{
	std::vector<int> keep;
	for (LoadedCell* l : w.loaded)
		for (const Actor& a : l->actors.actors)
			keep.push_back(a.skeleton);
	for (const Actor& a : body.set.actors)
		keep.push_back(a.skeleton);
	if (body.skel >= 0)
		keep.push_back(body.skel);
	int kb = skeletonsTrim(keep);
	if (kb > 0)
		logf("memory: %d KB of animation skeletons let go, heap used %d KB", kb, mallinfo().uordblks / 1024);
	return kb;
}

void Session::cleanForLoad()
{
	int before = mallinfo().uordblks / 1024;
	w.waitForStream();                 // (no cell streaming: its actors may be reading a skeleton)
	// (behind the loading screen anything far goes, a cell a script asked for a moment ago or keeps asking for too: it
	// is read back if asked again. Kept, an Old 3DS on a Recall held 7,500 objects of a 5,000 budget, the heap down to a
	// 32 KB block, and Curio Manor couldn't be unpacked: the player stood in an empty cell)
	evictFarCells(0, true);
	freeMapTiles();
	audioCacheClear();
	trimSkeletons();
	Atom::prefill();
	logf("memory: cleaned for the load: heap used %d KB (was %d KB), biggest free block %u KB", mallinfo().uordblks / 1024, before,
		(unsigned)(heapLargestBlock() / 1024));
}

void Session::relieveMemory(bool force)
{
	// (at most a few times a second: while nothing more can go, every frame would scan the cells again)
	static u64 last = 0;
	u64 now = osGetTime();
	if (!force && now - last < 250)
		return;
	last = now;
	int before = mallinfo().uordblks / 1024;
	evictFarCells(0);
	freeMapTiles();
	memoryReserveTake();
	// Still spent: the first round freed nothing contiguous (far cells were already gone). Escalate, or
	// the streaming thread retries its allocation forever against the same holes — the step-631 freeze:
	// unused skeletons first, then a loaded neighbour's geometry (its collision arrays are the large
	// single blocks the loader wants; the cell streams back in once there is room)
	if (!memoryReserveHeld())
	{
		trimSkeletons();
		memoryReserveTake();
		if (!memoryReserveHeld() && w.dropFarthestLiveNeighbour())
			memoryReserveTake();
	}
	int after = mallinfo().uordblks / 1024;
	static int lastLogged = -1;
	if (after != lastLogged || memoryReserveHeld())
		logf("memory: the heap ran out: far cells and map tiles freed, %d KB used (was %d KB), reserve %s", after,
			before, memoryReserveHeld() ? "taken again" : "still spent");
	lastLogged = after;
}

void Session::evictFarCells(int budget, bool now)
{
	if (budget < 0)
		budget = refBudget();
	// lines said and done are forgotten (their references' slots go to other cells' objects)
	for (auto it = sayUntil.begin(); it != sayUntil.end();)
		it = w.time >= it->second ? sayUntil.erase(it) : std::next(it);
	dropDeadLoops();
	int n = w.evictCells(budget, [this](int ri) { return refInUse(ri); }, now);
	if (n)
		logf("world: %d cells' objects out of memory, %d objects in memory, %d cells' changes kept",
			n, w.refsInMemory(), (int)w.cellStates.size());
}
