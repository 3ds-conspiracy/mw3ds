// Actors as a group: hostility, death, followers, allies and duels, guards, greetings, soul trap
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"
#include <3ds.h>
#include <cstdio>
#include <malloc.h>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>

void Session::makeHostile(int ri)
{
	Ref& r = w.refs[ri];
	if (r.dead || r.ai == AI_COMBAT)
		return;
	r.ai = AI_COMBAT;
	r.attackTimer = 0.3f + frand() * 0.5f;
	r.castTimer = 1.0f + frand() * 2.0f;
	r.hitAt = -1.0f;
	r.lastSeen = w.time;
	if (dlg.open && dlg.ref == ri)
		closeScreen();
	if (Actor* a = w.actorOf(ri))
		a->showWeapon = r.actor >= 0 && !w.game.actors[r.actor].weapon.empty();
}

// StopCombat: the fight is over for them (OpenMW drops their combat packages; the only one fought here is the player's)
void Session::stopCombat(int ri)
{
	Ref& r = w.refs[ri];
	if (r.actor < 0 || r.ai != AI_COMBAT)
		return;
	r.ai = AI_IDLE;
	r.fleeing = false;
	r.aggressor = false;
}

void Session::killNpc(int ri, bool byPlayer)
{
	Ref& r = w.refs[ri];
	r.diedAt = w.gameHour;
	// An essential person: the prophecy can't be fulfilled now (Morrowind says so, and plays on)
	if (r.actor >= 0 && w.game.actors[r.actor].essential && !r.dead)
		messageBox(w.game.gmst("skilledessential", "With this character's death, the thread of prophecy is severed. "
			"Restore a saved game to restore the weave of fate, or persist in the doomed world you have created."), {});
	// Soultrap on them: a creature's soul fills a gem
	if (r.soulTrapUntil > w.time && r.actor >= 0 && w.game.actors[r.actor].creature)
	{
		const ActorDef& def = w.game.actors[r.actor];
		if (def.soul > 0 && w.trapSoul(def.id, def.soul))
		{
			notify(w.game.gmst("ssoultrapsuccess", "You have trapped a soul."));
			playSound(-1, "conjuration hit");
			logf("soultrap: %s (soul %d)", def.id.c_str(), def.soul);
		}
	}
	if (byPlayer && r.actor >= 0 && !w.game.actors[r.actor].creature && !r.aggressor)
	{
		// Murder is a crime when anyone else could report it: OpenMW counts any living NPC within the alarm radius
		// (hearing is enough, not just sight); a kill with nobody about, a writ carried out alone, isn't one
		int witness = -1;
		float alarmRadius = w.game.gmstf("falarmradius", 2000.0f);
		for (int i : w.loadedActors)
		{
			const Ref& o = w.refs[i];
			if (i != ri && o.recType == REC_NPC_ && !o.dead && !o.ally && o.knockTimer <= 0.0f && w.active(i)
				&& w.distanceToPlayer(i) < alarmRadius)
			{
				witness = i;
				break;
			}
		}
		if (witness >= 0)
			reportCrime(ri, kBountyMurder);
		if (witness >= 0)
			crimeSeen(CRIME_MURDER, 0, ri);
		logf("crime: %s murdered, %s", r.id.c_str(), witness >= 0 ? ("seen by " + w.refs[witness].id).c_str() : "unseen");
		r.murdered = true;
	}
	r.dead = r.died = true;
	w.deadCounts[r.idLower]++;
	r.health = 0.0f;
	r.ai = AI_IDLE;
	r.hitAt = -1.0f;
	r.fleeing = false;
	bool down = r.knockTimer > 0.0f, out = down && r.outCold;
	r.knockTimer = 0.0f;
	if (r.gold > 0)
		r.contents.emplace_back(r.gold, "gold_001");
	r.gold = 0;
	if (Actor* a = w.actorOf(ri))
	{
		// One of its deaths at random, or the one from lying down or out (OpenMW's playRandomDeath)
		ActorSet& set = *w.actorsOf(ri);
		const char* fromDown = out ? "DeathKnockOut" : down ? "DeathKnockDown" : "";
		std::string g = fromDown[0] && actorFindGroup(actorSkeleton(set, a->skeleton), fromDown) >= 0 ? fromDown
			: actorRandomGroup(set, *a, "Death");
		snprintf(r.deathGroup, sizeof(r.deathGroup), "%s", g.empty() ? "Death1" : g.c_str());
		a->showWeapon = false;
		if (!actorPlay(set, *a, r.deathGroup, ANIM_HOLD))
			logf("combat: %s has no death animation", r.id.c_str());
	}
	w.syncActor(ri);
	playSound(ri, "Body Fall Medium");
	logf("combat: %s died", r.id.c_str());
}

