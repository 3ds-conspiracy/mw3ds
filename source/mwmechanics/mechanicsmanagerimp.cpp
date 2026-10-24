// Crime, detection, disposition, stolen items and bartering prices
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "components/debug/profile.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"
#include <3ds.h>
#include <cstdio>
#include <malloc.h>

bool Session::npcAware(int ri)
{
	// OpenMW: in combat, or the awareness check passes (not sneaking, and not invisible, a check that always passes)
	return w.refs[ri].ai == AI_COMBAT || awarenessCheck(ri);
}

// Four times a second everyone nearby may notice the player. Walking openly they see the player
// in front of them; sneaking, it is the player's Sneak against their eyes (farther and from
// behind is harder to notice).
void Session::updateDetection(float dt)
{
	detectTimer -= dt;
	if (detectTimer > 0.0f)
		return;
	detectTimer = 0.25f;
	// OpenMW: an observer in line of sight who passes the awareness check (mwmechanics/formulas.cpp) sees the player; the check's
	// roll is kept 5 seconds. Sneaking, the Sneak skill trains every fSneakUseDelay seconds while someone within
	// fSneakUseDist has the player in sight and nobody has noticed
	bool sneaking = w.player.sneaking;
	bool detected = false, avoidedNotice = false;
	float useDist = w.game.gmstf("fsneakusedist", 500.0f);
	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	w.forLoadedActors([&](int i) {
		Ref& r = w.refs[i];
		if (r.actor < 0 || r.dead || !r.visible())
			return;
		r.awarenessTimer += 0.25f;
		if (r.awarenessTimer >= 5.0f)
		{
			r.awarenessTimer = 0.0f;
			r.awarenessRoll = -1;
		}
		float d = w.distanceToPlayer(i);
		if (d > 2000.0f)
			return;
		float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f };
		if (!w.lineOfSight(head, eye))
			return;
		if (awarenessCheck(i))
		{
			r.lastSeen = w.time;
			if (d < useDist)
				detected = true;
		}
		else if (d < useDist)
			avoidedNotice = true;
	});
	hidden = sneaking && !detected;
	sneakSkillTimer += 0.25f;
	if (sneakSkillTimer >= w.game.gmstf("fsneakusedelay", 1.0f))
	{
		sneakSkillTimer = 0.0f;
		if (sneaking && avoidedNotice && !detected)
			useSkill(SKILL_SNEAK, 0);
	}
}

// ---- Crime

// A crime against one's own faction ends the membership
void Session::crimeAgainstFaction(const std::string& faction)
{
	std::string f = lower(faction);
	if (f.empty() || w.pcRankIn(f) < 0 || w.pcExpelled.count(f))
		return;
	w.pcExpelled.insert(f);
	notify("You have been expelled from the " + w.game.factionName(f) + ".");
	logf("crime: expelled from %s", f.c_str());
}

