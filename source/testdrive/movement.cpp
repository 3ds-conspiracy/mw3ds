// The driver's movement: finding the target, walking to it, flying, hopping
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include <optional>

// The reference an action is about: an id, or for DOORTO the nearest door going there (-1: not loaded)
int TestDriver::findTarget(Session& s, const std::string& verb, const std::string& arg)
{
	if (verb != "DOORTO")
	{
		int r = findRef(s, arg);
		return r >= 0 && s.w.active(r) ? r : -1;
	}
	std::string want = lower(spaced(arg));
	// DOORTO:<cell>@<n>: the n-th door going there in the ranking below (0 = the best, as without it; 1 = the next): for a cell
	// with several doors to one place that lead to different parts of it (Therana's Chamber: a closed vestibule, and the hall)
	int nth = 0;
	size_t at = want.rfind('@');
	if (at != std::string::npos && at + 1 < want.size() && isdigit((unsigned char)want[at + 1]))
	{
		nth = atoi(want.c_str() + at + 1);
		want.resize(at);
	}
	bool outside = want == "outside";
	// the door whose destination is exactly that cell first, else one that starts so; of those the one nearest by the
	// route a player would walk (the path grid), else in 3D (a door on another floor is not "near": a stair or a ceiling
	// between). Doors the grid can't reach from here come after the reachable ones.
	struct Cand { int i; bool exact; float d3; };
	std::vector<Cand> cands;
	for (size_t i = 0; i < s.w.refs.size(); i++)
	{
		const Ref& r = s.w.refs[i];
		if (!r.hasDest || !s.w.active((int)i) || !r.visible())
			continue;
		if (r.idLower == "prisonmarker")      // the jail's drop-off mark, no door anyone can use
			continue;
		bool match = outside ? r.destHasGrid || r.destCell.empty() : cellNameMatches(r.destCell, want);
		if (!match)
			continue;
		bool exact = !outside && lower(r.destCell) == want;
		float dx = r.pos[0] - s.w.player.feet[0], dy = r.pos[1] - s.w.player.feet[1], dz = r.pos[2] - s.w.player.feet[2];
		cands.push_back({(int)i, exact, dx * dx + dy * dy + dz * dz});
	}
	int found = -1;
	float best = 1e30f;
	bool bestExact = false;
	std::vector<std::pair<double, int>> ranked;
	for (const Cand& c : cands)
	{
		float d = c.d3;
		if (cands.size() > 1)
		{
			const Ref& r = s.w.refs[c.i];
			d = doorWalkRank(s, r, c.d3);
			// a locked door the player can't open (no key, lock still on) after the doors that open: stacked doors to one
			// place are often a locked upper door and the open one below
			if (r.lockLevel > 0 && (r.key.empty() || s.w.itemCount(r.key) <= 0))
				d += 1e13f;
			// the character generation doors are shut by their scripts until the intro is done: the ordinary doors first
			if (r.idLower.compare(0, 7, "chargen") == 0)
				d += 1e13f;
			logf("drive: DOORTO candidate %s (%d) at %.0f %.0f %.0f: 3D %.0f, ranked %.0f, lock %d%s", r.id.c_str(), c.i, r.pos[0], r.pos[1],
				r.pos[2], sqrtf(c.d3), sqrtf(d), r.lockLevel, r.trap.empty() ? "" : ", trapped");
		}
		ranked.push_back({ (c.exact ? 0.0 : 1e14) + d, c.i });
		if ((c.exact && !bestExact) || (c.exact == bestExact && d < best))
		{
			best = d;
			bestExact = c.exact;
			found = c.i;
		}
	}
	if (nth > 0 && !ranked.empty())
	{
		std::sort(ranked.begin(), ranked.end());
		found = ranked[std::min((size_t)nth, ranked.size() - 1)].second;
	}
	if (found >= 0)
		id = s.w.refs[found].id + " -> " + (s.w.refs[found].destCell.empty() ? "outside" : s.w.refs[found].destCell);
	return found;
}

// FLYTO: levitating, as a player flies: up (R) to the cruising height, straight at the spot, then down (L). A wall in
// the way: climb higher (three times at most). Levitation running out on the way: a fall, and the leg fails.
void TestDriver::keepFloating(Session& s)
{
	if (hideUntil >= 0.0f && s.kitV2() && s.w.itemCount("uber_hide") > 0)
	{
		if (s.w.time >= hideUntil)
		{
			hideUntil = -1.0f;
			logf("drive: hide over");
		}
		else
		{
			float left = 0.0f;
			for (const ActiveEffect& e : s.w.effects)
				if (e.effect == 40 && e.source == "uber hide")
					left = fmaxf(left, e.remaining);
			if (left < 0.5f)
				s.castItem("uber_hide");
		}
	}
	if (floatUntil < 0.0f || !s.kitV2() || s.w.itemCount("uber_fly") <= 0)
		return;
	// (through a door, a float ends: it was for the way to it, the potions' 60 s in v1; levitating on in a villa the walk
	// pushed against a trapped door for good, Dren's)
	if (s.w.time >= floatUntil || (floatWalk && kind == NONE) || (!floatWalk && floatCell != -3 && floatPlace(s.w) != floatCell))
	{
		floatUntil = -1.0f;
		floatWalk = false;
		logf("drive: float over");
		return;
	}
	float left = 0.0f;
	for (const ActiveEffect& e : s.w.effects)
		if (e.effect == 10)
			left = fmaxf(left, e.remaining);
	if (left < 0.5f)
		s.castItem("uber_fly");
}

