// Moving an actor towards a point: steps, ledges, the path grid
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

// A creature that moves up and down as well as along (OpenMW's canActorMoveByZAxis: one that flies, or one that swims
// while it is in water): it goes on its pitch toward its point, nothing drops it to the floor. The highest it goes
// (a swimmer stays 50 under the surface, as a floating corpse), else a huge number
float zMoverTop(World& w, const Ref& r, const Cell& cell)
{
	if (r.type != "CREA")
		return -1e9f;
	u8 f = w.game.actors[r.actor].afloat;
	if (f & 0x20)
		return 1e9f;
	if ((f & 0x10) && cell.hasWater() && r.pos[2] < cell.waterZ)
		return cell.waterZ - 50.0f;
	return -1e9f;
}

// A way a body fits along: rays at both its sides, at knee and chest height, clear for `len` units
static bool bodyWayClear(World& w, const float pos[3], float dx, float dy, float len)
{
	for (float side : { -22.0f, 22.0f })
		for (float h : { 35.0f, 90.0f })
		{
			float a[3] = { pos[0] + dy * side, pos[1] - dx * side, pos[2] + h };
			float b[3] = { a[0] + dx * len, a[1] + dy * len, a[2] };
			if (!w.lineOfSight(a, b))
				return false;
		}
	return true;
}