// Health gone, whatever took it (blows, spells, traps, falls, scripts)
void Session::playerDies()
{
	PlayerStats& s = w.stats;
	if (testGod)
	{
		s.health = s.healthMax;
		logf("test: god mode kept the player alive");
		return;
	}
	s.health = 0.0f;
	playerDead = true;
	for (auto& r : w.refs)
		r.ai = AI_IDLE;
	logf("combat: player died");
	// (the saves are listed here once: reading them every frame of the screen cost 70 ms a frame)
	deathSave.clear();
	u64 newest = 0;
	std::vector<SaveInfo> saves = savesList(dataDir);
	for (const SaveInfo& sv : saves)
		if (!sv.bundled && (deathSave.empty() || sv.time > newest))
		{
			deathSave = sv.path;
			newest = sv.time;
		}
	deathAnySave = !saves.empty();
	openScreen(SCR_DEATH);
}

// Its collision box's half length (OpenMW's getHalfExtents().y(), what getDistanceToBounds takes off): a creature's
// "Bounding Box" node, people's base_anim.nif box (28.5), x its scale
float Session::actorHalfLength(int ri)
{
	const Ref& r = w.refs[ri];
	if (r.actor < 0)
		return 0.0f;
	const ActorDef& def = w.game.actors[r.actor];
	float s = r.scale > 0.0f ? r.scale : 1.0f;
	if (const Actor* a = w.actorOf(ri))
		s = sqrtf(a->place[0] * a->place[0] + a->place[4] * a->place[4] + a->place[8] * a->place[8]);
	return (def.hasBox ? def.box[3] : kPlayerHalfLength) * s;
}

// ---- Summoned allies

// The nearest actor fighting the player within 1500: fought as any fight between two actors is (npcDuel: its reach
// from the edges, the wind-up, the blow at the attack's hit with a hit roll and armour), as OpenMW gives the player's
// followers and summons a combat package against whoever fights the player. It struck at once every 1.2 s from 120
// away, never missing. With nobody to fight it follows the player
void Session::allyFight(int ri, float dt)
{
	Ref& r = w.refs[ri];
	int foe = -1;
	float best = 1500.0f;
	w.forLoadedActors([&](int i) {
		const Ref& o = w.refs[i];
		if (i == ri || o.dead || o.ally || o.ai != AI_COMBAT)
			return;
		float dx = o.pos[0] - r.pos[0], dy = o.pos[1] - r.pos[1], d = sqrtf(dx * dx + dy * dy);
		if (d < best)
		{
			best = d;
			foe = i;
		}
	});
	if (foe < 0)
	{
		npcPackage(ri, dt);
		return;
	}
	r.duelFoe = foe;
	npcDuel(ri, dt);
}