void Session::crimeSeen(int kind, int value, int victim)
{
	float radius = w.game.gmstf("falarmradius", 2000.0f);
	static const char* disp[5] = { "fdispstealing", "fdisppickpocketmod", "idisptresspass", "idispattackmod", "idispkilling" };
	static const float dispDef[5] = { -0.5f, -25.0f, -20.0f, -50.0f, -50.0f };
	static const char* fight[5] = { "ffightstealing", "ifightpickpocket", "ifighttrespass", "ifightattack", "ifightkilling" };
	static const float fightDef[5] = { 50.0f, 25.0f, 25.0f, 100.0f, 50.0f };
	float dispTerm = w.game.gmstf(disp[kind], dispDef[kind]) * (kind == CRIME_THEFT ? (float)value : 1.0f);
	float fightTerm = w.game.gmstf(fight[kind], fightDef[kind]);
	// a new crime id, given to everyone it stirs (OpenMW's reportCrime): they calm down once it is paid for
	int id = ++w.crimeIdNow;
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if (r.recType != REC_NPC_ || r.dead || r.ally || r.actor < 0 || !w.active(i) || w.time - r.lastSeen > 1.0f
			|| w.distanceToPlayer(i) > radius || r.ai == AI_COMBAT)
			continue;
		const ActorDef& def = w.game.actors[r.actor];
		if (lower(def.cls) == "guard")
			continue;                        // guards arrest instead
		int base = r.fight >= 0 ? r.fight : def.fight;
		// OpenMW's reportCrime: the urge to attack scales with the witness's Alarm / 100 (a pickpocket victim
		// with Alarm 0 counts as 100, other pickpocket witnesses as 0); an assault's witnesses use
		// iFightAttacking, its victim iFightAttack
		float alarmTerm = 0.01f * (float)(r.alarm >= 0 ? r.alarm : def.alarm);
		if (kind == CRIME_PICKPOCKET)
			alarmTerm = i == victim ? (alarmTerm == 0.0f ? 1.0f : alarmTerm) : 0.0f;
		float kindFight = kind == CRIME_ASSAULT && i != victim ? w.game.gmstf("ifightattacking", 50.0f) : fightTerm;
		float term = fmaxf(0.0f, fminf(kindFight * alarmTerm, 100.0f - base));
		if (i == victim || base + term >= 100.0f)
		{
			if (base + term >= 100.0f)
			{
				logf("crime: %s attacks (fight %d + %.0f)", r.id.c_str(), base, term);
				makeHostile(i);
			}
		}
		r.fight = base + (int)term;
		r.disposition += (int)dispTerm;
		r.crimeId = id;
		if (i != victim)
			r.crimeDisp += (int)dispTerm;       // (a witness's is given back then; the victim's stays, as OpenMW's permanent one)
	}
}

// A witness (or victim) of a crime now paid for calms down (OpenMW's Actors::updateActor, "if you've paid for your
// crimes"): out of the fight, Fight back to their own, no longer alarmed, the disposition the crime took given back
void Session::calmWitness(int ri)
{
	Ref& r = w.refs[ri];
	stopCombat(ri);
	r.fight = -1;
	r.alarmed = false;
	r.attacked = false;
	r.disposition -= r.crimeDisp;
	r.crimeDisp = 0;
	r.crimeId = -1;
	if (Actor* a = w.actorOf(ri))
		a->showWeapon = false;
	logf("crime: %s calms down, the crime paid for", r.id.c_str());
}

void Session::reportCrime(int victim, int amount)
{
	// A crime someone saw goes on record only if someone within the alarm radius reports it: an NPC
	// with AI Alarm 100 (guards and the like; most commoners have 0). OpenMW's reading.
	float radius = w.game.gmstf("falarmradius", 2000.0f);
	int reporter = -1;
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		if (r.recType != REC_NPC_ || r.dead || r.ally || r.actor < 0 || !w.active(i) || w.distanceToPlayer(i) > radius)
			continue;
		if ((r.alarm >= 0 ? r.alarm : w.game.actors[r.actor].alarm) >= 100)
		{
			reporter = i;
			break;
		}
	}
	if (reporter < 0)
	{
		logf("crime: +%d seen, nobody to report it", amount);
		return;
	}
	if (victim >= 0 && w.refs[victim].actor >= 0 && !w.game.actors[w.refs[victim].actor].creature)
	{
		crimeAgainstFaction(w.game.actors[w.refs[victim].actor].faction);
		// (the victim fighting back calms down with the witnesses once it is paid for: OpenMW sets its crime id when reported)
		if (!w.refs[victim].dead && w.crimeIdNow >= 0)
			w.refs[victim].crimeId = w.crimeIdNow;
	}
	w.bounty += amount;
	w.globals["pccrimelevel"] = (float)w.bounty;
	notify("Crime reported. Your bounty is " + std::to_string(w.bounty) + " gold.");
	logf("crime: +%d, bounty %d", amount, w.bounty);
}

