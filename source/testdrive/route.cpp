// The driver's routes: the path grid, dead ends, flood fill
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include <malloc.h>

// The path grid of a cave can be split in two pieces with no link between them: findPath then has no route. A player
// walks as far as the grid goes and then crosses by sight: to the point of the goal's piece that can be seen from a
// point of the start's piece (a gap, a step down), else along the start's piece to the point nearest the goal.
static void gridPiece(World& w, int from, std::vector<char>& in)
{
	in.assign(w.pathPoints.size(), 0);
	std::vector<int> todo(1, from);
	in[from] = 1;
	while (!todo.empty())
	{
		int c = todo.back();
		todo.pop_back();
		for (int nb : w.pathPoints[c].links)
		{
			if (nb < 0 || nb >= (int)in.size() || in[nb] || w.pathPoints[nb].cell < 0 || !w.cells[w.pathPoints[nb].cell].live)
				continue;
			in[nb] = 1;
			todo.push_back(nb);
		}
	}
}

static float gridDist(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

static bool splitGridPath(Session& s, const float from[3], const float to[3], std::vector<int>& path)
{
	World& w = s.w;
	int st = w.nearestPathPoint(from), gl = w.nearestPathPoint(to);
	if (st < 0 || gl < 0 || st == gl)
		return false;
	std::vector<char> inS, inG;
	gridPiece(w, st, inS);
	gridPiece(w, gl, inG);
	std::vector<std::pair<float, int>> sp, gp;
	for (size_t i = 0; i < inS.size(); i++)
	{
		if (inS[i])
			sp.push_back({ gridDist(w.pathPoints[i].pos, to), (int)i });
		if (inG[i])
			gp.push_back({ gridDist(w.pathPoints[i].pos, to), (int)i });
	}
	std::sort(sp.begin(), sp.end());
	std::sort(gp.begin(), gp.end());
	// (the goal's points nearest the goal first; for each the start's points nearest to it, seen from one another)
	// (a first pass keeps to a crossing a player walks, a step down of 200 or less; only when there is none does a drop
	// count, as a ledge over a pit does. Without it a balcony above the Rotheran arena's pit was "seen" from the pit
	// floor, and the walk ran into the balcony's wall instead of taking the ramp at the west end.)
	int bestA = -1, bestB = -1;
	for (int pass = 0; pass < 2 && bestA < 0; pass++)
	for (size_t k = 0; k < gp.size() && k < 12 && bestA < 0; k++)
	{
		const float* b = w.pathPoints[gp[k].second].pos;
		std::vector<std::pair<float, int>> near;
		for (size_t i = 0; i < sp.size(); i++)
			near.push_back({ gridDist(w.pathPoints[sp[i].second].pos, b), sp[i].second });
		std::sort(near.begin(), near.end());
		for (size_t j = 0; j < near.size() && j < 40; j++)
		{
			const float* a = w.pathPoints[near[j].second].pos;
			float pa[3] = { a[0], a[1], a[2] + 50.0f }, pb[3] = { b[0], b[1], b[2] + 50.0f };
			if (near[j].first < 3000.0f && fabsf(a[2] - b[2]) < (pass == 0 ? 200.0f : 1500.0f) && w.lineOfSight(pa, pb, false))
			{
				bestA = near[j].second;
				bestB = gp[k].second;
				break;
			}
		}
	}
	std::vector<int> head, tail;
	if (bestA >= 0)
	{
		if (!w.findPath(from, w.pathPoints[bestA].pos, head))
			head.clear();
		// (the end of the goal's piece nearest the goal on the map and in height: not the pool under a door's ledge)
		int end = bestB;
		float endCost = 1e30f;
		for (size_t i = 0; i < inG.size(); i++)
		{
			const float* q = w.pathPoints[i].pos;
			float c = hypotf(q[0] - to[0], q[1] - to[1]) + 2.0f * fabsf(q[2] - to[2]);
			if (inG[i] && c < endCost)
			{
				endCost = c;
				end = (int)i;
			}
		}
		if (!w.findPath(w.pathPoints[bestB].pos, w.pathPoints[end].pos, tail))
			tail.clear();
		path = head;
		path.push_back(bestB);
		for (int t : tail)
			if (t != bestB)
				path.push_back(t);
		logf("drive: split path grid: crossing from point %d to %d by sight", bestA, bestB);
		return true;
	}
	// (a cave only: outdoors the grid is patchy, the land between is open, and the straight walk is the better guess)
	if (w.current < 0 || !w.cells[w.current].interior || sp.empty() || !w.findPath(from, w.pathPoints[sp[0].second].pos, path))
		return false;
	logf("drive: split path grid: along the start's piece to point %d, nearest the goal", sp[0].second);
	return true;
}

// Stuck at the end of the start's piece of a split grid, the end nearest the goal (a cave's grid in two pieces with
// nothing in sight between: the Ashalmimilkala shrine, where the goal's piece starts down a ramp round a wall): the
// straight walk at the goal only meets the wall. Gives the goal's piece's point nearest us (height counted double), for
// a route by trial steps to it; from there the goal's piece leads on
bool splitDeadEnd(Session& s, const float from[3], const float to[3], float entry[3], int* entryIndex)
{
	World& w = s.w;
	if (w.current < 0 || !w.cells[w.current].interior)
		return false;
	int st = w.nearestPathPoint(from), gl = w.nearestPathPoint(to);
	if (st < 0 || gl < 0 || st == gl)
		return false;
	std::vector<char> inS, inG;
	gridPiece(w, st, inS);
	if (inS[gl])
		return false;
	gridPiece(w, gl, inG);
	int near = -1, best = -1;
	float nearD = 1e30f, bestC = 3000.0f;
	for (size_t i = 0; i < inS.size(); i++)
	{
		const float* q = w.pathPoints[i].pos;
		if (inS[i] && gridDist(q, to) < nearD)
		{
			nearD = gridDist(q, to);
			near = (int)i;
		}
		float c = hypotf(q[0] - from[0], q[1] - from[1]) + 2.0f * fabsf(q[2] - from[2]);
		if (inG[i] && c < bestC)
		{
			bestC = c;
			best = (int)i;
		}
	}
	if (near < 0 || best < 0)
		return false;
	const float* e = w.pathPoints[near].pos;
	if (hypotf(e[0] - from[0], e[1] - from[1]) > 300.0f || fabsf(e[2] - from[2]) > 200.0f)
		return false;
	memcpy(entry, w.pathPoints[best].pos, sizeof(float) * 3);
	*entryIndex = best;
	logf("drive: at the dead end of a split path grid (point %d): a route by trial steps to the goal's piece at point %d", near, best);
	return true;
}

// The grid route for a walk. World::findPath snaps both ends to the nearest grid point up to 2500 away, a goal in plain
// sight included, so a walk to a spot would first head for a point far off its way. A route is kept only when it
// helps: its last points on another floor than the goal (a walkway above a door, a stair landing) are dropped, and a
// walk to a spot goes straight unless the route still ends near it.
// Where a walk to a thing ends: its origin, but for a door whose origin sits well outside its own mesh (an Ashlander yurt's:
// the model is placed 336 units from the reference, inside the tent) the middle of its box, where the door shows (other
// doors keep their origin: a cave door's box takes in the rock round it)
void walkTarget(const Ref& r, bool activate, float out[3])
{
	out[0] = r.pos[0];
	out[1] = r.pos[1];
	out[2] = r.pos[2];
	if (activate && r.recType == REC_DOOR && r.boxMax[0] > r.boxMin[0])
	{
		float ox = fmaxf(r.boxMin[0] - r.pos[0], fmaxf(0.0f, r.pos[0] - r.boxMax[0]));
		float oy = fmaxf(r.boxMin[1] - r.pos[1], fmaxf(0.0f, r.pos[1] - r.boxMax[1]));
		if (ox * ox + oy * oy > 100.0f * 100.0f)
		{
			out[0] = (r.boxMin[0] + r.boxMax[0]) * 0.5f;
			out[1] = (r.boxMin[1] + r.boxMax[1]) * 0.5f;
		}
	}
}

bool planPath(Session& s, const float from[3], const float to[3], bool spot, std::vector<int>& path)
{
	World& w = s.w;
	// close and in plain sight (a shrine on a plateau with no grid near it, the nearest point at the foot of the hill): straight
	// at it. The grid's route went down to that point, then back up straight, then down again, until the time ran out
	float gd = hypotf(to[0] - from[0], to[1] - from[1]);
	if (gd < 500.0f)
	{
		float a[3] = { from[0], from[1], from[2] + 60.0f }, b[3] = { to[0], to[1], to[2] + 60.0f };
		int first = w.nearestPathPoint(from);
		if (first >= 0 && hypotf(w.pathPoints[first].pos[0] - from[0], w.pathPoints[first].pos[1] - from[1]) > gd + 200.0f
			&& w.lineOfSight(a, b, false))
		{
			path.clear();
			return false;
		}
	}
	if (!w.findPath(from, to, path))
	{
		path.clear();
		// the grid is in two pieces (Urshilaku Karma Burial): no route from here to the goal's piece
		if (!splitGridPath(s, from, to, path))
			return false;
		return true;
	}
	// trailing points stacked over or under the goal (a walkway above a door) lead to the wrong floor: drop them. Only
	// those near it on the map: a route that climbs a ramp to a door set higher or lower than its origin is kept whole
	// (the Vivec cantons: dropping every point by height alone left the walk going straight at a wall)
	std::vector<int> whole = path;
	while (!path.empty())
	{
		const float* q = w.pathPoints[path.back()].pos;
		float dx = q[0] - to[0], dy = q[1] - to[1];
		if (dx * dx + dy * dy > 200.0f * 200.0f || fabsf(q[2] - to[2]) <= (spot ? 250.0f : 150.0f))
			break;
		path.pop_back();
	}
	if (path.empty())
		path = whole;
	if (spot)
	{
		const float* q = w.pathPoints[path.back()].pos;
		float dx = q[0] - to[0], dy = q[1] - to[1];
		if (dx * dx + dy * dy > 350.0f * 350.0f)
		{
			path.clear();
			return false;
		}
	}
	return true;
}

// Hovering over a floor it can stand on (a levitating player at the foot of a stair, in a hall): collision floor within
// 60 units under the feet. Outdoors the land is not in the cells' collision: no floor found counts as hanging in the air.
bool standsOnFloor(Session& s)
{
	const Player& p = s.w.player;
	Scene sc = s.w.scene();
	for (Cell* c : sc.cells)
	{
		float fz;
		if (collisionFloor(c->collision, p.feet[0], p.feet[1], p.feet[2] + 30.0f, p.feet[2] - 60.0f, &fz))
			return true;
	}
	return false;
}

// The weapon slot holds a lockpick or a probe (USELOCKPICK / USEPROBE equip it, as in OpenMW): a swing with it does nothing.
// A player takes the best carried melee weapon back in hand before a fight. Returns what is in hand afterwards ("" none).
std::string handItem(Session& s, bool takeWeapon)
{
	World& w = s.w;
	int held = -1;
	for (size_t k = 0; k < w.inventory.size(); k++)
		if (w.inventory[k].equipped)
			if (const Object* o = w.game.object(w.inventory[k].id))
				if ((o->recType == REC_WEAP && o->subtype < WEAP_ARROW) || o->recType == REC_LOCK || o->recType == REC_PROB)
					held = (int)k;
	if (held < 0)
		return "";
	const Object* ho = w.game.object(w.inventory[held].id);
	if (takeWeapon && ho && (ho->recType == REC_LOCK || ho->recType == REC_PROB))
	{
		int best = -1;
		float bestDmg = 0.0f;
		for (size_t k = 0; k < w.inventory.size(); k++)
		{
			const Object* o = w.game.object(w.inventory[k].id);
			if (!o || o->recType != REC_WEAP || o->subtype >= WEAP_BOW)
				continue;
			float dmg = (float)std::max(o->chop[1], std::max(o->slash[1], o->thrust[1]));
			if (dmg > bestDmg)
			{
				bestDmg = dmg;
				best = (int)k;
			}
		}
		if (best >= 0)
		{
			std::string was = w.inventory[held].id;
			if (s.equipItem(best))
			{
				logf("drive: %s was in hand: took %s for the fight", was.c_str(), w.inventory[best].id.c_str());
				return w.inventory[best].id;
			}
		}
	}
	return w.inventory[held].id;
}

// Beside a spot that a roof, an arch or a wall top hangs over: the nearest place whose column is clear down to the spot's
// height (nothing over it) and from which the spot can be seen at ground level (not behind a wall from it). A player
// drifts off the edge to it. fromZ is where the search looks down from.
bool spotBeside(Session& s, const float aim[3], float fromZ, float out[2])
{
	Scene sc = s.w.scene();
	// (first with ground clear a body's width all round the spot, not on a ledge's lip; then without)
	for (int pass = 0; pass < 2; pass++)
	for (int k = 0; k < 48; k++)
	{
		float ang = k * 0.3927f, rr = 120.0f + 90.0f * (k / 16);
		float cx = aim[0] + sinf(ang) * rr, cy = aim[1] + cosf(ang) * rr, top = -1e9f;
		bool even = true;
		for (int q = 0; q < (pass == 0 ? 5 : 1) && even; q++)
		{
			static const float kAt[5][2] = { { 0, 0 }, { 50, 0 }, { -50, 0 }, { 0, 50 }, { 0, -50 } };
			float qt = -1e9f;
			for (Cell* c : sc.cells)
			{
				float z;
				if (collisionFloor(c->collision, cx + kAt[q][0], cy + kAt[q][1], fromZ + 50.0f, aim[2] - 300.0f, &z) && z > qt)
					qt = z;
			}
			if (q == 0)
				top = qt;
			even = qt > aim[2] - 200.0f && qt < aim[2] + 150.0f;     // (a door's z is its middle)
		}
		float from[3] = { cx, cy, top + 60.0f }, to[3] = { aim[0], aim[1], aim[2] + 60.0f };
		if (even && s.w.lineOfSight(from, to, false))
		{
			out[0] = cx;
			out[1] = cy;
			return true;
		}
	}
	return false;
}
