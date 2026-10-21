// AI packages (wander, travel, follow, escort, activate): running one, finishing it
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

// A package that ran its hours or got where it was going is over: GetAIPackageDone reports 1, and OpenMW drops it, so
// the actor stands with no package (GetCurrentAIPackage -1) unless the script asked for it to repeat. The package
// still counts as the current one until the next tick (aiSettle), so a script polling both sees the pair once
void aiFinish(World& w, Ref& r)
{
	if (!r.aiDone)
		logf("ai: %s package %d done at %.0f %.0f %.0f", r.idLower.c_str(), r.aiPackage, r.pos[0], r.pos[1], r.pos[2]);
	r.aiDone = true;
	if (r.aiRepeat)
		r.aiStart = w.gameHour;
}

// The tick after a package finished: it is gone, nothing runs in its place
void aiSettle(Ref& r)
{
	if (!r.aiDone || r.aiRepeat || r.aiPackage <= AIPKG_NONE || r.aiPackage == AIPKG_IDLE)
		return;
	r.aiPackage = AIPKG_IDLE;
	r.aiTarget.clear();
	r.aiCell.clear();
	r.aiDest[0] = r.aiDest[1] = r.aiDest[2] = 0.0f;
	r.aiDuration = 0.0f;
	r.path.clear();
	r.wandering = false;
}