// Seen by someone awake who noticed the player just now
int Session::crimeWitness(int except)
{
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		if (i != except && r.recType == REC_NPC_ && !r.dead && !r.ally && w.active(i) && w.time - r.lastSeen < 1.0f
			&& w.distanceToPlayer(i) < 2000.0f)
			return i;
	}
	return -1;
}

void Session::takeOwned(int from, const std::string& item, int count)
{
	const Object* o = w.game.object(item);
	if (from >= 0)
		w.markStolen(item, count, w.refs[from].recType == REC_NPC_ ? w.refs[from].idLower
			: !w.refs[from].owner.empty() ? w.refs[from].owner : w.refs[from].ownerFaction);
	else
		w.markStolen(item, count, "");
	int witness = crimeWitness();
	logf("theft: %s x%d from %s%s", item.c_str(), count, from >= 0 ? w.refs[from].id.c_str() : "?",
		witness >= 0 ? (", seen by " + w.refs[witness].id).c_str() : ", unseen");
	if (witness < 0)
		return;
	int value = o ? o->value * count : count;
	if (!npcSayTopic(witness, "Thief"))
		say(witness, "", w.game.gmst("scaughtstealingmessage", "Hey he's stealing my stuff!"));
	// Stealing from one's own faction (its goods, or a member's)
	if (from >= 0)
	{
		const Ref& src = w.refs[from];
		crimeAgainstFaction(src.ownerFaction);
		for (auto& a : w.game.actors)
			if (!src.owner.empty() && lower(a.id) == src.owner)
				crimeAgainstFaction(a.faction);
	}
	crimeSeen(CRIME_THEFT, value > 0 ? value : 1, -1);
	reportCrime(-1, crimeBounty(CRIME_THEFT, value > 0 ? value : 1));
}

// A guard who knows about the player's bounty comes over to arrest them (or, after they
// resisted, attacks)
void Session::guardCheck(int ri, float dt)
{
	Ref& g = w.refs[ri];
	float d = w.distanceToPlayer(ri);
	if (d > 1500.0f)
		return;
	// The last guard to stop the player is fighting them (Resist Arrest's StartCombat): OpenMW's startCombat sends the
	// other guards after the player too
	if (arrestingGuard >= 0 && arrestingGuard != ri && w.refs[arrestingGuard].ai == AI_COMBAT && !w.refs[arrestingGuard].dead)
		w.arrestDeclined = w.bounty;
	if (w.arrestDeclined == w.bounty)
	{
		makeHostile(ri);
		return;
	}
	float dx = w.player.feet[0] - g.pos[0], dy = w.player.feet[1] - g.pos[1];
	if (d > 170.0f)
	{
		g.rot[2] = atan2f(dx, dy);
		npcMoveTo(ri, w.player.feet, actorWalkSpeed(ri), dt, false);
		if (Actor* a = w.actorOf(ri))
			actorPlay(*w.actorsOf(ri), *a, "WalkForward", ANIM_LOOP);
		w.syncActor(ri);
	}
	else if (screen == SCR_NONE && messages.empty())
	{
		if (Actor* a = w.actorOf(ri))
			actorPlay(*w.actorsOf(ri), *a, "Idle", ANIM_IDLE);
		arrestingGuard = ri;
		g.alarmed = true;
		// OpenMW's AiPursue: the guard opens the conversation, and Greeting 0's crime lines decide: pay the fine, go to
		// jail or resist (its Choice), or, during character creation, let the theft go (PayFine takes the stolen goods
		// back and clears the bounty). The fixed arrest screen only when no greeting fits
		forceGreeting(ri);
		if (screen != SCR_DIALOGUE)
			openScreen(SCR_ARREST);
	}
}

// ---- Bartering and repairs

