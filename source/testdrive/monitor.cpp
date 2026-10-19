// monitor: checks that run every frame of a test (actors, floors, cells)
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "components/debug/debuglog.hpp"
#include <malloc.h>

void Session::noteHudMismatch(int left, int right)
{
	monitorOnce(("hud:" + std::to_string((int)screen) + ":" + std::to_string(dlg.open)).c_str(),
		"the HUD drew %d things in the left eye and %d in the right (screen %d, dialogue %d, %s): one eye is missing something",
		left, right, (int)screen, (int)dlg.open, w.cellName().c_str());
}

// Engine-side checks, run in every test (and the game): a "monitor:" line each, once per actor or place.
// Floating or sunk actors, actors going nowhere, the player under the world, doors that lead nowhere,
// linear memory that shrinks each time the same cell is entered.
void Session::monitorActors(float dt)
{
	monitorTimer += dt;
	if (monitorTimer < 1.0f)
		return;
	float step = monitorTimer;
	monitorTimer = 0.0f;
	if (monitorCell != w.current)
		monitorCellChanged();
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		// Out of the crosshair's reach: the box it's aimed at isn't where the actor stands (no talking to
		// them, nor searching the body)
		if (r.visible() && (!r.hasBox || r.pos[0] < r.boxMin[0] || r.pos[0] > r.boxMax[0] || r.pos[1] < r.boxMin[1]
				|| r.pos[1] > r.boxMax[1]))
			monitorOnce(("aim:" + r.idLower + ":" + w.cellName()).c_str(),
				"%s can't be aimed at in %s: at %.0f %.0f %.0f, its box %s %.0f %.0f .. %.0f %.0f", r.id.c_str(),
				w.cellName().c_str(), r.pos[0], r.pos[1], r.pos[2], r.hasBox ? "" : "(none)", r.boxMin[0], r.boxMin[1],
				r.boxMax[0], r.boxMax[1]);
		Actor* a = r.dead || !r.visible() ? nullptr : w.actorOf(i);
		if (!a)
			continue;
		// Drawn, or in the way, somewhere other than where it stands: another copy of its position that
		// whatever moved it (a cell's state, a save, a script) didn't update
		{
			const Cell* cell = w.cellOf(i);
			bool hasBlock = cell && r.cellActor >= 0 && r.cellActor < (int)cell->actors.size();
			float blocks[2] = { 0.0f, 0.0f };     // (copied: the record is packed)
			if (hasBlock)
				memcpy(blocks, (const void*)&cell->actors[r.cellActor].pos, sizeof(blocks));
			float ddx = a->place[3] - r.pos[0], ddy = a->place[7] - r.pos[1];
			if (ddx * ddx + ddy * ddy > 4.0f * 4.0f)
				monitorOnce(("drawn:" + r.idLower + ":" + w.cellName()).c_str(),
					"%s is drawn at %.0f %.0f but stands at %.0f %.0f in %s", r.id.c_str(), a->place[3], a->place[7],
					r.pos[0], r.pos[1], w.cellName().c_str());
			if (hasBlock && ((blocks[0] - r.pos[0]) * (blocks[0] - r.pos[0]) + (blocks[1] - r.pos[1]) * (blocks[1] - r.pos[1])
					> 4.0f * 4.0f))
				monitorOnce(("blocks:" + r.idLower + ":" + w.cellName()).c_str(),
					"%s blocks the way at %.0f %.0f but stands at %.0f %.0f in %s", r.id.c_str(), blocks[0], blocks[1],
					r.pos[0], r.pos[1], w.cellName().c_str());
		}
		const Skeleton& sk = actorSkeleton(*w.actorsOf(i), a->skeleton);
		const char* g = a->group < (int)sk.groups.size() ? sk.groups[a->group].name : "";
		bool cycle = a->mode == ANIM_LOOP && (strstr(g, "Walk") || strstr(g, "Run"));
		Watch& wt = watches[i];
		float mx = r.pos[0] - wt.x, my = r.pos[1] - wt.y;
		wt.x = r.pos[0];
		wt.y = r.pos[1];
		wt.still = cycle && mx * mx + my * my < 15.0f * 15.0f ? wt.still + step : 0.0f;
		if (wt.still >= 4.0f && !wt.told)
		{
			wt.told = true;
			logf("monitor: %s walks in place (%s) at %.0f %.0f %.0f%s [ai %d, package %d, fleeing %d, still %.1f, hitAt %.1f, "
				"player %.0f away, %.0f up]", r.id.c_str(), g, r.pos[0], r.pos[1], r.pos[2],
				dlg.open && dlg.ref == i ? ", while talking" : "", (int)r.ai, (int)r.aiPackage, (int)r.fleeing, r.chaseStill,
				r.hitAt, w.distanceToPlayer(i), w.player.feet[2] - r.pos[2]);
		}

		// Off the floor: an idle person more than 8 units above the floor under their feet or 20 below it
		// (creatures that fly, or swim while in water, hang where they are, so they're only checked for sinking),
		// for 2 s
		bool creature = r.actor >= 0 && w.game.actors[r.actor].creature;
		if (creature)
		{
			u8 afloat = w.game.actors[r.actor].afloat;
			creature = (afloat & 0x20) || ((afloat & 0x10) && w.underWater(r.pos[2]));
		}
		bool idle = r.ai != AI_COMBAT && !r.falling && r.knockTimer <= 0.0f && !a->swimming;
		// (on the floor anywhere from the floor under the middle up to the highest under the feet: a step and
		// gravity stand them on the latter, as OpenMW's actor box rests on a slope or a ledge's edge)
		float floorZ = -1e9f, footZ = -1e9f;
		if (idle)
			for (LoadedCell* l : w.loaded)
			{
				float z;
				if (collisionFloor(l->cell.collision, r.pos[0], r.pos[1], r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z) && z > floorZ)
					floorZ = z;
				if (collisionFootFloor(l->cell.collision, r.pos[0], r.pos[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 6000.0f,
						&z) && z > footZ)
					footZ = z;
			}
		float gap = floorZ > -1e8f ? r.pos[2] - floorZ : 0.0f;
		if (gap > 0.0f)
			gap = fmaxf(0.0f, r.pos[2] - footZ);
		bool off = idle && ((floorZ < -1e8f && !creature) || gap < -20.0f || (gap > 8.0f && !creature));
		wt.off = off ? wt.off + step : 0.0f;
		if (wt.off >= 2.0f && !wt.toldFloor)
		{
			wt.toldFloor = true;
			if (floorZ < -1e8f)
				logf("monitor: %s has no floor under it at %.0f %.0f %.0f in %s", r.id.c_str(), r.pos[0], r.pos[1], r.pos[2],
					w.cellName().c_str());
			else
				logf("monitor: %s is %.0f units %s the floor at %.0f %.0f %.0f in %s", r.id.c_str(), fabsf(gap),
					gap > 0 ? "above" : "below", r.pos[0], r.pos[1], r.pos[2], w.cellName().c_str());
		}

		// Going somewhere and getting nowhere: a travel / follow / escort package, or a fight, with under 15
		// units of net movement in 5 s (next to the player is where a follower or a fighter stops)
		// (a scripted follower that hasn't seen its leader yet stands, as in OpenMW; a follower stops within
		// follow distance, 256, of whoever it follows, which needn't be the player)
		bool going = ((r.aiPackage == AIPKG_TRAVEL || r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && !r.aiDone
				&& r.aiActive) || r.ai == AI_COMBAT;
		float leaderDist = w.distanceToPlayer(i);
		if ((r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.ai != AI_COMBAT && r.aiTarget != "player"
			&& !r.aiTarget.empty())
		{
			int t = w.findRef(r.aiTarget);
			if (t >= 0)
			{
				float dx = w.refs[t].pos[0] - r.pos[0], dy = w.refs[t].pos[1] - r.pos[1];
				leaderDist = sqrtf(dx * dx + dy * dy);
			}
		}
		bool near = r.aiPackage != AIPKG_TRAVEL && leaderDist < 300.0f;
		// (a fleeing actor stands while its foe is out of reach, one held in a pose for a test stands: not stuck)
		const Actor* held = w.actorOf(i);
		if (!going || near || r.knockTimer > 0.0f || (r.fleeing && r.fleeState == 0) || (held && held->frozen) || (dlg.open && dlg.ref == i))
		{
			wt.moveT = 0.0f;
			wt.anchorX = r.pos[0];
			wt.anchorY = r.pos[1];
		}
		else
		{
			float ax = r.pos[0] - wt.anchorX, ay = r.pos[1] - wt.anchorY;
			if (ax * ax + ay * ay >= 15.0f * 15.0f)
			{
				wt.moveT = 0.0f;
				wt.anchorX = r.pos[0];
				wt.anchorY = r.pos[1];
			}
			else
				wt.moveT += step;
			if (wt.moveT >= 5.0f && !wt.toldStuck)
			{
				wt.toldStuck = true;
				logf("monitor: %s is stuck at %.0f %.0f %.0f in %s (%s, package %d, target %s, path %d points, player %.0f away)",
					r.id.c_str(), r.pos[0], r.pos[1], r.pos[2], w.cellName().c_str(), r.ai == AI_COMBAT ? "fighting" : "travelling",
					r.aiPackage, r.aiTarget.c_str(), (int)r.path.size(), w.distanceToPlayer(i));
			}
		}
	}
	if (watches.size() > 400)
		watches.clear();

	// The player under the world: no floor in any loaded cell below the feet, or in the air for over 3 s
	// (flying, levitating and swimming excepted)
	const Player& p = w.player;
	bool airborne = !p.onGround && !p.flying && !p.swimming && p.levitate <= 0.0f && !p.waterWalk && !playerDead
		&& !menuOpen() && travelCell < 0;
	playerAirT = airborne ? playerAirT + step : 0.0f;
	if (airborne)
	{
		float floorZ = -1e9f;
		for (LoadedCell* l : w.loaded)
		{
			float z;
			if (collisionFloor(l->cell.collision, p.feet[0], p.feet[1], p.feet[2] + 30.0f, p.feet[2] - 100000.0f, &z) && z > floorZ)
				floorZ = z;
		}
		// (a jump may hang in the air much longer, over open sea too: Scroll of Icarian Flight)
		bool flight = p.jumpFlight && p.feet[2] > 0.0f;
		if (((floorZ < -1e8f && !flight) || (playerAirT >= 3.0f && !p.jumpFlight)) && !toldFall)
		{
			toldFall = true;
			logf("monitor: the player is %s at %.0f %.0f %.0f in %s (%.1f s in the air, vz %.0f)",
				floorZ < -1e8f ? "under every floor" : "falling", p.feet[0], p.feet[1], p.feet[2], w.cellName().c_str(),
				playerAirT, p.vz);
		}
	}
	else if (p.onGround)
		toldFall = false;
}

// A new cell: doors in it that lead nowhere, and the linear memory left each time the same cell is entered
// (falling on each visit: something a visit allocates isn't given back)
void Session::monitorCellChanged()
{
	monitorCell = w.current;
	if (w.current < 0)
		return;
	w.forLoadedRefs([&](int i) {
		const Ref& r = w.refs[i];
		if (!r.hasDest || r.destEnd)
			return;
		int dest = r.destUnconverted ? -1 : r.destHasGrid ? w.gridCell(r.destGrid[0], r.destGrid[1]) : w.cellIndex(r.destCell);
		if (dest < 0)
			monitorOnce(("door:" + r.idLower + ":" + w.cells[w.current].name).c_str(),
				"door %s in %s leads to %s, which is not in the level (unconverted %d)", r.id.c_str(), w.cellName().c_str(),
				r.destHasGrid ? "an exterior cell" : r.destCell.c_str(), (int)r.destUnconverted);
	});
	std::vector<unsigned long>& m = cellMemory[w.cellName()];
	unsigned long freeKb = linearSpaceFree() / 1024;
	// what holds linear memory: textures, loaded cells' geometry, their people, distant land, decoded sounds (the rest:
	// the GPU's command buffer, the UI, sky, effects)
	u32 geometry = 0, actorBytes = 0;
	for (LoadedCell* l : w.loaded)
	{
		geometry += l->cell.geometryBytes;
		actorBytes += l->actors.bytes;
	}
	logf("cellmem: %s entered, %lu KB linear free (visit %d): textures %lu, cells %lu, actors %lu, distant %lu, sounds %lu KB",
		w.cellName().c_str(), freeKb, (int)m.size() + 1, (unsigned long)(w.textures.bytes / 1024), (unsigned long)(geometry / 1024),
		(unsigned long)(actorBytes / 1024), (unsigned long)(distantBytes() / 1024), (unsigned long)(audioCacheBytes() / 1024));
	// The check counts decoded sounds and distant land as free: both are capped caches that fill over a session (a New
	// 3DS keeps up to 6 MB of sounds), and a cache filling up read as a leak on hardware. Interiors only: outside, what
	// is loaded at entry depends on the cells still streaming and the way in (one name covers several grid cells, as
	// Sadrith Mora's three), 3 MB apart from one visit to the next with nothing leaked
	m.push_back(freeKb + (unsigned long)((audioCacheBytes() + distantBytes()) / 1024));
	size_t n = m.size();
	// (from the fifth visit on: the first entry always has the most free, the caches that fill after it not yet there)
	if (w.cells[w.current].interior && n >= 5 && m[n - 1] + 64 < m[n - 2] && m[n - 2] + 64 < m[n - 3]
		&& m[n - 3] + 64 < m[n - 4])
		monitorOnce(("mem:" + w.cellName()).c_str(),
			"linear memory falls on each visit to %s: %lu, %lu, %lu, %lu KB free at entry (sound and distant caches counted free)",
			w.cellName().c_str(), m[n - 4], m[n - 3], m[n - 2], m[n - 1]);
	if (n > 12)
		m.erase(m.begin());
}