// Walks toward `target`: straight when nothing is in the way, else along the path grid; steps
// aside when stuck. faceMove turns the actor along its way. False when it can't make progress.
bool Session::npcMoveTo(int ri, const float target[3], float speed, float dt, bool faceMove)
{
	Ref& r = w.refs[ri];
	Cell* cell = w.cellOf(ri);
	if (!cell)
		return false;
	// a creature that flies, or swims in water, climbs or sinks toward the target's height too, at its pace (npcStep
	// keeps it over the floor; a swimmer under the surface)
	float top = zMoverTop(w, r, *cell);
	if (top > -1e8f)
		r.pos[2] += fmaxf(-speed * dt, fminf(speed * dt, fminf(target[2], top) - r.pos[2]));
	r.repathTimer -= dt;
	if (r.repathTimer <= 0.0f)
	{
		r.repathTimer = 0.8f + frand() * 0.4f;
		float a[3] = { r.pos[0], r.pos[1], r.pos[2] + 50.0f }, b[3] = { target[0], target[1], target[2] + 50.0f };
		// straight when nothing is in the way, unless an edge stopped the straight way lately
		bool straight = w.time >= r.gridUntil && w.lineOfSight(a, b);
		if (straight != r.straightWay)
		{
			// A script's AITravel that lost sight of its spot on a straight way that took it down while the spot is above,
			// or up while it is below: the grid's way for a while, as for an edge, not the straight way again once the
			// spot shows (OpenMW's AiTravel keeps to its path from the start). From the top of the stairs on the prison
			// ship the guard saw the upper deck across the stairwell, went straight down the stairs, lost it at the foot,
			// and the grid took him up again, for good
			float rise = target[2] - r.straightZ, went = r.pos[2] - r.straightZ;
			if (!straight && r.aiPackage == AIPKG_TRAVEL && r.ai != AI_COMBAT && fabsf(rise) >= 40.0f && fabsf(went) >= 24.0f
				&& went * rise < 0.0f)
				r.gridUntil = w.time + 15.0f;
			r.straightWay = straight;
			r.straightZ = r.pos[2];
		}
		if (straight)
			r.path.clear();
		else if (!w.findPath(r.pos, target, r.path))
			r.path.clear();
		else
		{
			// a grid route that ends far from the target (the wilds have no grid: the nearest town's points were
			// what it found) leads away: straight then, going round what's in the way as below. Itermerel turned
			// back into Balmora whenever a hill hid the player he followed
			const float* end = w.pathPoints[r.path.back()].pos;
			float ex = end[0] - target[0], ey = end[1] - target[1];
			// (kept when the straight way has failed and the route's last point sees the target: the strider
			// platform at Gnisis ends in a cliff, and the grid's way down is the ramp 2000 units off)
			float e[3] = { end[0], end[1], end[2] + 50.0f };
			if (ex * ex + ey * ey > 600.0f * 600.0f && !(w.time < r.gridUntil && w.lineOfSight(e, b)))
				r.path.clear();
			// outdoors, a route that begins by climbing back up over 200 while the target is below leads away too: the
			// nearest point in sight was up the slope it was coming down. The straight way on, down the ledge that stopped
			// it as OpenMW's actors walk off one. Halfway down the Gnisis cliff a ledge held a following guar, and it
			// turned back up for the strider platform's points and stuck at the foot of the rock face
			else if (!r.path.empty() && !w.cells[w.placeOf(ri)].interior && target[2] < r.pos[2] - 40.0f
				&& w.pathPoints[r.path.front()].pos[2] > r.pos[2] + 200.0f)
			{
				r.path.clear();
				r.dropUntil = w.time + 3.0f;
			}
		}
	}
	float goal[3] = { target[0], target[1], target[2] };
	while (!r.path.empty())
	{
		const PathPoint& p = w.pathPoints[r.path.front()];
		float dx = p.pos[0] - r.pos[0], dy = p.pos[1] - r.pos[1];
		// a waypoint counts as reached close to it: from 50 away the guard on the prison ship cut the
		// corner round the stairwell opening and walked over its edge
		if (dx * dx + dy * dy > 20.0f * 20.0f)
		{
			memcpy(goal, p.pos, sizeof(goal));
			break;
		}
		r.path.erase(r.path.begin());
	}
	float dx = goal[0] - r.pos[0], dy = goal[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist < 1.0f)
		return true;
	dx /= dist;
	dy /= dist;
	// Stuck against something: a while along the way chosen when it got stuck (below)
	if (r.stuckTimer < 0.0f)
	{
		r.stuckTimer += dt;
		dx = r.evadeWay[0];
		dy = r.evadeWay[1];
	}
	if (faceMove)
	{
		float diff = angleDiff(atan2f(dx, dy), r.rot[2]);
		r.rot[2] += fmaxf(-kTurnSpeed * dt, fminf(kTurnSpeed * dt, diff));
	}
	float before[2] = { r.pos[0], r.pos[1] };
	if (!npcStep(ri, *cell, dx, dy, fminf(speed * dt, dist)) && r.path.empty() && w.time >= r.gridUntil)
	{
		// the straight way runs over an edge (a stairwell opening): the path grid goes round it
		r.gridUntil = w.time + 15.0f;
		r.repathTimer = 0.0f;
	}
	float moved = sqrtf((r.pos[0] - before[0]) * (r.pos[0] - before[0]) + (r.pos[1] - before[1]) * (r.pos[1] - before[1]));
	if (r.stuckTimer >= 0.0f)
	{
		// (or, on a path grid route, going back and forth on one spot, a wall's corner pushing it back as far as it
		// steps: no headway for a second counts too, as OpenMW's ObstacleCheck judges the distance over a stretch of
		// time. Leaving Abernanit, Dandsa walked into the rock beside the slope down for good. Off the grid a follower
		// held up takes the player's footsteps instead, and a chaser held up in a fight stands and casts)
		bool held = r.chaseStill >= 1.0f && r.ai != AI_COMBAT && !r.path.empty();
		r.stuckTimer = moved < speed * dt * 0.25f || held ? r.stuckTimer + dt : 0.0f;
		if (r.stuckTimer > 0.5f)
		{
			// stuck again where it was stuck last: look farther round, and keep to the way longer, each time
			float sx = r.pos[0] - r.evadeAt[0], sy = r.pos[1] - r.evadeAt[1];
			r.evadeTries = sx * sx + sy * sy < 150.0f * 150.0f ? std::min(r.evadeTries + 1, 5) : 0;
			r.evadeAt[0] = r.pos[0];
			r.evadeAt[1] = r.pos[1];
			r.stuckTimer = -1.0f - 0.5f * r.evadeTries;
			r.repathTimer = 0.0f;
			// OpenMW goes round with its navigation mesh. Here, with no path grid to go round by (the wilds have
			// none), the opening nearest the goal a body fits through, keeping to the side of the last one: a
			// follower walked into the gap between two rock pillars near Ald'ruhn again and again
			bool found = false;
			for (int k = 1 + r.evadeTries; k <= 6 && !found; k++)
				for (int s = 0; s < 2 && !found; s++)
				{
					float side = s == 0 ? r.evadeSide : -r.evadeSide, c = cosf(k * 0.45f), sn = side * sinf(k * 0.45f);
					float wx = dx * c + dy * sn, wy = dy * c - dx * sn;     // turned toward that side
					if (bodyWayClear(w, r.pos, wx, wy, 200.0f))
					{
						r.evadeWay[0] = wx;
						r.evadeWay[1] = wy;
						r.evadeSide = side;
						found = true;
					}
				}
			// else OpenMW's evasion (obstacle.cpp), a second each way in turn, as (right, forward): right and
			// forward, right, right and back, back, left and back, left, left and forward
			if (!found)
			{
				static const float kEvade[7][2] = { { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 } };
				r.evadeDir = (r.evadeDir + 1) % 7;
				const float* e = kEvade[r.evadeDir];
				float ex = dy * e[0] + dx * e[1], ey = -dx * e[0] + dy * e[1], el = sqrtf(ex * ex + ey * ey);
				r.evadeWay[0] = ex / el;
				r.evadeWay[1] = ey / el;
			}
		}
	}
	return moved > 0.0f;
}