// Haggling (OpenMW's reading of Morrowind): how far the offer is from the price (d, percent) against the
// player's and the merchant's Mercantile, Luck and Personality (and disposition), each by fatigue
bool Session::haggleAccepted(int price, int offer)
{
	Ref& m = w.refs[barterRef];
	if (m.actor < 0 || price <= 0)
		return true;
	// OpenMW's haggle: an offer at least as good for the merchant as theirs is taken without a roll
	if (barterSell ? offer <= price : offer >= price)
		return true;
	const ActorDef& def = w.game.actors[m.actor];
	bool ok = false;
	int d = 0;
	float x = 0.0f;
	int roll = 0;
	if (!def.creature)                   // creatures never haggle
	{
		x = haggleChance(barterRef, price, offer, barterSell, &d);        // (mwmechanics/formulas.cpp)
		roll = (persuadeRoll >= 0 ? persuadeRoll : rand() % 100) + 1;       // (tests fix the die)
		ok = roll <= x;
	}
	if (!def.creature)
		m.disposition += (int)w.game.gmstf(ok ? "ibartersuccessdisposition" : "ibarterfaildisposition", ok ? 1.0f : -1.0f);
	logf("barter: price %d offer %d: d %d, x %.0f, roll %d -> %s", price, offer, d, x, roll, ok ? "accepted" : "refused");
	if (ok)
	{
		// Mercantile, scaled by how good the bargain was: floor(100 x the difference / the larger price)
		float scale = barterSell ? floorf(100.0f * (offer - price) / offer) : floorf(100.0f * (price - offer) / price);
		if (scale > 0.0f)
			useSkill(24, 0, scale);
	}
	return ok;
}

bool Session::isMerchant(int ri)
{
	const Ref& r = w.refs[ri];
	return r.actor >= 0 && !r.dead && (w.game.actors[r.actor].services & SERVICE_BARTER) != 0;
}

bool Session::merchantTrades(int ri, const Object* o)
{
	if (!o || o->value <= 0 || o->id == "gold_001" || lower(o->id) == "gold_001")
		return false;
	unsigned services = w.game.actors[w.refs[ri].actor].services;
	const std::string& t = o->type;
	unsigned need = t == "WEAP" ? SERVICE_WEAPON : t == "ARMO" ? SERVICE_ARMOR : t == "CLOT" ? SERVICE_CLOTHING
		: t == "BOOK" ? SERVICE_BOOKS : t == "INGR" ? SERVICE_INGREDIENTS : t == "LOCK" ? SERVICE_PICKS
		: t == "PROB" ? SERVICE_PROBES : t == "LIGH" ? SERVICE_LIGHTS : t == "APPA" ? SERVICE_APPARATUS
		: t == "REPA" ? SERVICE_REPAIR_ITEMS : t == "ALCH" ? SERVICE_POTIONS : t == "MISC" ? SERVICE_MISC : 0;
	return (services & need) != 0;
}

int Session::barterPrice(int ri, int value, bool buying)
{
	if (value == 0)
		return 0;                       // OpenMW: a free thing stays free
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	// a creature trades at the thing's own value, both ways (OpenMW's getBarterOffer: Creeper, the Mudcrab)
	if (def.creature)
		return value;
	const PlayerStats& s = w.stats;
	float disp = (float)w.disposition(ri);
	float pc = (disp - 50.0f + fminf(100.0f, s.skills[SKILL_MERCANTILE]) + fminf(10.0f, 0.1f * s.attributes[ATTR_LUCK])
		+ fminf(10.0f, 0.2f * s.attributes[ATTR_PERSONALITY])) * fatigueTerm(s.fatigue, s.fatigueMax);
	float npc = (fminf(100.0f, def.skills[SKILL_MERCANTILE]) + fminf(10.0f, 0.1f * def.attributes[ATTR_LUCK])
		+ fminf(10.0f, 0.2f * def.attributes[ATTR_PERSONALITY])) * fatigueTerm(r.fatigue, r.fatigueMax);
	float term = buying ? 0.01f * (100.0f - 0.5f * (pc - npc)) : 0.01f * (50.0f - 0.5f * (npc - pc));
	int price = (int)(value * term);
	return price < 1 ? 1 : price;
}

