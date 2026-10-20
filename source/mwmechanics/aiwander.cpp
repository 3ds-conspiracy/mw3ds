// AI wander: idles, standing to talk, what people say on their own
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

// Peaceful NPCs: face the player while talking to them; otherwise stroll to random spots within
// their wander distance of home, pausing between walks
// Talking to the player (or saying a line): stand, face them, no walk cycle (whatever the NPC was
// doing: wandering, a travel / follow / escort package; a line said on a follow or escort, on the move). True when
// that's what they do this frame.
bool Session::npcTalkStand(int ri, float dt)
{
	Ref& r = w.refs[ri];
	// (one following or escorting someone says a line on the move: OpenMW's voice lines don't stop an AI package)
	bool going = r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT;
	if (!((dlg.open && dlg.ref == ri) || (!sayDone(ri) && !going)))
		return false;
	float want = atan2f(w.player.feet[0] - r.pos[0], w.player.feet[1] - r.pos[1]);
	float diff = angleDiff(want, r.rot[2]);
	r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
	r.moved = true;
	Actor* a = w.actorOf(ri);
	if (a && (r.wandering || a->mode == ANIM_LOOP))
	{
		logf("ai: %s stops to talk", r.id.c_str());
		actorPlay(*w.actorsOf(ri), *a, "Idle", ANIM_IDLE);
	}
	r.wandering = false;
	r.wanderTimer = fmaxf(r.wanderTimer, 4.0f);
	w.syncActor(ri);
	return true;
}

