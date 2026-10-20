// AI combat: an NPC or creature fighting (moves, ranged attacks, fleeing)
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

static const float kArcherMinRange = 350.0f, kArcherMaxRange = 1600.0f;

// Their bow / crossbow / thrown weapon lets go at the player
void Session::npcFire(int ri)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = w.game.object(def.weapon);
	if (!wpn || r.ammo <= 0)
		return;
	r.ammo--;
	float from[3] = { r.pos[0] + sinf(r.rot[2]) * 30.0f, r.pos[1] + cosf(r.rot[2]) * 30.0f, r.pos[2] + 110.0f };
	float to[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + 90.0f };
	float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	// Aim wobbles less the better their Marksman
	float err = (100 - def.skills[SKILL_MARKSMAN]) * 0.0015f;
	for (int k = 0; k < 3; k++)
		d[k] = d[k] / len + (frand() - 0.5f) * err;
	const std::string& ammo = wpn->subtype == WEAP_THROWN ? def.weapon : def.ammo;
	// (the player's rule: fProjectileMinSpeed .. MaxSpeed, or fThrownWeaponMin .. MaxSpeed, by the draw)
	bool thrown = wpn->subtype == WEAP_THROWN;
	float draw = 0.6f + 0.4f * frand();
	float lo = thrown ? w.game.gmstf("fthrownweaponminspeed", 300.0f) : w.game.gmstf("fprojectileminspeed", 400.0f);
	float hi = thrown ? w.game.gmstf("fthrownweaponmaxspeed", 1000.0f) : w.game.gmstf("fprojectilemaxspeed", 3000.0f);
	fireProjectile(ri, from, d, lo + (hi - lo) * draw, ammo, def.weapon, draw, "");
	playSound(ri, wpn->subtype == WEAP_CROSSBOW ? "crossbowShoot" : wpn->subtype == WEAP_BOW ? "bowShoot" : "SwishM");
}

// Combat moves (OpenMW's AiCombatStorage::startCombatMove, looked at each reaction, 0.25 s, while ready to attack).
// People and two-legged creatures fighting hand to hand step aside: 0.2 s when the foe has come more than 45 degrees
// round (`facing`: the turn still to make toward it), else a quarter of the time within reach of the foe's weapon,
// 0.1 - 0.2 s, left or right. A ranged fighter backs away from a foe holding no ranged weapon within 1.5 x its
// reach (npcStep keeps it off a drop and out of walls, as OpenMW's rays behind it). Both at run speed (a fight runs)
bool Session::combatMove(int ri, Cell& cell, float dist, float facing, bool ranged, const Object* foeWeapon, float dt)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	Actor* a = w.actorOf(ri);
	float foeReach = meleeReach(w, foeWeapon, false) + actorHalfLength(ri) + kPlayerHalfLength;
	r.combatReact -= dt;
	if (r.combatReact <= 0.0f)
	{
		r.combatReact = 0.25f;
		if (r.sideStep != 0)
			r.sideTimer = 0.1f + 0.1f * frand();
		else if ((!def.creature || (def.afloat & 1)) && !ranged)
		{
			float len = fabsf(facing) > 0.785398f ? 0.2f : dist <= foeReach && frand() < 0.25f ? 0.1f + 0.1f * frand() : 0.0f;
			if (len > 0.0f)
			{
				r.sideStep = rand() % 2 ? 1 : -1;
				r.sideTimer = len;
				r.sideSteps++;
			}
		}
		r.backingUp = ranged && !(foeWeapon && isRanged(foeWeapon)) && dist <= foeReach * 1.5f && !(a && a->swimming);
	}
	if (r.sideStep != 0 && (r.sideTimer -= dt) <= 0.0f)
		r.sideStep = 0;
	float fx = sinf(r.rot[2]), fy = cosf(r.rot[2]);
	const char* group = nullptr;
	if (r.backingUp && npcStep(ri, cell, -fx, -fy, actorRunSpeed(ri) * dt))
		group = "RunBack";
	else if (r.sideStep != 0 && npcStep(ri, cell, fy * r.sideStep, -fx * r.sideStep, actorRunSpeed(ri) * dt))
		group = r.sideStep > 0 ? "RunRight" : "RunLeft";
	if (!group)
		return false;
	if (a && (a->mode == ANIM_LOOP || a->mode == ANIM_IDLE))
	{
		ActorSet& set = *w.actorsOf(ri);
		if (!actorPlay(set, *a, group, ANIM_LOOP))
			actorPlay(set, *a, stanceGroup(set, *a, "RunForward", w.game.object(def.weapon)).c_str(), ANIM_LOOP);
	}
	return true;
}