// Travel to a spot, follow someone, escort the player to a spot (waiting when they fall behind), or
// walk up to something and use it. Duration counts game hours; then GetAIPackageDone reports 1.
void Session::npcPackage(int ri, float dt)
{
	Ref& r = w.refs[ri];
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	aiSettle(r);
	trailRecord(w);
	if (r.aiDuration > 0.0f && r.aiActive && w.gameHour - r.aiStart >= r.aiDuration)
		aiFinish(w, r);
	// Where they're headed: the destination, or whoever they follow / escort / activate
	const float* who = nullptr;
	if (r.aiTarget == "player")
		who = w.player.feet;
	else if (!r.aiTarget.empty())
	{
		int t = w.findRef(r.aiTarget);
		if (t >= 0 && w.active(t))
			who = w.refs[t].pos;
	}
	bool anyDest = r.aiDest[0] != 0.0f || r.aiDest[1] != 0.0f || r.aiDest[2] != 0.0f;
	bool hasDest = anyDest;
	// The destination is in another cell: until there, they keep with whoever they follow / escort
	// (going through doors with the player)
	bool elsewhere = !r.aiCell.empty() && lower(w.cells[w.placeOf(ri)].name) != r.aiCell;
	if (elsewhere)
		hasDest = false;
	auto near = [&](const float* p, float d) {
		float dx = p[0] - r.pos[0], dy = p[1] - r.pos[1];
		return dx * dx + dy * dy < d * d;
	};
	// A destination reached: close on the ground and at its height too (OpenMW's AiPackage::pathTo measures all three
	// axes): flat alone, a guard in the hold right under the deck he was sent to counted as there. (A script's point is
	// often set a little above the floor: some 150 of height is let go)
	auto arrived = [&](const float* p, float d) { return near(p, d) && fabsf(p[2] - r.pos[2]) < 150.0f; };
	// How close to whoever they follow or escort: in a straight line, height and all (OpenMW's AiFollow, AiEscort). On the
	// ground alone, a follower at the foot of the stairs with the player halfway up stood as if it had caught up
	auto within = [&](const float* p, float d) {
		float dx = p[0] - r.pos[0], dy = p[1] - r.pos[1], dz = p[2] - r.pos[2];
		return dx * dx + dy * dy + dz * dz < d * d;
	};
	const float* goal = nullptr;
	float speed = actorWalkSpeed(ri);   // walking pace (OpenMW: by their Speed)
	bool running = false;               // only a follower far behind runs (OpenMW's AiFollow; the rest walk)
	switch (r.aiPackage)
	{
	case AIPKG_TRAVEL:
	{
		// (OpenMW leaves a destination more than 7168 units away alone: no walking, never done)
		float gap[3] = { r.aiDest[0] - r.pos[0], r.aiDest[1] - r.pos[1], r.aiDest[2] - r.pos[2] };
		if (gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2] > 7168.0f * 7168.0f)
			break;
		// (and, as Morrowind, one that stays flat-close for 2 s without getting there gives up: OpenMW's AiTravel, the
		// half height let go)
		r.aiNearTime = near(r.aiDest, 64.0f + 64.0f) ? r.aiNearTime + dt : 0.0f;
		if (arrived(r.aiDest, 64.0f) || r.aiNearTime >= 2.0f)
		{
			r.aiNearTime = 0.0f;
			aiFinish(w, r);
		}
		else
			goal = r.aiDest;
		break;
	}
	case AIPKG_FOLLOW:
	{
		// A scripted follower starts once it sees who it follows within follow distance + 384 (looked for twice a second)
		if (!r.aiActive)
		{
			if (who && near(who, 640.0f) && (int)(w.time * 2.0f) != (int)((w.time - dt) * 2.0f))
			{
				float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f }, eyeB[3] = { who[0], who[1], who[2] + 100.0f };
				if (w.lineOfSight(eyeA, eyeB))
				{
					r.aiActive = true;
					r.aiStart = w.gameHour;
				}
			}
			break;
		}
		// Arrived: in an interior, the cell it was told; outdoors, none (OpenMW)
		const LevelCell& here = w.cells[w.placeOf(ri)];
		if (anyDest && near(r.aiDest, 256.0f) && (here.interior ? lower(here.name) == r.aiCell : r.aiCell.empty()))
		{
			aiFinish(w, r);
			if (!r.aiRepeat)
				break;
		}
		if (who && !within(who, 256.0f))
		{
			goal = r.aiTarget == "player" ? followGoal(w, r, who, dt) : who;
			// running to catch up: from 450 units away, until within 325 (a dead zone, as in OpenMW, so they don't flip)
			bool wasRunning = a && a->mode == ANIM_LOOP && a->group == actorFindGroup(actorSkeleton(*set, a->skeleton), "RunForward");
			if (!within(who, wasRunning ? 325.0f : 450.0f))
			{
				speed = actorRunSpeed(ri);
				running = true;
			}
		}
		break;
	}
	case AIPKG_ESCORT:
		if (hasDest && arrived(r.aiDest, 128.0f))
			aiFinish(w, r);
		// Lead the way, but wait when the escorted one falls behind (OpenMW: stop past half extent + 450, go on again
		// within half extent + 250)
		else if (hasDest && (!who || within(who, r.escortWaiting ? 300.0f : 500.0f)))
		{
			r.escortWaiting = false;
			goal = r.aiDest;
		}
		else if (hasDest && who && !elsewhere)
			r.escortWaiting = true;
		else if (elsewhere && who && !near(who, 180.0f))
			goal = r.aiTarget == "player" ? followGoal(w, r, who, dt) : who;
		break;
	case AIPKG_ACTIVATE:
	{
		// Goes up to it and uses it again and again, never done by that (OpenMW): only when it's gone or disabled
		int t = w.findRef(r.aiTarget);
		if (t < 0 || !w.refs[t].visible())
		{
			aiFinish(w, r);
			break;
		}
		if (who)
		{
			float dz = who[2] - r.pos[2], dx = who[0] - r.pos[0], dy = who[1] - r.pos[1];
			float reach = w.game.gmstf("imaxactivatedist", 192.0f);
			if (dx * dx + dy * dy + dz * dz <= reach * reach)
			{
				if (w.refs[t].script >= 0)
					w.scripts[w.refs[t].script].activated = true;
			}
			else
				goal = who;
		}
		break;
	}
	}
	if (goal)
	{
		npcMoveTo(ri, goal, speed, dt, true);
		r.moved = true;
		// no headway for 2 s (a travel point beyond a wall): stand rather than walk in place; walk again
		// once it gets somewhere. (Headway: 40 a second, or a third of its own pace for a slow one, as a chase counts
		// it: the Kwama Forager walks 20 a second, was judged stuck after 2 s and slid along in its Idle)
		r.chaseWindow += dt;
		if (r.chaseWindow >= 1.0f)
		{
			float mx = r.pos[0] - r.chaseAnchor[0], my = r.pos[1] - r.chaseAnchor[1];
			float minGain = fminf(40.0f, speed * 0.33f) * r.chaseWindow;
			r.chaseStill = mx * mx + my * my < minGain * minGain ? r.chaseStill + r.chaseWindow : 0.0f;
			r.chaseAnchor[0] = r.pos[0];
			r.chaseAnchor[1] = r.pos[1];
			r.chaseWindow = 0.0f;
		}
		if (r.chaseStill >= 2.0f)
		{
			if (a && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		else if (a)
			actorPlay(*set, *a, running ? "RunForward" : "WalkForward", ANIM_LOOP);
	}
	else
	{
		if (a && a->mode == ANIM_LOOP)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		// Following: face whoever it is while standing. (Not an escort waiting for the one it leads: OpenMW's AiEscort only
		// stops, its idle playing, still facing its way. The prison ship's guard turned round each time the player fell
		// behind and back again when they caught up)
		if (who && r.aiPackage == AIPKG_FOLLOW && r.aiActive)
		{
			float diff = angleDiff(atan2f(who[0] - r.pos[0], who[1] - r.pos[1]), r.rot[2]);
			r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
			r.moved = true;
		}
	}
	w.syncActor(ri);
}