void Session::npcWander(int ri, float dt)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	Cell* cell = w.cellOf(ri);
	if (!cell)
		return;
	(void)a;
	(void)set;
	int range = r.aiPackage == AIPKG_WANDER && r.aiRange >= 0 ? r.aiRange : def.wander;
	// a walk / run cycle left over from a fight, a flight or a finished package (combat ends when the
	// player goes away, StopCombat, ...): standing, not going anywhere
	if (!r.wandering && a && a->mode == ANIM_LOOP)
		actorPlay(*set, *a, "Idle", ANIM_IDLE);
	if (range <= 0)
		return;
	if (!r.wandering)
	{
		r.wanderTimer -= dt;
		if (r.wanderTimer > 0.0f)
			return;
		// Somewhere to stroll to: a path grid point (Morrowind's walkable spots) within their wander
		// distance of home that they can walk to, straight or along the grid (not one across a wall
		// they would only walk into); else a spot in the open at least a few steps away
		std::vector<int> spots;
		for (size_t i = 0; i < w.pathPoints.size(); i++)
		{
			const PathPoint& pp = w.pathPoints[i];
			if (pp.cell < 0 || !w.cells[pp.cell].live)
				continue;
			float hx = pp.pos[0] - r.home[0], hy = pp.pos[1] - r.home[1];
			float px = pp.pos[0] - r.pos[0], py = pp.pos[1] - r.pos[1];
			if (hx * hx + hy * hy <= (float)range * range && px * px + py * py > 150.0f * 150.0f
				&& fabsf(pp.pos[2] - r.pos[2]) < 200.0f)
				spots.push_back(i);
		}
		r.path.clear();
		bool picked = false, anySpot = !spots.empty();
		for (int tries = 0; tries < 4 && !spots.empty() && !picked; tries++)
		{
			int k = rand() % spots.size();
			const PathPoint& pp = w.pathPoints[spots[k]];
			spots.erase(spots.begin() + k);
			float from[3] = { r.pos[0], r.pos[1], r.pos[2] + 50.0f }, to[3] = { pp.pos[0], pp.pos[1], pp.pos[2] + 50.0f };
			if (!w.lineOfSight(from, to) && !w.findPath(r.pos, pp.pos, r.path))
				continue;
			r.wanderTo[0] = pp.pos[0];
			r.wanderTo[1] = pp.pos[1];
			picked = true;
		}
		if (!picked && !anySpot)
		{
			float ang = frand() * 6.2831853f, d = fmaxf(150.0f, frand() * range);
			r.wanderTo[0] = r.home[0] + sinf(ang) * d;
			r.wanderTo[1] = r.home[1] + cosf(ang) * d;
			picked = true;
		}
		if (!picked)
		{
			// nowhere reachable just now: look again in a while
			r.wanderTimer = 5.0f + frand() * 8.0f;
			return;
		}
		r.wandering = true;
		// give up on a walk after a while: the way's length at walking pace, and some
		float way = 0.0f, at[2] = { r.pos[0], r.pos[1] };
		for (int pi : r.path)
		{
			way += hypotf(w.pathPoints[pi].pos[0] - at[0], w.pathPoints[pi].pos[1] - at[1]);
			at[0] = w.pathPoints[pi].pos[0];
			at[1] = w.pathPoints[pi].pos[1];
		}
		way += hypotf(r.wanderTo[0] - at[0], r.wanderTo[1] - at[1]);
		r.wanderTimer = 8.0f + way / 70.0f;
		r.wanderCheck = 1.0f;
		r.wanderFrom[0] = r.pos[0];
		r.wanderFrom[1] = r.pos[1];
		if (a)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);    // turn first, then walk
		if (a)
			actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
	}
	// The next grid point on the way (passed ones dropped), else the spot itself
	float goal[2] = { r.wanderTo[0], r.wanderTo[1] };
	while (!r.path.empty())
	{
		const PathPoint& p = w.pathPoints[r.path.front()];
		float px = p.pos[0] - r.pos[0], py = p.pos[1] - r.pos[1];
		if (px * px + py * py > 50.0f * 50.0f)
		{
			goal[0] = p.pos[0];
			goal[1] = p.pos[1];
			break;
		}
		r.path.erase(r.path.begin());
	}
	float dx = goal[0] - r.pos[0], dy = goal[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	r.wanderTimer -= dt;
	bool arrived = (r.path.empty() && dist < 60.0f) || r.wanderTimer <= 0.0f;
	if (!arrived)
	{
		float want = atan2f(dx, dy);
		float diff = angleDiff(want, r.rot[2]);
		r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
		// Turn in place until facing the spot, then walk (fWalkSpeed-ish pace), still turning a
		// little on the way. Walking while turning hard is what made them circle spots beside them.
		if (fabsf(diff) < 0.35f)
		{
			if (a && a->mode != ANIM_LOOP)
				actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
			if (!npcStep(ri, *cell, dx / dist, dy / dist, fminf(110.0f * dt, dist)) && r.path.empty())
				arrived = true;
			// Stuck against something: covered under a third of a second's walk in a second
			r.wanderCheck -= dt;
			if (r.wanderCheck <= 0.0f)
			{
				float mx = r.pos[0] - r.wanderFrom[0], my = r.pos[1] - r.wanderFrom[1];
				if (mx * mx + my * my < 40.0f * 40.0f)
					arrived = true;
				r.wanderCheck = 1.0f;
				r.wanderFrom[0] = r.pos[0];
				r.wanderFrom[1] = r.pos[1];
			}
		}
		else if (a && a->mode == ANIM_LOOP)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		r.moved = true;
	}
	if (arrived)
	{
		r.wandering = false;
		r.path.clear();
		r.wanderTimer = 5.0f + frand() * 12.0f;
		if (a)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
	}
	w.syncActor(ri);
}

// ---- Voices: what people say on their own (the voice topics' lines that fit them)

bool Session::npcSayTopic(int ri, const char* voiceTopic)
{
	const Ref& r = w.refs[ri];
	if (r.dead || r.recType != REC_NPC_ || !sayDone(ri) || w.distanceToPlayer(ri) > 2000.0f)
		return false;
	const Topic* t = w.game.topic(voiceTopic);
	if (!t)
		return false;
	const Info* info = dialogueVoiced(w, *t, ri);
	if (!info || info->sound.empty())
		return false;
	say(ri, info->sound, info->text);
	return true;
}

// Idle lines (OpenMW's playIdleDialogue): not from one fighting, following or escorting someone. Tarvyn Faren, escorted
// to Pelagiad, stopped for his idle lines and was left behind
bool Session::idleVoiceOk(int ri)
{
	const Ref& r = w.refs[ri];
	return r.ai != AI_COMBAT && r.aiPackage != AIPKG_FOLLOW && r.aiPackage != AIPKG_ESCORT;
}