void Session::npcCombat(int ri, float dt)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = w.game.object(def.weapon);
	float dx = w.player.feet[0] - r.pos[0], dy = w.player.feet[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist > kHostileGiveUp || playerDead || !waterCreatureCanFight(w, ri))
	{
		r.ai = AI_IDLE;
		r.fleeing = false;
		if (Actor* a = w.actorOf(ri))
			a->showWeapon = false;
		return;
	}
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	Cell* cell = w.cellOf(ri);
	if (!cell)
		return;

	// Knocked down: helpless until they get up (npcKnockUpdate)
	if (r.knockTimer > 0.0f)
		return;

	// Fleeing: badly hurt actors run for a while, then come back
	// OpenMW's vanillaRateFlee: Flee 100 and up always flees; else (1 - health share) x fAIFleeHealthMult + Flee x
	// fAIFleeFleeMult, plus the distance bias (iFightDistanceBase - fFightDistanceMultiplier x distance) when not 0;
	// it flees at 100 or more (the action choice is made again every second or so)
	const float fleeRating = fleeRatingOf(ri, dist);
	if (!r.fleeing && fleeRating >= 100.0f)
	{
		r.fleeing = true;
		r.fleeTimer = 3.0f;
		r.fleeState = 0;
		npcSayTopic(ri, "Flee");
		r.hitAt = -1.0f;
		r.castSpell = -1;
		logf("combat: %s flees", r.id.c_str());
	}
	if (r.fleeing)
	{
		// the choice is made again every 3 s (OpenMW's ActionFlee cooldown): still rated 100, keep fleeing
		r.fleeTimer -= dt;
		if (r.fleeTimer <= 0.0f)
		{
			if (fleeRating >= 100.0f)
				r.fleeTimer = 3.0f;
			else
				r.fleeing = false;
		}
	}
	if (r.fleeing)
	{
		// OpenMW's AiCombat::updateFleeing: it stands until the foe, in sight, comes within the reach of the foe's
		// weapon (any distance for a bow or a throwing weapon), then runs to a path grid point picked at random among
		// those its nearest one leads to in that cell (none: away from the foe for 1 s), and stands again there or
		// once fFleeDistance away out of sight. (OpenMW also counts a spell in hand: our player has no spell stance.)
		// It ran straight away from the player, all the time, until 3000 away
		float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
		float eyeB[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
		bool los = w.lineOfSight(eyeA, eyeB);
		bool run = false;
		if (r.fleeState == 0 && los)
		{
			const Object* pw = playerWeapon();
			float trigger = !pw ? kCombatDistance : isRanged(pw) ? 1000.0f : fmaxf(1.0f, pw->reach) * kCombatDistance;
			if (trigger >= 1000.0f || dist - actorHalfLength(ri) - kPlayerHalfLength <= trigger)
			{
				r.fleeState = 1;
				r.fleeRun = 0.0f;
				int p0 = w.nearestPathPoint(r.pos);
				if (p0 >= 0 && !((def.afloat & 0x10) && !(def.afloat & 0x40)))
				{
					// the points p0 leads to, in its cell
					std::vector<int> seen = { p0 }, open = { p0 };
					while (!open.empty())
					{
						int p = open.back();
						open.pop_back();
						for (int q : w.pathPoints[p].links)
							if (w.pathPoints[q].cell == w.pathPoints[p0].cell && std::find(seen.begin(), seen.end(), q) == seen.end())
							{
								seen.push_back(q);
								open.push_back(q);
							}
					}
					if (seen.size() > 1)
					{
						memcpy(r.fleeDest, w.pathPoints[seen[1 + rand() % (seen.size() - 1)]].pos, sizeof(r.fleeDest));
						r.fleeState = 2;
						r.path.clear();
						r.repathTimer = 0.0f;
					}
				}
			}
		}
		if (r.fleeState == 1)
		{
			r.fleeRun += dt;
			if (r.fleeRun < 1.0f)
			{
				float ax = -dx / fmaxf(dist, 1.0f), ay = -dy / fmaxf(dist, 1.0f);
				float diff = angleDiff(atan2f(ax, ay), r.rot[2]);
				r.rot[2] += fmaxf(-kTurnSpeed * dt, fminf(kTurnSpeed * dt, diff));
				npcStep(ri, *cell, ax, ay, actorRunSpeed(ri) * dt);
				run = true;
			}
			else
				r.fleeState = 0;
		}
		else if (r.fleeState == 2)
		{
			float ex = r.fleeDest[0] - r.pos[0], ey = r.fleeDest[1] - r.pos[1];
			if ((dist > w.game.gmstf("ffleedistance", 3000.0f) && !los) || ex * ex + ey * ey < 40.0f * 40.0f)
				r.fleeState = 0;
			else
			{
				npcMoveTo(ri, r.fleeDest, actorRunSpeed(ri), dt, true);
				run = true;
			}
		}
		if (a)
		{
			if (run && !actorPlay(*set, *a, "RunForward", ANIM_LOOP))
				actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
			else if (!run && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		r.moved = true;
		w.syncActor(ri);
		return;
	}

	// Face the player
	float want = atan2f(dx, dy);
	float diff = angleDiff(want, r.rot[2]);
	float turn = kTurnSpeed * dt;
	r.rot[2] += fmaxf(-turn, fminf(turn, diff));
	r.moved = true;

	bool ranged = isRanged(wpn) && r.ammo > 0;
	if (wpn && isRanged(wpn) && r.ammo <= 0)
		wpn = nullptr;                                     // out of arrows: fists
	// (from the middles: the reach and both half lengths. It was fCombatDistance x 0.9 for a creature, whatever its size:
	// a guar, 120 long each way of its middle, bit with its head through the player)
	const float gap = actorHalfLength(ri) + kPlayerHalfLength, meleeAt = meleeReach(w, wpn, def.creature);
	float reach = meleeAt + gap;
	bool attacking = r.hitAt >= 0.0f;
	float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
	float eyeB[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };

	// A potion when one rates above its weapon (npcDrinkPotion), weighed as often as OpenMW's actors choose their next
	// action, about every blow
	r.potionTimer -= dt;
	if (!attacking && r.potionTimer <= 0.0f)
	{
		r.potionTimer = 0.75f;
		npcDrinkPotion(ri, dist);
	}

	// Spells now and then
	r.castTimer -= dt;
	if (!attacking && r.castTimer <= 0.0f && (!def.combatSpells.empty() || !def.spells.empty()))   // (a summon among its spells)
	{
		r.castTimer = 3.0f + frand() * 3.0f;
		if (w.lineOfSight(eyeA, eyeB))
			npcCastSpell(ri);
		attacking = r.hitAt >= 0.0f;
	}

	float keep = ranged ? kArcherMaxRange : reach * 0.9f;
	bool see = true;
	// (OpenMW's getHitContact wants line of sight for a blow too: none through a gate or a wall)
	if (!attacking && (ranged || dist <= keep))
		see = w.lineOfSight(eyeA, eyeB);
	// in reach only on about the same level (a player on the platform above can't be struck from below:
	// that's a chase, which stands still when it gets nowhere, not an idle that flips back each frame)
	// (one that flies or swims is in reach only within its reach up or down too, as OpenMW measures it to the target's
	// bounds: it closes in through the air or the water; a cliff racer hovered over the player's head)
	// (up or down OpenMW's isInMeleeReach counts the reach alone, not the bodies' half lengths: a netch 160 over the
	// player stayed there, its bites short)
	bool level = ranged || fabsf(w.player.feet[2] - r.pos[2]) < (zMoverTop(w, r, *cell) > -1e8f ? meleeAt : 180.0f);
	if ((dist > keep || !see || !level) && !attacking)
	{
		// Chase: straight at the player, or along the path grid around walls. Can't get any closer (a
		// ledge, a gap the grid doesn't cover): stand rather than run in place, and run again once it moves.
		r.sideStep = 0;
		r.backingUp = false;
		float was[2] = { r.pos[0], r.pos[1] };
		npcMoveTo(ri, w.player.feet, actorRunSpeed(ri), dt, false);
		float moved = hypotf(r.pos[0] - was[0], r.pos[1] - was[1]);
		// progress: how far it really got each second (sliding to and fro against a door or a wall, or the
		// player walking up to it, doesn't count)
		r.chaseWindow += dt;
		if (r.chaseWindow >= 1.0f)
		{
			float mx = r.pos[0] - r.chaseAnchor[0], my = r.pos[1] - r.chaseAnchor[1];
			// (getting nowhere: under 60 units a second, or under a third of its own pace for a slow one: a steam
			// centurion runs 43 a second, was judged stuck after 2 s of a chase, and idled or walked while it ran on)
			float minGain = fminf(60.0f, actorRunSpeed(ri) * 0.33f) * r.chaseWindow;
			r.chaseStill = mx * mx + my * my < minGain * minGain ? r.chaseStill + r.chaseWindow : 0.0f;
			r.chaseAnchor[0] = r.pos[0];
			r.chaseAnchor[1] = r.pos[1];
			r.chaseWindow = 0.0f;
		}
		// (stood still only while it really goes nowhere: one sliding slowly along a fence or a wall it can't get past walks,
		// over a crawl of 30 a second; it played its Idle and glided along, legs still)
		if (r.chaseStill >= 2.0f && moved <= 30.0f * dt)
		{
			if (a && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		else if (r.chaseStill >= 2.0f)
		{
			if (a && (a->mode == ANIM_LOOP || a->mode == ANIM_IDLE))
				actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
		}
		else if (a && !actorPlay(*set, *a, stanceGroup(*set, *a, "RunForward", wpn).c_str(), ANIM_LOOP))
			actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
	}
	else
	{
		r.chaseStill = 0.0f;
		r.chaseWindow = 0.0f;
		r.chaseAnchor[0] = r.pos[0];
		r.chaseAnchor[1] = r.pos[1];
		if (!combatMove(ri, *cell, dist, diff, ranged, playerWeapon(), dt) && a && (a->mode == ANIM_LOOP || a->mode == ANIM_IDLE))
		{
			// ready to strike: the weapon's stance (its idles otherwise, left to run on)
			std::string idle = stanceGroup(*set, *a, "Idle", wpn);
			if (idle != "Idle")
				actorPlay(*set, *a, idle.c_str(), ANIM_LOOP);
			else if (a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		r.attackTimer -= dt;
		if (!attacking && r.attackTimer <= 0.0f && fabsf(diff) < 0.6f && (!ranged || dist > kArcherMinRange * 0.5f))
		{
			float windup = 0.35f, length = 1.0f;
			if (a)
				a->showWeapon = wpn != nullptr;
			if (!def.creature && rand() % 100 < (int)w.game.gmstf("ivoiceattackodds", 10.0f))
				npcSayTopic(ri, "Attack");
			if (a && actorPlay(*set, *a, blowGroup(*set, *a, r, wpn).c_str(), ANIM_ONCE))
			{
				const AnimGroup& g = actorSkeleton(*set, a->skeleton).groups[a->group];
				windup = g.loopStart - g.start;
				length = g.stop - g.start;
			}
			r.hitAt = w.time + windup;
			r.shooting = ranged;
			r.castSpell = -1;
			r.attackTimer = length + 0.4f + frand() * 0.8f / fmaxf(0.5f, wpn ? wpn->speed : 1.0f);
		}
	}
	if (r.hitAt >= 0.0f && w.time >= r.hitAt)
	{
		r.hitAt = -1.0f;
		if (r.castSpell == -2)
		{
			// A summon leaves their hands (the same chance as any of their spells; Silence: none)
			r.castSpell = -1;
			auto ps = pendingSummon.find(ri);
			auto sit = ps == pendingSummon.end() ? w.game.spells.end() : w.game.spells.find(ps->second);
			if (ps != pendingSummon.end())
				pendingSummon.erase(ps);
			if (sit != w.game.spells.end())
			{
				const SpellDef& sp = sit->second;
				static const int kSchoolSkill[6] = { 11, 13, 10, 12, 14, 15 };
				float chance = sp.type == 5 ? 100.0f : (def.skills[kSchoolSkill[spellSchool(sp)]] * 2.0f + def.attributes[ATTR_WILLPOWER] / 5.0f
					+ def.attributes[ATTR_LUCK] / 10.0f - sp.cost - w.actorEffect(ri, 48)) * fatigueTerm(r.fatigue, r.fatigueMax);
				if (w.actorEffect(ri, 46) > 0.0f || rand() % 100 >= (int)chance)
				{
					playSound(ri, "Spell Failure Conjuration");
					logf("combat: %s's summon fizzles", r.id.c_str());
				}
				else
					npcSummonRelease(ri, sp);
			}
			if (a)
				a->showWeapon = wpn != nullptr;
		}
		else if (r.castSpell >= 0)
		{
			// The spell leaves their hands
			const SpellDef& sp = w.game.spells[def.combatSpells[r.castSpell]];
			r.castSpell = -1;
			static const int schoolSkill[6] = { 11, 13, 10, 12, 14, 15 };
			int school = spellSchool(sp);
			// (the player's rule, their Sound taken off before the fatigue term; Silence: no spell at all)
			float chance = sp.type == 5 ? 100.0f : (def.skills[schoolSkill[school]] * 2.0f + def.attributes[ATTR_WILLPOWER] / 5.0f
				+ def.attributes[ATTR_LUCK] / 10.0f - sp.cost - w.actorEffect(ri, 48)) * fatigueTerm(r.fatigue, r.fatigueMax);
			if (w.actorEffect(ri, 46) > 0.0f)
				chance = 0.0f;
			if (rand() % 100 >= (int)chance)
			{
				playSound(ri, "Spell Failure Destruction");
				logf("combat: %s's spell fizzles", r.id.c_str());
			}
			else
			{
				bool target = false;
				for (auto& e : sp.effects)
				{
					if (e.range == 0 && e.effect == 75)
						r.health = fminf(r.healthMax, r.health + (e.min + (e.max - e.min) * frand()) * (e.duration > 0 ? e.duration : 1));
					else if (e.range == 1 && dist <= kCombatDistance + gap)
						applyEffectToPlayer(e.effect == 86 ? SpellEffect{ 23, e.skill, e.attribute, e.min, e.max, e.duration, 1 } : e, sp.name, ri, (float)sp.cost,
							chance);
					else if (e.range == 2)
						target = true;
				}
				if (target)
				{
					float from[3] = { r.pos[0] + sinf(r.rot[2]) * 40.0f, r.pos[1] + cosf(r.rot[2]) * 40.0f, r.pos[2] + 110.0f };
					float d[3] = { eyeB[0] - from[0], eyeB[1] - from[1], eyeB[2] - 30.0f - from[2] };
					float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
					for (int k = 0; k < 3; k++)
						d[k] /= len;
					fireProjectile(ri, from, d, 1200.0f, "", "", 1.0f, sp.id);
				}
			}
			if (a)
				a->showWeapon = wpn != nullptr;
		}
		else if (r.shooting)
		{
			r.shooting = false;
			npcFire(ri);
		}
		// (OpenMW's getHitContact: within the reach of the edges, the height apart under the reach, inside the hit cone.
		// It was 40 past the reach and 57 degrees round)
		else if (dist < reach && fabsf(w.player.feet[2] - r.pos[2]) < meleeAt && fabsf(angleDiff(want, r.rot[2])) < hitAngle(w))
			npcStrike(ri);
		else if (a)
			attackFollowThrough(*set, *a, frand());      // a blow at nothing: a strength all the same
	}
	w.syncActor(ri);
}