// StartCombat on another actor (not the player): OpenMW gives it a combat package with that actor as the target. It
// walks up to its foe and swings (hit chance, damage and armor as for a blow on the player, no block or crit), the
// foe takes up the fight against it, and a death ends it. The player's own fights come first (AI_COMBAT: npcCombat)
void Session::npcDuel(int ri, float dt)
{
	Ref& r = w.refs[ri];
	const int fi = r.duelFoe;
	if (fi < 0 || fi >= (int)w.refs.size() || w.refs[fi].dead || w.refs[fi].actor < 0 || !w.sameSpace(fi, ri))
	{
		r.duelFoe = -1;
		r.hitAt = -1.0f;
		if (Actor* a = w.actorOf(ri))
			a->showWeapon = false;
		return;
	}
	Ref& f = w.refs[fi];
	const ActorDef& def = w.game.actors[r.actor];
	const ActorDef& fdef = w.game.actors[f.actor];
	const Object* wpn = w.game.object(def.weapon);
	if (wpn && (wpn->recType != REC_WEAP || isRanged(wpn)))
		wpn = nullptr;
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	if (f.duelFoe < 0 && f.ai != AI_COMBAT)
		f.duelFoe = ri;                                  // whoever is struck back at takes up the fight
	if (r.knockTimer > 0.0f)
		return;                                          // (npcKnockUpdate)
	float dx = f.pos[0] - r.pos[0], dy = f.pos[1] - r.pos[1], dist = sqrtf(dx * dx + dy * dy);
	float diff = angleDiff(atan2f(dx, dy), r.rot[2]);
	r.rot[2] += fmaxf(-kTurnSpeed * dt, fminf(kTurnSpeed * dt, diff));
	r.moved = true;
	if (a)
		a->showWeapon = wpn != nullptr;
	const float meleeAt = meleeReach(w, wpn, def.creature);
	float reach = meleeAt + actorHalfLength(ri) + actorHalfLength(fi);   // (npcCombat)
	bool attacking = r.hitAt >= 0.0f;
	Cell* cell = w.cellOf(ri);
	// (one that flies or swims: within its reach up or down too, npcCombat)
	float level = cell && zMoverTop(w, r, *cell) > -1e8f ? meleeAt : 180.0f;
	if (!attacking && (dist > reach * 0.9f || fabsf(f.pos[2] - r.pos[2]) >= level))
	{
		r.sideStep = 0;
		r.backingUp = false;
		npcMoveTo(ri, f.pos, actorRunSpeed(ri), dt, false);
		if (a && !actorPlay(*set, *a, stanceGroup(*set, *a, "RunForward", wpn).c_str(), ANIM_LOOP))
			actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
	}
	else
	{
		bool stepped = cell && combatMove(ri, *cell, dist, diff, false, w.game.object(fdef.weapon), dt);
		if (!stepped && a && (a->mode == ANIM_LOOP || a->mode == ANIM_IDLE))
		{
			// ready to strike: the weapon's stance (its idles otherwise, left to run on)
			std::string idle = stanceGroup(*set, *a, "Idle", wpn);
			if (idle != "Idle")
				actorPlay(*set, *a, idle.c_str(), ANIM_LOOP);
			else if (a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		r.attackTimer -= dt;
		if (!attacking && r.attackTimer <= 0.0f && fabsf(diff) < 0.6f)
		{
			float windup = 0.35f, length = 1.0f;
			if (a && actorPlay(*set, *a, blowGroup(*set, *a, r, wpn).c_str(), ANIM_ONCE))
			{
				const AnimGroup& g = actorSkeleton(*set, a->skeleton).groups[a->group];
				windup = g.loopStart - g.start;
				length = g.stop - g.start;
			}
			r.hitAt = w.time + windup;
			r.attackTimer = length + 0.4f + frand() * 0.8f / fmaxf(0.5f, wpn ? wpn->speed : 1.0f);
		}
	}
	if (r.hitAt >= 0.0f && w.time >= r.hitAt)
	{
		r.hitAt = -1.0f;
		if (dist < reach && fabsf(f.pos[2] - r.pos[2]) < meleeAt && fabsf(angleDiff(atan2f(dx, dy), r.rot[2])) < hitAngle(w))
		{
			float charge = frand();
			r.fatigue -= w.game.gmstf("ffatigueattackbase", 2.0f) + (wpn ? wpn->weight : 0.0f) * charge * w.game.gmstf("fweaponfatiguemult", 0.25f);
			float chance = roundf(attackTermOf(attackSkill(def, wpn), def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK],
				r.fatigue, r.fatigueMax, w.actorEffect(ri, 117), w.actorEffect(ri, 47))
				- npcDefense(fi, f.knockTimer > 0.0f || w.actorEffect(fi, 45) > 0.0f));
			BlowRoll roll = rollBlow(chance, false, false);
			if (a)
				attackFollowThrough(*set, *a, roll.lands ? charge : 0.0f);
			if (!roll.lands)
				playSound(ri, charge > 0.6f ? "SwishL" : "SwishM");
			else
			{
				blowSound(fi, roll);
				bool fatigueOnly = false;
				float dmg;
				if (def.creature && (wpn || !def.attack.empty()))
					dmg = creatureDamage(def, wpn, charge, r.attackKind) * roll.damage;
				else if (wpn)
					dmg = weaponDamage(wpn, r.attackKind, charge, def.attributes[ATTR_STRENGTH]) * roll.damage;
				else    // fists tire (health only on someone down)
				{
					float lo = w.game.gmstf("fminhandtohandmult", 0.1f), hi = w.game.gmstf("fmaxhandtohandmult", 0.5f);
					dmg = def.skills[SKILL_HAND_TO_HAND] * (lo + (hi - lo) * charge);
					if (f.knockTimer > 0.0f)
						dmg *= w.game.gmstf("fhandtohandhealthper", 0.1f);
					else
						fatigueOnly = true;
				}
				if (!fatigueOnly)
				{
					if (f.knockTimer > 0.0f && w.actorEffect(fi, 45) <= 0.0f)
						dmg *= w.game.gmstf("fcombatkodamagemult", 1.5f);
					dmg = fmaxf(1.0f, applyArmor(dmg, fdef.armor + w.actorEffect(fi, 3)));
				}
				logf("combat: %s hits %s for %.1f%s", r.id.c_str(), f.id.c_str(), dmg, fatigueOnly ? " fatigue" : "");
				if (!fatigueOnly && f.health - dmg <= 0.0f)
				{
					f.health = 0.0f;
					killNpc(fi, false);              // (a duel's death is nobody's murder)
				}
				else
					damageNpc(fi, dmg, fatigueOnly);
			}
		}
		else if (a)
			attackFollowThrough(*set, *a, frand());
		if (f.dead)
		{
			r.duelFoe = -1;
			if (a)
				a->showWeapon = false;
		}
	}
	w.syncActor(ri);
}

// Guards go for hostile creatures (OpenMW's engageCombat, as Morrowind does): a creature in a fight within fAlarmRadius
// of a Guard-class NPC, whose Fight towards the player reaches 100, unless the guard or the creature is calmed or the
// creature follows or escorts someone. The nearest such creature, or -1
int Session::guardFoe(int ri)
{
	const Ref& r = w.refs[ri];
	if (lower(w.game.actors[r.actor].cls) != "guard" || w.actorEffect(ri, 49) > 0.0f || r.calmUntil > w.time)
		return -1;
	const float radius = w.game.gmstf("falarmradius", 2000.0f);
	int best = -1;
	float bestD = radius * radius;
	for (int ci : w.loadedActors)
	{
		const Ref& c = w.refs[ci];
		if (ci == ri || c.actor < 0 || c.dead || !c.visible() || !w.active(ci) || c.ally || !w.game.actors[c.actor].creature)
			continue;
		if (c.ai != AI_COMBAT && c.duelFoe < 0)
			continue;                        // not fighting anyone
		if ((c.aiPackage == AIPKG_FOLLOW || c.aiPackage == AIPKG_ESCORT) && !c.aiDone)
			continue;
		if (w.actorEffect(ci, 50) > 0.0f || c.calmUntil > w.time || !w.sameSpace(ci, ri))
			continue;
		float dx = c.pos[0] - r.pos[0], dy = c.pos[1] - r.pos[1], dz = c.pos[2] - r.pos[2], d = dx * dx + dy * dy + dz * dz;
		if (d > bestD || fightTermOf(ci, w.distanceToPlayer(ci)) < 100.0f)
			continue;
		best = ci;
		bestD = d;
	}
	return best;
}

bool World::followsPlayer(int ri) const
{
	if (ri < 0 || ri >= (int)refs.size() || refs[ri].cell < 0 || refs[ri].dead || refs[ri].actor < 0)
		return false;
	const Ref& r = refs[ri];
	if ((r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.aiTarget == "player")
		return true;
	for (auto& e : effects)
		if (e.ref == ri)
			return true;
	return false;
}

void World::detachActor(int ri)
{
	Ref& r = refs[ri];
	ActorSet* set = r.anim >= 0 || r.cellActor >= 0 ? actorsOf(ri) : nullptr;
	Cell* cell = r.cellActor >= 0 ? cellOf(ri) : nullptr;
	if (set && r.anim >= 0 && r.anim < (int)set->actors.size())
		set->actors[r.anim].ref = -1;          // no longer drawn
	if (cell && r.cellActor >= 0 && r.cellActor < (int)cell->actors.size())
		cell->actors[r.cellActor].radius = 0.0f;   // nor in the way
	r.anim = r.cellActor = -1;
}

// A creature's soul goes into the smallest empty soul gem that holds it (capacity: the gem's value
// x fSoulgemMult)
bool World::trapSoul(const std::string& creature, int soul)
{
	int best = -1;
	float bestCap = 1e30f;
	for (size_t i = 0; i < inventory.size(); i++)
	{
		const InventoryItem& it = inventory[i];
		if (!it.soul.empty() || it.id.compare(0, 13, "misc_soulgem_") != 0)
			continue;
		const Object* o = game.object(it.id);
		float cap = (o ? o->value : 0) * game.gmstf("fsoulgemmult", 3.0f);
		if (cap >= soul && cap < bestCap)
		{
			best = i;
			bestCap = cap;
		}
	}
	if (best < 0)
		return false;
	std::string gem = inventory[best].id;
	removeItem(gem, 1);
	InventoryItem filled = { gem, 1, false };
	filled.soul = lower(creature);
	addStack(filled);
	return true;
}

// NPCs greet the player with a "Hello" voice line when they come close
void Session::greetings(float dt)
{
	// (looked up once per game data: a scan of every topic)
	static const Topic* hello = nullptr;
	static const GameData* helloData = nullptr;
	static u32 helloGen = ~0u;
	if (helloData != &w.game || helloGen != g_gameGeneration)
	{
		hello = w.game.topic("Hello");
		helloData = &w.game;
		helloGen = g_gameGeneration;
	}
	// Nobody greets during scripted scenes (controls taken away)
	if (!hello || !w.controlsEnabled)
		return;
	// OpenMW's updateGreetingState: an actor wandering, travelling or with no package, not in combat, swimming or
	// paralyzed, greets once when the player is within AI Hello x iGreetDistanceMultiplier, in sight and noticed; it
	// greets again only after the player has gone fGreetDistanceReset away
	float reset = w.game.gmstf("fgreetdistancereset", 512.0f);
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if (r.actor < 0 || !w.active(i) || !r.visible() || r.dead)
			continue;
		float d = w.distanceToPlayer(i);
		if (r.greeted && d >= reset)
			r.greeted = false;
		if (r.greeted || r.ai == AI_COMBAT || r.helloTimer > 0.0f || !sayDone(i) || (w.actorOf(i) && w.actorOf(i)->swimming)
			|| (r.aiPackage != AIPKG_NONE && r.aiPackage != AIPKG_WANDER && r.aiPackage != AIPKG_TRAVEL
				&& r.aiPackage != AIPKG_IDLE)
			|| w.actorEffect(i, 45) > 0.0f || r.knockTimer > 0.0f)
			continue;
		int helloSetting = r.hello >= 0 ? r.hello : w.game.actors[r.actor].hello;
		if (helloSetting <= 0 || d > helloSetting * w.game.gmstf("igreetdistancemultiplier", 6.0f))
			continue;
		float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f }, eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
		if (!w.lineOfSight(head, eye) || !awarenessCheck(i))
			continue;
		const Info* info = dialogueVoiced(w, *hello, i);
		r.greeted = true;
		r.helloTimer = 2.0f;
		if (info && !info->sound.empty())
			say(i, info->sound, dialogueSubstitute(w, info->text, i));
	}
}

// A held light burns down (OpenMW's updateEquippedLight): its time left (the stack's charge; -1 a fresh one) less the
// frame's; one that burns out is gone, and the next of the stack is lit
void Session::burnHeldLight(float dt)
{
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		InventoryItem& it = w.inventory[i];
		const Object* o = it.equipped ? w.game.object(it.id) : nullptr;
		if (!o || o->recType != REC_LIGH || o->lightTime <= 0.0f)
			continue;
		float left = (it.charge < 0.0f ? o->lightTime : it.charge) - dt;
		if (left > 0.0f)
		{
			it.charge = left;
			break;
		}
		logf("light: %s burned out", it.id.c_str());
		it.charge = -1.0f;
		if (--it.count <= 0)
			w.inventory.erase(w.inventory.begin() + i);
		break;
	}
}