bool TestDriver::updateFly(Session& s, PlayerInput& in, float dt)
{
	Player& p = s.w.player;
	float dx = goal[0] - p.feet[0], dy = goal[1] - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	// v2 kit: the fly item, used again whenever less than half a second of its levitation is left (a use starts it afresh)
	bool flyItem = s.kitV2() && s.w.itemCount("uber_fly") > 0;
	if (flyItem && !(phase == 2 && p.onGround))
	{
		float left = 0.0f;
		for (const ActiveEffect& e : s.w.effects)
			if (e.effect == 10)
				left = fmaxf(left, e.remaining);
		if (left < 0.5f)
		{
			s.castItem("uber_fly");
			if (p.levitate <= 0.0f)
				return true;           // (the effect is on from the next frame)
		}
	}
	if (p.levitate <= 0.0f)
	{
		if (phase == 2 && p.onGround)
		{
			logf("drive: flew to %s in %.1f s (%.0f off)", id.c_str(), elapsed, dist);
			kind = NONE;
			return false;
		}
		// the levitation cast the step before (the v2 kit's fly spell) lands at its animation's release: wait for it
		if (!flyBegun && elapsed < 4.0f && (s.castPending || s.castBusy()))
			return true;
		fail("not levitating (or it ran out on the way)");
		return false;
	}
	p.pitch = 0.0f;
	// (a fly cast still on its way, over what is left of the last one: it lands first, then the time left is counted)
	if (!flyBegun && elapsed < 4.0f && (s.castPending || s.castBusy()))
		return true;
	if (!flyBegun)
	{
		// (the time allowed, again now the levitation is on: start computed it from a fly speed without it)
		flyBegun = true;
		takeoffZ = p.feet[2];
		flyAllow = ((hypotf(goal[0] - p.feet[0], goal[1] - p.feet[1]) + 2.0f * fmaxf(0.0f, cruise - p.feet[2])) / fmaxf(1.0f, p.flySpeed)) * 1.35f + 30.0f;
		float left = 0.0f;
		for (const ActiveEffect& e : s.w.effects)
			if (e.effect == 10)
				left = fmaxf(left, e.remaining);
		float need = (dist + fmaxf(0.0f, cruise - p.feet[2]) + fmaxf(0.0f, cruise - goal[2])) / fmaxf(1.0f, p.flySpeed);
		if (need > left && !flyItem)
		{
			char why[120];
			snprintf(why, sizeof(why), "levitation runs out first (%.0f s left, the flight takes %.0f s)", left, need);
			fail(why);
			return false;
		}
	}
	phaseTimer += dt;
	progressTimer += dt;
	if (phase == 0)
	{
		// up to the cruising height (or down to it: inside, a blocked flight tries lower under the roof before higher)
		bool lowering = lowFly && cruise < p.feet[2] - 20.0f;
		in.up = !lowering;
		in.down = lowering;
		// v2 kit: the first 400 up at v1's 500 a second (up pressed a share of the frames): at the kit's 3000 a second the
		// first frame's 98 units put the head into an eave or an arch over the start and it stayed there (Balmora)
		if (s.kitV2() && in.up && p.feet[2] < takeoffZ + 400.0f && p.flySpeed > 500.0f)
		{
			vertDuty += 500.0f / p.flySpeed;
			if (vertDuty >= 1.0f)
				vertDuty -= 1.0f;
			else
				in.up = false;
		}
		// out from under an overhang (a bridge, an eave): a stretch sideways, a different way each time
		if (outTimer > 0.0f)
		{
			outTimer -= dt;
			// (v2 kit: its first try is the open sky below, an extra one: v1's four ways round follow it as they were)
			p.yaw = atan2f(dx, dy) + (s.kitV2() ? outTries - 1 : outTries) * 1.5708f;
			// (v2 kit: to the nearest open sky found, not a guessed way round; there, the rise is tried again)
			if (outAimed)
			{
				float ox = outGoal[0] - p.feet[0], oy = outGoal[1] - p.feet[1];
				p.yaw = atan2f(ox, oy);
				if (ox * ox + oy * oy < 40.0f * 40.0f)
					outTimer = 0.0f;
			}
			// (v2 kit: the stretch at v1's 500 a second: at the kit's fly speed it went six times as far, into the next wall)
			in.moveY = s.kitV2() ? fminf(1.0f, 500.0f / fmaxf(1.0f, p.flySpeed)) : 1.0f;
		}
		if (lowering ? p.feet[2] <= cruise + 20.0f : p.feet[2] >= cruise)
		{
			logf("drive: FLYTO %s: at %.0f up after %.1f s, %.0f to go", id.c_str(), p.feet[2], elapsed, dist);
			phase = 1;
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		else if (lowering ? p.feet[2] <= progressAt[2] - 20.0f : p.feet[2] >= progressAt[2] + 20.0f)
		{
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		else if (progressTimer >= 1.5f && outTimer <= 0.0f)
		{
			// no rise at all in 1.5 s (a flight climbs 200 a second): a ceiling over us. Step out from under it (toward the goal, then the other ways round);
			// four ways tried without a rise is a roof the flight can't get out from under
			if (++outTries > (s.kitV2() ? 5 : 4))
			{
				fail("can't climb (a ceiling): fly out of the building first");
				return false;
			}
			logf("drive: FLYTO %s: no rise at %.0f %.0f %.0f, stepping out sideways (way %d)", id.c_str(), p.feet[0], p.feet[1], p.feet[2], outTries);
			outAimed = false;
			if (s.kitV2() && outTries == 1)   // (first: the nearest open sky; then v1's ways round, 2000 units)
			{
				float spot[2];
				if (spotBeside(s, p.feet, p.feet[2] + 3000.0f, spot))
				{
					outGoal[0] = spot[0];
					outGoal[1] = spot[1];
					outAimed = true;
					logf("drive: FLYTO %s: open sky at %.0f %.0f, %.0f off", id.c_str(), spot[0], spot[1], hypotf(spot[0] - p.feet[0], spot[1] - p.feet[1]));
				}
			}
			outTimer = s.kitV2() ? 4.0f : 2.5f;   // (v2: the stretch is at v1's 500 a second: v1's 4 s, 2000 units, to get clear)
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
		}
		return true;
	}
	if (phase == 1)
	{
		// straight at it, at height (the last stretch slows down so a frame's step doesn't overshoot)
		p.yaw = atan2f(dx, dy);
		float step = p.flySpeed * p.loadSpeed * dt;
		in.moveY = dist > step * 2.0f ? 1.0f : fmaxf(0.05f, dist / fmaxf(1.0f, step * 2.0f));
		if (dist < 40.0f)
		{
			logf("drive: FLYTO %s: over it after %.1f s, coming down from %.0f", id.c_str(), elapsed, p.feet[2]);
			phase = 2;
			phaseTimer = progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		if (progressTimer >= 2.0f)
		{
			bool closer = dist < lastGoal - 40.0f;
			lastGoal = dist;
			progressTimer = 0.0f;
			if (!closer)
			{
				// (v2 kit: 10: the count is shared with the ways down beside the spot, and the kit's lower first climb meets more
				// ridges on the way: two on the way to Bthungthumz, then one beside it; four across the island to the Arenim tomb)
				if (++climbs > (s.kitV2() ? 10 : 3))
				{
					fail("blocked in the air, even higher up");
					return false;
				}
				// a wall or a tower: go over it; FLYTO ...:low (a cave), with the spot below, under it first (every other block, up)
				cruise = lowFly ? (goal[2] < p.feet[2] - 100.0f && climbs % 2 == 1 ? fmaxf(goal[2] + 80.0f, p.feet[2] - 250.0f) : p.feet[2] + 250.0f) : p.feet[2] + 800.0f;
				phase = 0;
				logf("drive: FLYTO %s: blocked at %.0f %.0f %.0f, climbing to %.0f", id.c_str(), p.feet[0], p.feet[1], p.feet[2],
					cruise);
			}
		}
		return true;
	}
	// down to the spot's height (v2 kit: held a while first when something stopped us, below)
	if (underWait > elapsed)
	{
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	in.down = true;
	bool stopped = progressTimer >= 1.0f && p.feet[2] > progressAt[2] - 20.0f;     // (not coming down any more)
	// (within 250: the ground, uneven; higher: a roof. v2 kit: within 200 (a canton roof 180 over its spot is landed on): a shrine's mound or a roof's edge 241 over the
	// spot passed for the ground, and the walk to the door below went over its top: Redas Ancestral Tomb)
	if (stopped && p.feet[2] > goal[2] + (s.kitV2() ? 200.0f : 250.0f) && climbs < 12)
	{
		// v2 kit: the first time, hold here 6 s and come down again: it may be moving out of the way (Holamayan's rock cover
		// turns open over 5 s once its cell is in, and the kit's flight got there first; v1's slower one found it open)
		if (s.kitV2() && !underWaited)
		{
			underWaited = true;
			underWait = elapsed + 6.0f;
			logf("drive: FLYTO %s: something under us at %.0f (%.0f over the spot): holding 6 s in case it moves", id.c_str(), p.feet[2],
				p.feet[2] - goal[2]);
			return true;
		}
		// a roof, an arch or a wall top under us: come down beside it, as a player would drift off its edge
		climbs++;
		// beside it: the nearest spot whose column is clear down to the spot's height (no roof over it) and from which
		// the spot can be seen at ground level (not behind a wall from it)
		bool found = false;
		Scene sc = s.w.scene();
		float spot[2];
		if (spotBeside(s, aim, p.feet[2], spot))
		{
			goal[0] = spot[0];
			goal[1] = spot[1];
			found = true;
		}
		if (!found)
		{
			// a street under a big roof (the Vivec canton doors: a plaza 450 over them, stairs and a sloped roof between): the
			// roof is wider than the ring above. A player flies off its edge to where the street opens and walks in, so make
			// for the nearest path grid point of the street whose column is clear down to it (its open end)
			float bestD = 2000.0f * 2000.0f;
			for (const PathPoint& pp : s.w.pathPoints)
			{
				float ex = pp.pos[0] - aim[0], ey = pp.pos[1] - aim[1], d = ex * ex + ey * ey;
				if (d >= bestD || fabsf(pp.pos[2] - aim[2]) > 400.0f)
					continue;
				float top = -1e9f;
				for (Cell* c : sc.cells)
				{
					float z;
					if (collisionFloor(c->collision, pp.pos[0], pp.pos[1], p.feet[2] + 50.0f, pp.pos[2] - 300.0f, &z) && z > top)
						top = z;
				}
				if (top > pp.pos[2] - 150.0f && top < pp.pos[2] + 150.0f)
				{
					bestD = d;
					goal[0] = pp.pos[0];
					goal[1] = pp.pos[1];
					found = true;
				}
			}
		}
		// (the spot beside it is where we already hang: trying it again only waits out the tries, a second at a time)
		if (found && hypotf(goal[0] - p.feet[0], goal[1] - p.feet[1]) < 60.0f)
			found = false;
		if (!found)
		{
			// (a covered street: stop here, still levitating; the walk that follows comes down the way, L held)
			logf("drive: FLYTO %s: no clear spot to come down beside it: stopping at %.0f %.0f %.0f", id.c_str(), p.feet[0], p.feet[1],
				p.feet[2]);
			kind = NONE;
			return false;
		}
		cruise = p.feet[2];
		phase = 1;
		progressTimer = 0.0f;
		lastGoal = 1e9f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		logf("drive: FLYTO %s: something under us at %.0f (%.0f over the spot), trying beside it", id.c_str(), p.feet[2], p.feet[2] - goal[2]);
		return true;
	}
	if (p.feet[2] <= goal[2] + 5.0f || stopped)
	{
		logf("drive: flew to %s in %.1f s (%.0f off, %.0f up)", id.c_str(), elapsed, dist, p.feet[2] - goal[2]);
		kind = NONE;
		return false;
	}
	if (progressTimer >= 1.0f)
	{
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
	}
	return true;
}

// HOPTO: the speedrunners' way (Fortify Speed, Jump, run and jump): face the spot, run, jump; in the air steer toward
// it while the take-off would fall short, back against it while it would carry past. Landed within 200: done, else jump
// again (40 jumps at most).
bool TestDriver::updateHop(Session& s, PlayerInput& in, float dt)
{
	Player& p = s.w.player;
	float dx = goal[0] - p.feet[0], dy = goal[1] - p.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	p.pitch = 0.0f;
	p.yaw = atan2f(dx, dy);
	phaseTimer += dt;
	if (p.onGround || p.swimming)
	{
		if (dist < 200.0f && phaseTimer > 0.2f)
		{
			logf("drive: hopped to %s in %.1f s, %d jumps (%.0f off)", id.c_str(), elapsed, hops, dist);
			kind = NONE;
			return false;
		}
		if (hops >= 40)
		{
			fail("40 jumps and not there");
			return false;
		}
		// each landing: closer by 50 at least, else something is in the way (three times: give up, the route is wrong)
		if (inAir)
		{
			inAir = false;
			if (dist > lastGoal - 50.0f && ++climbs >= 3)
			{
				fail("blocked: three jumps without getting closer");
				return false;
			}
			lastGoal = fminf(lastGoal, dist);
		}
		// a short run-up, then the jump (a running take-off: 45 degrees, OpenMW), unless one jump would carry past it
		// (Icarian Flight's jump goes cells): then run the rest, as a player would
		in.moveY = 1.0f;
		// (in the air the pad can hold back airControl of the run speed for the whole flight, 2 v / g)
		float v = p.jumpSpeed * 0.707f, range = 2.0f * v * v / 627.0f;
		float brake = p.runSpeed * p.loadSpeed * p.airControl * 2.0f * v / 627.0f;
		// (running the rest: a run that stalls half a second, a rock in the way, gets a jump after all)
		progressTimer += dt;
		if (progressTimer >= 0.5f)
		{
			bool stalled = lastGoal - dist < 20.0f;
			lastGoal = dist;
			progressTimer = 0.0f;
			if (stalled && p.onGround)
			{
				logf("drive: hop %d toward %s: the run stalled, jumping (range %.0f, %.0f to go)", hops + 1, id.c_str(), range, dist);
				in.jump = true;
				hops++;
				phaseTimer = 0.0f;
				return true;
			}
		}
		if (range - brake > dist + 300.0f)
			return true;
		if (phaseTimer > 0.15f && p.onGround)
		{
			logf("drive: hop %d toward %s: jump speed %.0f (Acrobatics %d), range %.0f, %.0f to go", hops + 1, id.c_str(), p.jumpSpeed, s.w.stats.skills[20], range, dist);
			in.jump = true;
			hops++;
			phaseTimer = 0.0f;
		}
		return true;
	}
	inAir = true;
	// in the air: where would the take-off's own speed set us down? (gravity 627, from this height to the spot's)
	float h = fmaxf(0.0f, p.feet[2] - goal[2]);
	float tLeft = (p.vz + sqrtf(fmaxf(0.0f, p.vz * p.vz + 2.0f * 627.0f * h))) / 627.0f;
	float carry = sqrtf(p.inertia[0] * p.inertia[0] + p.inertia[1] * p.inertia[1]) * tLeft;
	in.moveY = carry > dist + 50.0f ? -1.0f : carry < dist - 50.0f ? 1.0f : 0.0f;
	return true;
}

// The walk towards the step's goal while it is still far (or on another floor): ends the frame's update
bool TestDriver::walkStep(Session& s, PlayerInput& in, u32& down, float dt, Ref& r, Player& p, float& tx, float& ty, float& dx, float& dy, float& dist, float& want, float& dz)
{
	World& w = s.w;
	// hovering right over it (a flight's potion not run out, the FLYTO ended above it): down (L) on the spot. The grid's
	// route would lead away to the floor it is on, off the plateau, and the walk out of it would time out
	if (p.levitate > 0.0f && !p.onGround && !pointGoal && dist <= 220.0f && dz < -60.0f && elapsed < 60.0f)
	{
		// a ledge or a roof under us holds us up: come down beside it, as the FLYTO does (the spot found after 1.5 s without
		// getting lower), else straight at it
		hoverTimer += dt;
		// v2 kit: the float (FLOAT after the flight) off, and down on the spot as it runs out: held up by it over a canal
		// door below, the descent stopped at a ledge and spotBeside found nothing (St. Delyn, Canal South-Two)
		if (s.kitV2() && floatUntil >= 0.0f && !floatWalk)
		{
			floatUntil = -1.0f;
			logf("drive: over %s, %.0f above it: float off", id.c_str(), -dz);
		}
		if (hoverTimer > 1.5f)
		{
			float at[3] = { tx, ty, r.pos[2] };
			if (!hoverOn && p.feet[2] > hoverZ - 10.0f && spotBeside(s, at, p.feet[2], hoverSpot))
			{
				hoverOn = true;
				logf("drive: over %s on something at %.0f: coming down beside it at %.0f %.0f", id.c_str(), p.feet[2], hoverSpot[0], hoverSpot[1]);
			}
			hoverZ = p.feet[2];
			hoverTimer = 0.0f;
		}
		float hx = (hoverOn ? hoverSpot[0] : tx) - p.feet[0], hy = (hoverOn ? hoverSpot[1] : ty) - p.feet[1];
		in.down = true;
		p.yaw = atan2f(hx, hy);
		p.pitch = 0.0f;
		in.moveY = hypotf(hx, hy) > (hoverOn ? 10.0f : 40.0f) ? 0.6f : 0.0f;
		path.clear();          // (planned from up here: replanned from the floor)
		repathTimer = 3.1f;
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	// an escort fallen behind: stand and let them catch up, as a player waits (not counted as being stuck)
	int e = -1;
	// (going back for one that stopped: right up to them, inside the 640 a follower starts at, not just under 600)
	bool fetching = escortWait - escortSince > 8.0f;
	// (v2 kit: kept closer, 350, the follower following our steps rather than finding its own way down a ledge)
	float lagAt = s.kitV2() ? (fetching ? 200.0f : 350.0f) : (fetching ? 300.0f : 600.0f);
	// (v2: one that stopped coming within 600, at the foot of a stair, is left to catch up as in v1: waiting on held
	// the walk there for good, Varvur Sarethi)
	if (s.kitV2() && fetching && escortLagging(s, escort, 0.0f, &e) && w.distanceToPlayer(e) < 600.0f)
		escortLoose = true;
	if (escortLoose && escortLagging(s, escort, 0.0f, &e) && w.distanceToPlayer(e) < 250.0f)
		escortLoose = false;
	// (1500: held within 600 of one stopped at a ledge's edge, our footsteps over the ledge stayed fresh and they kept to
	// them, never taking the drop; on further, the steps run out and they come straight on: Tarvyn Faren)
	if (escortLoose)
		lagAt = 1500.0f;
	if ((kind == WALK || kind == ACTIVATE) && escortLagging(s, escort, lagAt, &e))
	{
		if ((int)(elapsed / 5.0f) != (int)((elapsed - dt) / 5.0f))
			logf("drive: waiting for %s (%.0f away, at %.0f %.0f %.0f, ai %d)", escort.c_str(), w.distanceToPlayer(e), w.refs[e].pos[0],
				w.refs[e].pos[1], w.refs[e].pos[2], (int)w.refs[e].ai);
		// not coming (no 50 nearer in 8 s: their own route to us goes the long way round): go back for them, as a
		// player fetches a lost escort, until they are close enough to follow again
		float ed = w.distanceToPlayer(e);
		escortWait += dt;
		if (ed < escortBest - 50.0f)
		{
			escortBest = ed;
			escortSince = escortWait;
		}
		if (escortWait - escortSince > 8.0f)
		{
			if (escortWait - escortSince - dt <= 8.0f)
				logf("drive: %s not coming (%.0f away): going back for them", escort.c_str(), ed);
			p.yaw = atan2f(w.refs[e].pos[0] - p.feet[0], w.refs[e].pos[1] - p.feet[1]);
			p.pitch = 0.0f;
			in.moveY = 1.0f;
		}
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	escortWait = escortSince = 0.0f;
	escortBest = 1e9f;
	float floodGoal[3] = { pointGoal ? goal[0] : tx, pointGoal ? goal[1] : ty, pointGoal ? goal[2] : r.pos[2] };
	// an attacker in the way (rats in a corridor, a guard at a door): face it and fight, as a player clears the way,
	// until it is dead, gone, or 20 s have passed; then on with the walk
	if (fightRef >= 0)
	{
		const Ref& f = w.refs[fightRef];
		float fx = f.pos[0] - p.feet[0], fy = f.pos[1] - p.feet[1], fd = hypotf(fx, fy);
		fightTimer += dt;
		// v2 kit: someone peaceful the blow would reach (an escort stepping in front): the blow held wound up, not let go,
		// stepping aside for a clear swing as a player would. Sinnammu Mirpal died to a blow meant for a dreugh
		// (the blow lands on the nearest one in reach, OpenMW's getHitContact: Session::swingTarget. Any peaceful one in
		// reach held it, and with a dreugh fought from its own longer reach Sinnammu Mirpal, behind it, held every blow)
		auto friendInReach = [&]() {
			if (!s.kitV2())
				return false;
			int t = s.swingTarget();
			return t >= 0 && t != fightRef && w.refs[t].ai != AI_COMBAT;
		};
		bool fightOver = f.dead || !f.visible() || f.ai != AI_COMBAT || f.duelFoe >= 0 || fd > 600.0f || fightTimer > 20.0f;
		if (fightOver && fightHold && friendInReach())
		{
			// over with the blow still held: turned away from them before it goes
			p.yaw += 3.14159f;
			in.attack = true;
			return true;
		}
		if (fightOver)
		{
			fightHold = false;
			logf("drive: %s %s after %.1f s: on to %s", f.id.c_str(), f.dead ? "dead" : "no longer in the way", fightTimer, id.c_str());
			fightRef = -1;
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		handItem(s, true);
		p.yaw = atan2f(fx, fy);
		p.pitch = atan2f((f.boxMin[2] + f.boxMax[2]) * 0.5f - playerEyeZ(p), fmaxf(fd, 1.0f));
		// (in the water one above or below us out of the blow's reach up or down, OpenMW's isInMeleeReach: swum to
		// along the pitch, as a player dives after a fish. It stood 90 off and swung at a slaughterfish 150 under it)
		bool deep = p.swimming && fabsf(f.pos[2] - p.feet[2]) > 90.0f;
		in.moveY = fd > 90.0f || deep ? 1.0f : 0.0f;
		in.attack = fmodf(fightTimer, 0.7f) < 0.4f;
		// (judged facing the way just set: the blow goes from there)
		if (friendInReach())
		{
			in.attack = fightHold;      // held stays held (let go now it would reach them), unpressed stays so
			in.moveX = 1.0f;
		}
		fightHold = in.attack;
		return true;
	}
	// a route by trial steps (a dive, a run of jumps): leg by leg
	if (!swimRoute.empty())
	{
		// each step as the trial made it: its heading held for its time (a jump on the first frame), then the next
		if (legTimer >= swimRoute.front().seconds)
		{
			swimRoute.erase(swimRoute.begin());
			legTimer = 0.0f;
			legJumped = false;
			if (swimRoute.empty())
			{
				logf("drive: route by trial steps ended at %.0f %.0f %.0f", p.feet[0], p.feet[1], p.feet[2]);
				stuckTries = 0;
				// (made for the next path point: at it, on along the same route)
				if (!floodKeep)
				{
					path.clear();
					repathTimer = 3.1f;
				}
				floodKeep = false;
				// at the goal's piece of a split grid: along it from its point there, not a new route from scratch (that
				// snaps to the start's piece again when a ledge of it is nearer, and walks back to the dead end)
				if (deadEntry >= 0)
				{
					std::vector<int> tail;
					if (!s.w.findPath(s.w.pathPoints[deadEntry].pos, deadGoal, tail))
						tail.clear();
					path.push_back(deadEntry);
					for (int t : tail)
						if (t != deadEntry)
							path.push_back(t);
					logf("drive: along the goal's piece from point %d, %d path points", deadEntry, (int)path.size());
					deadEntry = -1;
					repathTimer = 0.0f;
				}
			}
			progressTimer = 0.0f;
			memcpy(progressAt, p.feet, sizeof(progressAt));
			return true;
		}
		legTimer += dt;
		p.yaw = swimRoute.front().yaw;
		p.pitch = swimRoute.front().pitch;
		in.moveY = 1.0f;
		if (swimRoute.front().jump && !legJumped && p.onGround)
		{
			in.jump = true;
			legJumped = true;
		}
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	// no route yet (the cells ahead were still loading): try again now and then
	repathTimer += dt;
	// v2 kit: far off with no route yet (the cells on the way still coming in): plan again every half second and stand
	// meanwhile, up to 6 s. A run straight at it at the kit's speed hit the first wall at once and the stuck jumps
	// threw it off the platform it started on (Wolverine Hall, the walk to Gals Arethi)
	bool waitRoute = s.kitV2() && path.empty() && dist > 3000.0f && elapsed < 6.0f;
	if (path.empty() && repathTimer > (waitRoute ? 0.5f : 3.0f) && !(floatWalk && floatUntil >= 0.0f))
	{
		repathTimer = 0.0f;
		float to[3];
		walkTarget(r, kind == ACTIVATE && !pointGoal, to);
		if (planPath(s, p.feet, to, pointGoal, path, ownPiece))
			logf("drive: route to %s found, %d path points", id.c_str(), (int)path.size());
	}
	if (waitRoute && path.empty())
	{
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
		return true;
	}
	// the next path point on the way (passed ones dropped), else straight at it
	float gx = tx, gy = ty, gz = r.pos[2];
	while (!path.empty())
	{
		const PathPoint& pp = w.pathPoints[path.front()];
		float px = pp.pos[0] - p.feet[0], py = pp.pos[1] - p.feet[1];
		if (px * px + py * py > 60.0f * 60.0f)
		{
			gx = pp.pos[0];
			gy = pp.pos[1];
			gz = pp.pos[2];
			break;
		}
		path.erase(path.begin());
	}
	p.yaw = atan2f(gx - p.feet[0], gy - p.feet[1]);
	p.pitch = 0.0f;
	in.moveY = 1.0f;
	// v2 kit, an escort along: at v1's pace (a third of the stick: the ring's Speed triples the run), or the follower,
	// who starts only on seeing us within 640, never does, and one that has falls behind at every step
	if (s.kitV2() && !escort.empty())
		in.moveY = 0.33f;
	// v2 kit, levitating on a walk (a float, the last second of a flight): at v1's pace too, 550 a second, not the
	// ring's 1500-3000 a second fly speed, which flew off the route of a mine's walk (Elith-Pal, to Dangor)
	if (s.kitV2() && p.levitate > 0.0f && p.flySpeed > 550.0f)
		in.moveY = fminf(in.moveY, 550.0f / p.flySpeed);
	// v2 kit (Speed fortified): a frame's step would carry past the next point and back; the last stretch slows down,
	// as the fly leg's does (updateFly phase 1)
	if (s.kitV2())
	{
		float perSec = fmaxf(p.runSpeed, p.levitate > 0.0f ? p.flySpeed : 0.0f) * p.loadSpeed, step = perSec * dt;
		float toNext = hypotf(gx - p.feet[0], gy - p.feet[1]);
		if (toNext < step * 2.0f)
			in.moveY = fmaxf(0.2f, toNext / fmaxf(1.0f, step * 2.0f));
	}
	// swimming: the stroke follows the view (a flooded tunnel, a sunken door): look down or up to the way's depth
	if (p.swimming && p.levitate <= 0.0f && fabsf(gz - p.feet[2]) > 60.0f)
	{
		float hd = hypotf(gx - p.feet[0], gy - p.feet[1]);
		p.pitch = fmaxf(-0.9f, fminf(0.9f, atan2f(gz - p.feet[2], fmaxf(hd, 1.0f))));
	}
	// still levitating (a flight's potion not run out): down (L) or up (R) to the way's height, as a player would
	if (p.levitate > 0.0f)
	{
		in.down = gz < p.feet[2] - 30.0f;
		in.up = gz > p.feet[2] + 30.0f;
		// v2 kit: up and down at v1's pace too (the buttons pressed a share of the frames, 550 a second of the fly speed's
		// 1500-3000: at full speed each press carried 50-100 units past the way's height, into roofs and floors)
		if (s.kitV2() && p.flySpeed > 550.0f && (in.up || in.down))
		{
			vertDuty += 550.0f / p.flySpeed;
			if (vertDuty >= 1.0f)
				vertDuty -= 1.0f;
			else
				in.up = in.down = false;
		}
		// the way is at or below us and we hang in the air: come down first (L), or the walk climbs onto roofs
		if (in.down && !p.onGround && elapsed < 15.0f && !standsOnFloor(s))
			in.moveY = 0.0f;
		// close to a door (a tower's or a temple's) and still hovering: land at its floor first, as a player does before the
		// last steps, or the walk stops short of it and the crosshair passes over its top (Ghostgate, Vivec)
		if (kind == ACTIVATE && !pointGoal && r.hasDest && dist < 500.0f && !p.onGround && p.feet[2] > r.pos[2] - 60.0f)
		{
			in.down = true;
			in.up = false;
			if (dist < 300.0f)
				in.moveY = 0.0f;
		}
	}
	if (backTimer > 0.0f)
	{
		backTimer -= dt;
		in.moveX = 0.0f;
		in.moveY = -1.0f;
	}
	if (sidestepTimer > 0.0f)
	{
		sidestepTimer -= dt;
		in.moveX = sidestep;
		in.moveY = 0.3f;
	}
	// MW3DS_DRIVE_TRACE=1 (native runs): where a walk is, once a second, to find where it dawdles
	static const bool trace = getenv("MW3DS_DRIVE_TRACE") != nullptr;
	static float traceTimer = 0.0f;
	if (trace && (traceTimer += dt) >= 1.0f)
	{
		traceTimer = 0.0f;
		logf("drive: trace %s at %.0f %.0f %.0f, %d path points, next %.0f %.0f, stuck %d", id.c_str(), p.feet[0], p.feet[1], p.feet[2],
			(int)path.size(), gx, gy, stuckTries);
	}
	// no headway for 2 s: jump once, then warp beside it (logged: the run still fails on it)
	// (off the route: no closer to it either, e.g. shuttling between the last two grid points)
	progressTimer += dt;
	if (progressTimer >= 2.0f)
	{
		float mx = p.feet[0] - progressAt[0], my = p.feet[1] - progressAt[1];
		float goal = sqrtf(dist * dist + dz * dz);
		bool noCloser = path.empty() && goal > lastGoal - 40.0f;
		lastGoal = goal;
		if (mx * mx + my * my < 40.0f * 40.0f || noCloser)
		{
			// a door in the way (inner doors swing shut): open it, as a player would
			int door = -1;
			float bd = 250.0f * 250.0f;
			for (int i : w.loadedDoors)
			{
				const Ref& d = w.refs[i];
				if (d.recType != REC_DOOR || d.hasDest || !d.visible())
					continue;
				// (v2 kit: not one open already, or opening: a press shut it again, the East Empire Company's back room)
				if (s.kitV2() && (d.doorTarget != 0.0f || d.doorAngle != 0.0f))
					continue;
				float ex = d.pos[0] - p.feet[0], ey = d.pos[1] - p.feet[1];
				if (ex * ex + ey * ey < bd && fabsf(d.pos[2] - p.feet[2]) < 200.0f)
				{
					bd = ex * ex + ey * ey;
					door = i;
				}
			}
			float entry[3];
			// v2 kit: no trial steps toward a way on over 1500 off (the search runs to its cap there and finds nothing;
			// the grid's route turns up once the cells are in)
			auto trialOk = [&](const float* t) {
				float far = hypotf(t[0] - p.feet[0], t[1] - p.feet[1]);
				if (!s.kitV2() || far <= 1500.0f)
					return true;
				logf("drive: stuck at %.0f %.0f %.0f going to %s: the way on is %.0f away, no trial steps (v2)", p.feet[0], p.feet[1],
					p.feet[2], id.c_str(), far);
				return false;
			};
			// LEGIT: an attacker in the way (fighting us, not another actor; close, at our height): fight it first
			int attacker = -1;
			if (s.testLegit && s.kitV2())     // (a case on the old kit walks on past one who attacks, as before)
			{
				float ad = 200.0f;
				for (int i : w.loadedActors)
				{
					const Ref& a = w.refs[i];
					float ex = a.pos[0] - p.feet[0], ey = a.pos[1] - p.feet[1], e = hypotf(ex, ey);
					// (not the one we walk to: a talk or an escort goes on as asked)
					if (i != ref && a.visible() && !a.dead && !a.ally && a.ai == AI_COMBAT && a.duelFoe < 0 && e < ad && fabsf(a.pos[2] - p.feet[2]) < 150.0f)
					{
						ad = e;
						attacker = i;
					}
				}
			}
			// (each inner door once: a walk may have several in a row; one whose trap ate the press, again)
			// v2 kit: held up within 80 of a spot to walk to (a rock between it and us, a waypoint of a planned route):
			// close enough, as a player calls it there
			// (with an activation queued only on a walk on foot round to another side of it: the spot is one of many round it;
			// floating, the spot up on a bed is the one to look over it from, Caryarel's barrel)
			if (s.kitV2() && kind == WALK && pointGoal && (thenDo.empty() || (viewTries > 0 && p.levitate <= 0.0f))
				&& sqrtf(dist * dist + dz * dz) < 80.0f)
			{
				logf("drive: reached %s in %.1f s (close enough: %.0f off, held up)", id.c_str(), elapsed, sqrtf(dist * dist + dz * dz));
				kind = NONE;
				if (!thenDo.empty())
				{
					// (a walk round to another side of what it was to activate: on with that, as an arrival does)
					std::string t = thenDo;
					thenDo.clear();
					retrying = true;
					start(s, t);
					retrying = false;
					return kind != NONE;
				}
				return false;
			}
			// v2 kit: held up hovering above it on the case's float (a railing over the door a floor down, in Aryon's chambers):
			// the float off, down to the floor, on foot from there
			// (indoors only, a floor down and near: outdoors the float carried the walk over the hill to Arvs-Drelen's door,
			// Gnisis; and Senise Thindo, 530 down a Tel Aruhn shaft, is reached floating)
			if (s.kitV2() && floatUntil >= 0.0f && !floatWalk && p.levitate > 0.0f && dz < -40.0f && dz > -200.0f
				&& dist < 500.0f && floatPlace(w) != -2)
			{
				logf("drive: held up floating %.0f over %s: float off", -dz, id.c_str());
				floatUntil = -1.0f;
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			// v2 kit: held up right below it (a door a floor up, a ledge, within 400): float up on the fly item a while, as a player with it
			// would; the walk rises to a target above while levitating
			if (s.kitV2() && dz > 60.0f && dist < 400.0f && floatUntil < 0.0f && s.w.itemCount("uber_fly") > 0 && p.levitate <= 0.0f)
			{
				logf("drive: stuck below %s (%.0f up): floating up", id.c_str(), dz);
				floatUntil = s.w.time + 8.0f;
				floatWalk = true;
				path.clear();          // (straight at it, rising to its height: the route's points are down here)
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			// held back by a swinging door (opened on the way, it swings into us): back off a moment and let it open, as
			// a player steps back from a door opening toward them (a closed one near is opened first, below)
			if (s.kitV2() && strcmp(g_playerBlock, "door") == 0 && backTries < 2 && (door < 0 || door == doorLast))
			{
				logf("drive: the door swings into us on the way to %s: backing off it", id.c_str());
				backTries++;
				backTimer = 1.0f;
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			else if (attacker >= 0)
			{
				logf("drive: %s attacks us on the way to %s: fighting it", w.refs[attacker].id.c_str(), id.c_str());
				fightRef = attacker;
				fightTimer = 0.0f;
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			// v2 kit: a locked one: unlocked first with what is carried (an Ondusi's Unhinging scroll, aimed at it), as a
			// player would, then opened (Rotheran's arena: a kill's chase left us on the wrong side of a locked door). The
			// scroll's magnitude is a roll: one too weak for the lock ("The lock is too strong") is followed by another
			else if (s.kitV2() && door >= 0 && door != doorLast && w.refs[door].lockLevel > 0 && unlockTriesLeft(door)
				&& w.itemCount("sc_ondusisunhinging") > 0)
			{
				unlockTries++;
				const Ref& d = w.refs[door];
				float ux = (d.boxMin[0] + d.boxMax[0]) * 0.5f - p.feet[0], uy = (d.boxMin[1] + d.boxMax[1]) * 0.5f - p.feet[1];
				p.yaw = atan2f(ux, uy);
				p.pitch = atan2f((d.boxMin[2] + d.boxMax[2]) * 0.5f - playerEyeZ(p), fmaxf(hypotf(ux, uy), 1.0f));
				logf("drive: %s on the way to %s is locked (%d): Ondusi's Unhinging on it", d.id.c_str(), id.c_str(), d.lockLevel);
				s.castItem("sc_ondusisunhinging");
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			else if (door >= 0 && door != doorLast)
			{
				logf("drive: opening %s on the way to %s", w.refs[door].id.c_str(), id.c_str());
				bool trapped = !w.refs[door].trap.empty() && !w.refs[door].disarmed;
				s.playerActivate(door);
				if (trapped && w.refs[door].doorTarget == 0.0f)
				{
					logf("drive: the trap on %s used the press, again", w.refs[door].id.c_str());
					s.playerActivate(door);
				}
				doorLast = door;
				doorTried = true;
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			else if ((s.testLegit && stuckTries == 2 && diveOk && floodTries < 3 && (floodTries++, true)
					&& floodRoute(s, floodGoal, pointGoal ? 80.0f : 150.0f, swimRoute, 200.0f, true))
				|| (s.testLegit && stuckTries <= 2 && !diveOk && floodTries < 3 && splitDeadEnd(s, p.feet, floodGoal, entry, &deadEntry)
					&& (trialOk(entry) || (deadEntry = -1, false)) && (floodTries++, true) && (memcpy(deadGoal, floodGoal, sizeof(deadGoal)), true)
					&& (floodRoute(s, entry, 80.0f, swimRoute) || (deadEntry = -1, false)))
				// LEGIT, held up a while on foot (a path link through a rock or a wall, a ledge, a gap a sidestep doesn't
				// clear): a route by trial steps to the next path point (else the goal), found with the game's own moves;
				// the walk goes on along the same route from there. Only one that arrives at the target's own level: one
				// up on the furniture (a bed, a table) beside it is no way there
				|| (s.testLegit && stuckTries == 2 && !diveOk && floodTries < 3 && p.levitate <= 0.0f
					&& trialOk(path.empty() ? floodGoal : w.pathPoints[path.front()].pos) && (floodTries++, true)
					&& floodRoute(s, path.empty() ? floodGoal : w.pathPoints[path.front()].pos, 80.0f, swimRoute, 60.0f)
					&& (floodKeep = !path.empty(), true))
				// LEGIT, in the water under a shore no search reached the goal from (a cliff below a hill town: Ald Velothi's
				// pier, the way back up to the door): swim to the nearest beach first, as a player would; the walk plans
				// again from the dry ground
				|| (s.testLegit && stuckTries == 2 && floodTries < 3 && p.levitate <= 0.0f && w.here().hasWater()
					&& p.feet[2] < w.here().waterZ && (floodTries++, true) && floodRoute(s, floodGoal, 80.0f, swimRoute, 200.0f, true, true)
					&& (floodKeep = false, true)))
			{
				logf("drive: stuck at %.0f %.0f %.0f going to %s: following a route of %d trial steps", p.feet[0], p.feet[1], p.feet[2], id.c_str(), (int)swimRoute.size());
				legTimer = 0.0f;
				legJumped = false;
				progressTimer = 0.0f;
				memcpy(progressAt, p.feet, sizeof(progressAt));
				return true;
			}
			else if (++stuckTries <= 2)
			{
				// step aside (left, then right) for a moment, as a player would around a post or a person
				sidestep = stuckTries == 1 ? -1.0f : 1.0f;
				sidestepTimer = 1.0f * (s.kitV2() ? 550.0f / fmaxf(100.0f, p.runSpeed * p.loadSpeed) : 1.0f);
			}
			else if (stuckTries == 3)
			{
				in.jump = true;
				in.moveY = 0.0f;      // (from standing: a running jump is a 45 degree hop, half as high; OpenMW)
			}
			else if (s.testLegit && stuckTries <= 12)
			{
				// LEGIT (no warp to fall back on): keep working round it as a player would, wider each time:
				// left, right, a running jump, by turns (a shrub, a rock, a rail)
				int round = (stuckTries - 1) / 3, step = (stuckTries - 1) % 3;
				if (step == 2)
				{
					in.jump = true;
					in.moveY = 0.0f;
				}
				else
				{
					sidestep = step == 0 ? -1.0f : 1.0f;
					sidestepTimer = (1.0f + round * 0.8f) * (s.kitV2() ? 550.0f / fmaxf(100.0f, p.runSpeed * p.loadSpeed) : 1.0f);
				}
			}
			else if (s.testLegit)
			{
				// LEGIT: no warp past it (a player can't); the run stops being one a player could have played
				{
					// what could be holding a player: tired, slowed, burdened, or a screen up
					std::string fx;
					for (const ActiveEffect& e : w.effects)
						fx += " " + std::to_string(e.effect) + "(" + std::to_string((int)e.magnitude) + ")";
					logf("drive: stuck diagnostics: fatigue %.0f of %.0f, effects:%s, screen %d, swimming %d, levitate %.0f", w.stats.fatigue, w.stats.fatigueMax,
						fx.empty() ? " none" : fx.c_str(), (int)s.screen, (int)p.swimming, p.levitate);
					logf("drive: stuck diagnostics: target at %.0f %.0f %.0f, last pushed back by %s", r.pos[0], r.pos[1], r.pos[2], g_playerBlock);
					// who stands close (a person in the way of a doorway shows here)
					for (int i : w.loadedActors)
					{
						const Ref& a = w.refs[i];
						float ex = a.pos[0] - p.feet[0], ey = a.pos[1] - p.feet[1];
						if (a.visible() && !a.dead && ex * ex + ey * ey < 250.0f * 250.0f && fabsf(a.pos[2] - p.feet[2]) < 200.0f)
							logf("drive: stuck diagnostics: %s at %.0f %.0f %.0f, %.0f away, ai %d, package %d", a.id.c_str(), a.pos[0], a.pos[1], a.pos[2],
								hypotf(ex, ey), a.ai, a.aiPackage);
					}
				}
char why[160];
				snprintf(why, sizeof(why), "stuck at %.0f %.0f %.0f (%.0f away, %.0f up), no warp in a LEGIT run", p.feet[0], p.feet[1],
					p.feet[2], dist, dz);
				fail(why);
				return false;
			}
			else
			{
				logf("drive: stuck at %.0f %.0f %.0f going to %s (%.0f away, %.0f up), warped", p.feet[0], p.feet[1], p.feet[2],
					id.c_str(), dist, dz);
				float k = dist > 1.0f ? (want - 20.0f) / dist : 0.0f;
				float ux = dist > 1.0f ? dx / dist : 1.0f, uy = dist > 1.0f ? dy / dist : 0.0f;
				p.feet[0] = tx - dx * k;
				p.feet[1] = ty - dy * k;
				p.feet[2] = r.pos[2] + 20.0f;
				// land where it can be seen (not behind a wall or a gate): turn the approach round until it can
				float eye[3] = { tx, ty, r.pos[2] + 100.0f };
				for (int a = 0; a < 8; a++)
				{
					float ang = a * 0.7854f, c = cosf(ang), sn = sinf(ang);
					float ox = tx - (ux * c - uy * sn) * (want - 20.0f), oy = ty - (ux * sn + uy * c) * (want - 20.0f);
					float from[3] = { ox, oy, r.pos[2] + 120.0f };
					if (w.lineOfSight(from, eye, false))
					{
						p.feet[0] = ox;
						p.feet[1] = oy;
						break;
					}
				}
				p.vz = 0.0f;
				p.fallTop = p.feet[2];
				p.landedFall = 0.0f;
				path.clear();
				stuckTries = 0;
			}
		}
		progressTimer = 0.0f;
		memcpy(progressAt, p.feet, sizeof(progressAt));
	}
	return true;
}