void World::markStolen(const std::string& item, int count, const std::string& owner)
{
	std::string id = lower(item), who = lower(owner);
	stolen[id] += count;
	if (who.empty())
		return;
	auto& owners = stolenFrom[id];
	for (auto& o : owners)
		if (o == who)
			return;
	owners.push_back(who);
}

void World::confiscateStolen()
{
	for (auto& s : stolen)
		removeItem(s.first, s.second);
	stolen.clear();
	stolenFrom.clear();
}

int World::factionReaction(const std::string& a, const std::string& b) const
{
	std::string la = lower(a), lb = lower(b);
	int v = 0;
	for (auto& f : game.factions)
		if (strcasecmp(f.first.c_str(), la.c_str()) == 0)      // (no copy of every faction's name per call)
		{
			auto r = f.second.reactions.find(lb);
			if (r != f.second.reactions.end())
				v = r->second;
		}
	auto m = factionReactions.find(la + "|" + lb);
	return v + (m != factionReactions.end() ? m->second : 0);
}

int World::disposition(int ri, bool clamp) const
{
	const Ref& r = refs[ri];
	if (r.actor < 0 || game.actors[r.actor].creature)
		return r.disposition;
	g_profCalls[PCALL_DISPOSITION]++;
	const ActorDef& a = game.actors[r.actor];
	float x = (float)r.disposition;
	if (!a.race.empty() && lower(a.race) == lower(stats.race))
		x += game.gmstf("fdispracemod", 5.0f);
	x += game.gmstf("fdisppersonalitymult", 0.5f) * (stats.attributes[ATTR_PERSONALITY] - game.gmstf("fdisppersonalitybase", 50.0f));
	// Faction: their faction's reaction to its own members, else to the player's factions (the worst)
	std::string fac = lower(a.faction);
	float reaction = 0.0f;
	int rank = 0;
	if (!fac.empty())
	{
		auto mine = pcRank.find(fac);
		if (mine != pcRank.end())
		{
			if (!pcExpelled.count(fac))
			{
				reaction = (float)factionReaction(fac, fac);
				rank = mine->second;
			}
		}
		else
		{
			bool first = true;
			for (auto& pf : pcRank)
			{
				if (pcExpelled.count(pf.first))
					continue;
				int re = factionReaction(fac, pf.first);
				if (first || re < reaction)
				{
					reaction = (float)re;
					rank = pf.second;
					first = false;
				}
			}
		}
	}
	x += (game.gmstf("fdispfactionrankmult", 0.5f) * rank + game.gmstf("fdispfactionrankbase", 1.0f))
		* game.gmstf("fdispfactionmod", 3.0f) * reaction;
	x -= game.gmstf("fdispcrimemod", 0.0f) * bounty;
	if (pcHasDisease(3) || pcHasDisease(2))
		x += game.gmstf("fdispdiseasemod", -10.0f);
	if (pcWeaponDrawn)
		x += game.gmstf("fdispweapondrawn", -5.0f);
	x += actorEffect(ri, 44);               // Charm (a spell effect on them: it ends)
	return clamp ? std::max(0, std::min(100, (int)x)) : (int)x;
}

bool World::ownedByOther(int ri) const
{
	if (ri < 0 || ri >= (int)refs.size())
		return false;
	const Ref& r = refs[ri];
	if (!r.ownerGlobal.empty())
	{
		auto g = globals.find(r.ownerGlobal);
		if (g != globals.end() && g->second != 0.0f)
			return false;             // rented
	}
	if (!r.owner.empty())
		return r.owner != "player";
	if (!r.ownerFaction.empty())
		return pcRankIn(r.ownerFaction) < r.ownerRank;
	return false;
}

// GetWeaponDrawn: the player's weapon (or fists) out; a person or creature fighting has its weapon out (their draw state
// follows the fight here)
bool Session::isWeaponDrawn(int ref)
{
	if (ref < 0)
		return weaponDrawn;
	const Ref& r = w.refs[ref];
	return r.actor >= 0 && !r.dead && r.ai == AI_COMBAT;
}
