// The numbers behind repair, recharge, security, persuasion and the resistance roll, as functions of the player's
// stats and the item / NPC in question, so the game and the generated spec tests (tools/test/openmw_specgen.py,
// spec/combat.md, spec/derived.md) read the same code. Each is Morrowind's rule as OpenMW has it.
#include <algorithm>
#include <cmath>

#include "log.h"
#include "session.h"

// fFatigueBase - fFatigueMult x (1 - fatigue / max)
float Session::playerFatigueTerm()
{
	const PlayerStats& s = w.stats;
	return w.game.gmstf("ffatiguebase", 1.25f) - w.game.gmstf("ffatiguemult", 0.5f)
		* (1.0f - s.fatigue / fmaxf(1.0f, s.fatigueMax));
}

// Armorer hammer / prongs: success when the roll (0 to 99) is at most this
float Session::repairChance()
{
	const PlayerStats& s = w.stats;
	return (0.1f * s.attributes[ATTR_STRENGTH] + 0.1f * s.attributes[ATTR_LUCK] + s.skills[1]) * playerFatigueTerm();
}

// What a successful repair mends: fRepairAmountMult x the tool's quality x the roll, at least 1
int Session::repairAmount(float quality, int roll)
{
	return std::max(1, (int)(w.game.gmstf("frepairamountmult", 3.0f) * quality * roll));
}

// Soul gem recharge: success when the roll (0 to 99) is below this
float Session::rechargeChance()
{
	const PlayerStats& s = w.stats;
	float luck = 0.1f * s.attributes[ATTR_LUCK], intel = 0.2f * s.attributes[ATTR_INTELLIGENCE];
	luck = luck < 1.0f || luck > 10.0f ? 1.0f : luck;
	intel = fmaxf(1.0f, fminf(20.0f, intel));
	return (s.skills[9] + intel + luck) * playerFatigueTerm();
}

// A won recharge restores the soul x roll / chance
float Session::rechargeGain(int soul, int roll, float chance)
{
	return soul * (roll / chance);
}

// Lockpick: (0.2 Agility + 0.1 Luck + Security) x quality x fatigue, less fPickLockMult x the lock level; success when
// the roll (0 to 99) is at most this, and never when it isn't above 0
float Session::lockChance(int lockLevel, float quality)
{
	const PlayerStats& s = w.stats;
	float x = 0.2f * s.attributes[ATTR_AGILITY] + 0.1f * s.attributes[ATTR_LUCK] + s.skills[18];
	x *= quality * playerFatigueTerm();
	return x + w.game.gmstf("fpicklockmult", -1.0f) * lockLevel;
}

// Probe: (0.2 Agility + 0.1 Luck + Security + fTrapCostMult x the trap spell's cost) x quality x fatigue
float Session::trapChance(float trapCost, float quality)
{
	const PlayerStats& s = w.stats;
	float x = 0.2f * s.attributes[ATTR_AGILITY] + 0.1f * s.attributes[ATTR_LUCK] + s.skills[18];
	x += w.game.gmstf("ftrapcostmult", -1.0f) * trapCost;
	return x * quality * playerFatigueTerm();
}

// Training: base skill x iTrainingMod (at least 1), at the trainer's barter price
int Session::baseSkill(int k)
{
	return std::min(100, w.stats.skillCreation[k] + w.stats.skillBonus[k]);
}

int Session::trainPriceFor(int npcRef, int skill)
{
	int price = std::max(1, (int)(baseSkill(skill) * w.game.gmstf("itrainingmod", 10.0f)));
	return barterPrice(npcRef, price, true);
}

// Travel: distance / fTravelMult (a distance if that is 0) x (1 + followers), or fMagesGuildTravel indoors; at least 1;
// then the barter price
int Session::travelPriceFor(int npcRef, float distance, int followers, bool interior)
{
	int price;
	if (interior)
		price = (int)w.game.gmstf("fmagesguildtravel", 10.0f);
	else
	{
		float mult = w.game.gmstf("ftravelmult", 4000.0f);
		price = mult != 0.0f ? (int)(distance / mult) : (int)distance;
	}
	price *= 1 + followers;
	return barterPrice(npcRef, std::max(1, price), true);
}

// The chance (percent, before the roll) that a persuasion succeeds. kind: 0 admire, 1 intimidate, 2 taunt,
// 3..5 bribe 10 / 100 / 1000. Player and NPC ratings from Speechcraft / Mercantile, Personality, Luck, reputation
// and level, each x its fatigue term; the NPC's current disposition scales the difference
float Session::persuadeChance(int npcRef, int kind, float* parts)
{
	Ref& npc = w.refs[npcRef];
	const ActorDef& def = w.game.actors[npc.actor];
	const PlayerStats& s = w.stats;
	auto g = [&](const char* name, float fb) { return w.game.gmstf(name, fb); };
	float fatigueP = playerFatigueTerm();
	float persP = s.attributes[ATTR_PERSONALITY] / g("fpersonalitymod", 5.0f), luckP = s.attributes[ATTR_LUCK] / g("fluckmod", 10.0f);
	float repP = w.pcReputation * g("freputationmod", 1.0f), levelP = s.level * g("flevelmod", 5.0f);
	float p1 = (repP + luckP + persP + s.skills[25]) * fatigueP;                     // Speechcraft
	float p2 = p1 + levelP;
	float p3 = (s.skills[24] + luckP + persP) * fatigueP;                             // Mercantile
	float persN = def.attributes[ATTR_PERSONALITY] / g("fpersonalitymod", 5.0f), luckN = def.attributes[ATTR_LUCK] / g("fluckmod", 10.0f);
	float repN = def.reputation * g("freputationmod", 1.0f), levelN = def.level * g("flevelmod", 5.0f);
	float fatigueN = g("ffatiguebase", 1.25f) - g("ffatiguemult", 0.5f) * (1.0f - npc.fatigue / fmaxf(1.0f, npc.fatigueMax));
	float n1 = (repN + luckN + persN + def.skills[25]) * fatigueN;             // the NPC's ratings tire too
	float n2 = (levelN + repN + luckN + persN + def.skills[25]) * fatigueN;
	float n3 = (def.skills[24] + repN + luckN + persN) * fatigueN;
	int disp = w.disposition(npcRef);
	float d = 1.0f - 0.02f * abs(disp - 50);
	static const char* bribeMod[3] = { "fbribe10mod", "fbribe100mod", "fbribe1000mod" };
	static const float bribeFb[3] = { 35.0f, 75.0f, 150.0f };
	float t;
	if (kind == 1)
		t = d * (p2 - n2 + 50);
	else if (kind >= 3)
		t = d * (p3 - n3 + 50) + g(bribeMod[kind - 3], bribeFb[kind - 3]);
	else
		t = d * (p1 - n1 + 50);
	if (parts)
	{
		const float v[8] = { p1, p2, p3, n1, n2, n3, d, t };
		for (int i = 0; i < 8; i++)
			parts[i] = v[i];
	}
	return fmaxf(g("iperminchance", 5.0f), t);
}

// What a crime adds to the bounty (OpenMW's reportCrime): the GMST for each kind, and the value stolen x
// fCrimeStealing (at least 1) for theft
int Session::crimeBounty(int kind, int arg)
{
	switch (kind)
	{
	case CRIME_TRESPASS: return (int)w.game.gmstf("icrimetresspass", 5.0f);
	case CRIME_PICKPOCKET: return (int)w.game.gmstf("icrimepickpocket", 25.0f);
	case CRIME_ASSAULT: return (int)w.game.gmstf("icrimeattack", 40.0f);
	case CRIME_MURDER: return (int)w.game.gmstf("icrimekilling", 1000.0f);
	default: return std::max(1, (int)(arg * w.game.gmstf("fcrimestealing", 1.0f)));
	}
}

// ---- Combat (spec/combat.md; OpenMW combat.cpp, creaturestats.cpp getEvasion, mechanicsmanagerimp.cpp awarenessCheck,
// data-mw combat/local.lua applyStagger)

float Session::fatigueTermOf(float fatigue, float fatigueMax)
{
	float normalised = floorf(fatigueMax) == 0.0f ? 1.0f : fmaxf(0.0f, fatigue / fatigueMax);
	return w.game.gmstf("ffatiguebase", 1.25f) - w.game.gmstf("ffatiguemult", 0.5f) * (1.0f - normalised);
}

// (skill + Agility / 5 + Luck / 10) x fatigue + Fortify Attack - Blind
float Session::attackTermOf(int skill, int agility, int luck, float fatigue, float fatigueMax, float fortifyAttack, float blind)
{
	return (skill + agility / 5.0f + luck / 10.0f) * fatigueTermOf(fatigue, fatigueMax) + fortifyAttack - blind;
}

// Evasion (Agility / 5 + Luck / 10) x fatigue + Sanctuary (at most 100), unless knocked down, paralyzed or unaware;
// Chameleon and Invisibility each add min(100, fCombatInvisoMult x magnitude) whatever the state
float Session::playerDefense(bool defenseless)
{
	const PlayerStats& s = w.stats;
	float d = 0.0f;
	if (s.fatigue < 0.0f)
		return 0.0f;
	if (!defenseless)
		d = (s.attributes[ATTR_AGILITY] / 5.0f + s.attributes[ATTR_LUCK] / 10.0f) * fatigueTermOf(s.fatigue, s.fatigueMax)
			+ fminf(100.0f, w.effectTotal(42));
	float inviso = w.game.gmstf("fcombatinvisomult", 0.2f);
	d += fminf(100.0f, inviso * w.effectTotal(40)) + fminf(100.0f, inviso * w.effectTotal(39));
	return d;
}

float Session::npcDefense(int ri, bool defenseless)
{
	const Ref& r = w.refs[ri];
	if (r.fatigue < 0.0f || r.actor < 0)
		return 0.0f;
	const ActorDef& def = w.game.actors[r.actor];
	float d = 0.0f;
	if (!defenseless && w.actorEffect(ri, 45) <= 0.0f)                 // (not paralyzed)
		d = (def.attributes[ATTR_AGILITY] / 5.0f + def.attributes[ATTR_LUCK] / 10.0f) * fatigueTermOf(r.fatigue, r.fatigueMax)
			+ fminf(100.0f, w.actorEffect(ri, 42));                    // Sanctuary
	float inviso = w.game.gmstf("fcombatinvisomult", 0.2f);
	return d + fminf(100.0f, inviso * w.actorEffect(ri, 40)) + fminf(100.0f, inviso * w.actorEffect(ri, 39));
}

// A shield block: (Block + 0.2 Agility + 0.1 Luck) x (swing x fSwingBlockMult + fSwingBlockBase), x fBlockStillBonus when
// not moving forward, x fatigue; less the attacker's (skill + 0.2 Agility + 0.1 Luck) x fatigue; clamped to
// iBlockMinChance .. iBlockMaxChance. The roll (0..99) must be below it
float Session::blockChance(float blockSkill, float agility, float luck, float fatigue, float fatigueMax, bool still,
	float attackSkill, float attackAgility, float attackLuck, float attackFatigue, float attackFatigueMax, float swing)
{
	float blockTerm = (blockSkill + 0.2f * agility + 0.1f * luck)
		* (swing * w.game.gmstf("fswingblockmult", 1.0f) + w.game.gmstf("fswingblockbase", 1.0f));
	if (still)
		blockTerm *= w.game.gmstf("fblockstillbonus", 1.25f);
	blockTerm *= fatigueTermOf(fatigue, fatigueMax);
	float attackTerm = (attackSkill + 0.2f * attackAgility + 0.1f * attackLuck) * fatigueTermOf(attackFatigue, attackFatigueMax);
	int x = (int)(blockTerm - attackTerm);
	int lo = (int)w.game.gmstf("iblockminchance", 10.0f), hi = (int)w.game.gmstf("iblockmaxchance", 50.0f);
	return (float)std::max(lo, std::min(hi, x));
}

// Knockdown: the raw health damage (before armor) at least Agility x fKnockDownMult, and a roll (0..99) of at least
// Agility x iKnockDownOddsMult / 100 + iKnockDownOddsBase
float Session::knockdownOdds(int agility)
{
	return agility * w.game.gmstf("iknockdownoddsmult", 50.0f) * 0.01f + w.game.gmstf("iknockdownoddsbase", 50.0f);
}

// A Fire / Lightning / Frost Shield hurts whoever strikes its bearer in melee:
// save = (Destruction + 0.2 Willpower + 0.1 Luck) x 1.25 x (fatigue / max); x = max(0, save - roll) + the striker's
// resistance to the element (at most 100); damage = fElementalShieldMult x magnitude x (1 - x / 100)
float Session::elementalShieldDamage(float magnitude, int destruction, int willpower, int luck, float fatigue, float fatigueMax,
	float resistance, int roll)
{
	float normalised = floorf(fatigueMax) == 0.0f ? 1.0f : fmaxf(0.0f, fatigue / fatigueMax);
	float save = (destruction + 0.2f * willpower + 0.1f * luck) * 1.25f * normalised;
	float x = fminf(100.0f, fmaxf(0.0f, save - roll) + resistance);
	return w.game.gmstf("felementalshieldmult", 0.1f) * magnitude * (1.0f - 0.01f * x);
}

// Whether this observer notices the player now (OpenMW's awarenessCheck): the player's
//   x = sneak term x (fSneakDistanceBase + fSneakDistanceMultiplier x distance) x fatigue + Chameleon (+100 invisible)
// where the sneak term is fSneakSkillMult x Sneak + 0.2 Agility + 0.1 Luck + boots' weight x fSneakBootMult while sneaking
// (0 otherwise), against the observer's y = (Sneak + 0.2 Agility + 0.1 Luck - Blind) x fatigue x fSneakViewMult (player
// in front) or fSneakNoViewMult (behind). Noticed when the observer's roll (0..99, rerolled every 5 seconds) >= x - y
bool Session::awarenessCheck(int ri)
{
	Ref& r = w.refs[ri];
	if (r.dead || r.actor < 0 || !r.visible())
		return false;
	const PlayerStats& s = w.stats;
	float sneakTerm = 0.0f;
	if (w.player.sneaking)
	{
		float boots = 0.0f;
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->type == "ARMO" && o->subtype == 5)
						boots = o->weight;
		sneakTerm = w.game.gmstf("fsneakskillmult", 1.0f) * s.skills[19] + 0.2f * s.attributes[ATTR_AGILITY]
			+ 0.1f * s.attributes[ATTR_LUCK] + boots * w.game.gmstf("fsneakbootmult", -1.0f);
	}
	float dx = w.player.feet[0] - r.pos[0], dy = w.player.feet[1] - r.pos[1], dz = w.player.feet[2] - r.pos[2];
	float dist = sqrtf(dx * dx + dy * dy + dz * dz);
	float distTerm = w.game.gmstf("fsneakdistancebase", 0.5f) + w.game.gmstf("fsneakdistancemultiplier", 0.002f) * dist;
	float x = sneakTerm * distTerm * fatigueTermOf(s.fatigue, s.fatigueMax) + w.effectTotal(40);
	if (w.effectTotal(39) > 0.0f)
		x += 100.0f;
	const ActorDef& def = w.game.actors[r.actor];
	float obsTerm = def.skills[19] + 0.2f * def.attributes[ATTR_AGILITY] + 0.1f * def.attributes[ATTR_LUCK] - w.actorEffect(ri, 47);   // (minus Blind on them)
	// facing: the angle between where they look and the player
	float fx = sinf(r.rot[2]), fy = cosf(r.rot[2]);
	float flat = sqrtf(dx * dx + dy * dy);
	bool behind = flat > 0.0f && (fx * dx + fy * dy) / flat < 0.0f;
	float y = obsTerm * fatigueTermOf(r.fatigue, r.fatigueMax)
		* (behind ? w.game.gmstf("fsneaknoviewmult", 0.5f) : w.game.gmstf("fsneakviewmult", 1.5f));
	if (r.awarenessRoll < 0)
		r.awarenessRoll = rand() % 100;
	return r.awarenessRoll >= x - y;
}

// Disease on contact (OpenMW's diseaseContact): each disease the carrier has and the player doesn't, a chance of
// fDiseaseXferChance percent x (1 - (resistance - weakness) / 100); corprus by Resist Corprus, common by Resist Common
// Disease, blight by Resist Blight Disease
void Session::catchDiseases(int carrier)
{
	const Ref& r = w.refs[carrier];
	if (r.actor < 0)
		return;
	PlayerStats& s = w.stats;
	for (auto& d : w.game.actors[r.actor].diseases)
	{
		auto sp = w.game.spells.find(d);
		if (sp == w.game.spells.end() || std::find(s.spells.begin(), s.spells.end(), d) != s.spells.end())
			continue;
		bool corprus = false;
		for (auto& e : sp->second.effects)
			corprus |= e.effect == 132;
		float resist;
		if (corprus)
			resist = 1.0f - 0.01f * (w.effectTotal(96) - w.effectTotal(34));
		else if (sp->second.type == 3)
			resist = 1.0f - 0.01f * (w.effectTotal(94) - w.effectTotal(32));
		else if (sp->second.type == 2)
			resist = 1.0f - 0.01f * (w.effectTotal(95) - w.effectTotal(33));
		else
			continue;
		int x = (int)(w.game.gmstf("fdiseasexferchance", 10.0f) * 100.0f * resist);
		if (rand() % 10000 < x)
		{
			s.spells.push_back(d);
			float hp = s.health, mp = s.magicka, fp = s.fatigue;
			w.recomputeStats();
			s.health = fminf(s.healthMax, hp);
			s.magicka = fminf(s.magickaMax, mp);
			s.fatigue = fminf(s.fatigueMax, fp);
			notify("You have contracted " + sp->second.name + ".");
			logf("disease: caught %s from %s", d.c_str(), r.id.c_str());
		}
	}
}

// Fall damage (OpenMW's getFallDamage): nothing under fFallDamageDistanceMin; else
//   x = max(0, height - min - 1.5 Acrobatics - Jump) -> (fFallDistanceBase + fFallDistanceMult x x)
//       x (fFallAcroBase + fFallAcroMult x (100 - Acrobatics))
// The health actually lost is x x (1 - 0.25 x fatigue term); x above Acrobatics x fatigue term knocks down
float Session::fallDamage(float height, int acrobatics, float jump)
{
	float minH = w.game.gmstf("ffalldamagedistancemin", 400.0f);
	if (height < minH)
		return 0.0f;
	float x = fmaxf(0.0f, height - minH - 1.5f * acrobatics - jump);
	x = w.game.gmstf("ffalldistancebase", 0.0f) + w.game.gmstf("ffalldistancemult", 0.07f) * x;
	return x * (w.game.gmstf("ffallacrobase", 0.5f) + w.game.gmstf("ffallacromult", 0.015f) * (100.0f - acrobatics));
}

// ---- Movement (spec/movement.md; OpenMW mwclass/npc.cpp / creature.cpp getWalkSpeed, getRunSpeed)

// NPCs: fMinWalkSpeed + 0.01 x Speed x (fMaxWalkSpeed - fMinWalkSpeed); creatures the same with the ...Creature GMSTs;
// NPCs carry nothing that slows them here (their encumbrance is not tracked)
float Session::actorWalkSpeed(int ri)
{
	const Ref& r = w.refs[ri];
	if (r.actor < 0)
		return 100.0f;
	const ActorDef& def = w.game.actors[r.actor];
	float lo = w.game.gmstf(def.creature ? "fminwalkspeedcreature" : "fminwalkspeed", def.creature ? 5.0f : 100.0f);
	float hi = w.game.gmstf(def.creature ? "fmaxwalkspeedcreature" : "fmaxwalkspeed", def.creature ? 300.0f : 200.0f);
	return lo + 0.01f * def.attributes[ATTR_SPEED] * (hi - lo);
}

// NPCs: walk x (0.01 x Athletics x fAthleticsRunBonus + fBaseRunMultiplier); creatures run at their walking speed
float Session::actorRunSpeed(int ri)
{
	const Ref& r = w.refs[ri];
	if (r.actor < 0 || w.game.actors[r.actor].creature)
		return actorWalkSpeed(ri);
	const ActorDef& def = w.game.actors[r.actor];
	return actorWalkSpeed(ri) * (0.01f * def.skills[8] * w.game.gmstf("fathleticsrunbonus", 1.0f) + w.game.gmstf("fbaserunmultiplier", 1.75f));
}

// The player: walk fMinWalkSpeed + 0.01 x Speed x (fMaxWalkSpeed - fMinWalkSpeed); run walk x (0.01 x Athletics x
// fAthleticsRunBonus + fBaseRunMultiplier) (encumbrance applied by the caller)
float Session::walkSpeedFor(int speed)
{
	return w.game.gmstf("fminwalkspeed", 100.0f) + 0.01f * speed * (w.game.gmstf("fmaxwalkspeed", 200.0f) - w.game.gmstf("fminwalkspeed", 100.0f));
}

float Session::runSpeedFor(int speed, int athletics)
{
	return walkSpeedFor(speed) * (0.01f * athletics * w.game.gmstf("fathleticsrunbonus", 1.0f) + w.game.gmstf("fbaserunmultiplier", 1.75f));
}

// OpenMW's getJump: Acrobatics a (at most 50) and b (above 50):
//   x = (fJumpAcrobaticsBase + (a / 15) ^ fJumpAcroMultiplier + 3 b x fJumpAcroMultiplier + 64 x Jump)
//       x (fJumpEncumbranceBase + fJumpEncumbranceMultiplier x (1 - load)) x fJumpRunMultiplier (running) x fatigue term
//   speed = (x + 8.96 x 69.99 (gravity in units)) / 3;  0 over the carrying limit
float Session::jumpSpeedFor(int acrobatics, float jump, float load, bool running, float fatigueTerm)
{
	if (load > 1.0f)
		return 0.0f;
	float a = (float)acrobatics, b = 0.0f;
	if (a > 50.0f)
	{
		b = a - 50.0f;
		a = 50.0f;
	}
	float mult = w.game.gmstf("fjumpacromultiplier", 4.0f);
	float x = w.game.gmstf("fjumpacrobaticsbase", 128.0f) + powf(a / 15.0f, mult) + 3.0f * b * mult + jump * 64.0f;
	x *= w.game.gmstf("fjumpencumbrancebase", 0.5f) + w.game.gmstf("fjumpencumbrancemultiplier", 0.5f) * (1.0f - fminf(1.0f, load));
	if (running)
		x *= w.game.gmstf("fjumprunmultiplier", 1.0f);
	x *= fatigueTerm;
	return (x + 8.96f * 69.99125f) / 3.0f;
}

// Resist Normal Weapons less Weakness to Normal Weapons on an actor, percent (its abilities and the spells on it; data
// converted before const_effects existed: the record's resist_normal)
float Session::normalWeaponResist(int ri)
{
	const Ref& r = w.refs[ri];
	if (r.actor < 0)
		return 0.0f;
	const ActorDef& def = w.game.actors[r.actor];
	float v = w.actorEffect(ri, 98) - w.actorEffect(ri, 36);
	if (def.constEffects.empty())
		v += def.resistNormal;
	return v;
}

// ---- NPC AI (spec/npc-ai-behaviour.md; OpenMW aicombataction.cpp vanillaRateFlee, combat.cpp getFightTerm)
float Session::fleeRatingOf(int ri, float distance)
{
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	int flee = r.flee >= 0 ? r.flee : def.flee;
	if (flee >= 100)
		return (float)flee;
	float health = r.healthMax > 0.0f ? fmaxf(0.0f, r.health) / r.healthMax : 1.0f;
	float rating = (1.0f - health) * w.game.gmstf("faifleehealthmult", 7.0f) + flee * w.game.gmstf("faifleefleemult", 0.3f);
	if (rating != 0.0f)
		rating += w.game.gmstf("ifightdistancebase", 20.0f) - w.game.gmstf("ffightdistancemultiplier", 0.005f) * distance;
	return rating;
}

// Attack on sight (OpenMW's getFightTerm / isAggressive): Fight + int(iFightDistanceBase - fFightDistanceMultiplier x
// distance + (50 - disposition) x fFightDispMult), creatures counting disposition 50; aggressive at 100
float Session::fightTermOf(int ri, float distance)
{
	const Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	float bias = w.game.gmstf("ifightdistancebase", 20.0f) - w.game.gmstf("ffightdistancemultiplier", 0.005f) * distance;
	if (!def.creature)
		bias += (50.0f - w.disposition(ri)) * w.game.gmstf("ffightdispmult", 0.2f);
	return (float)((r.fight >= 0 ? r.fight : def.fight) + (int)bias);
}

// ---- Trading (spec/trading-and-services.md; OpenMW tradewindow.cpp haggle)
// The haggle target (the roll 1..100 must be at most it):
//   d = int(100 (price - offer) / price) buying, int(100 (offer - price) / offer) selling  (whole numbers)
//   pc = (fDispositionMod x (disposition - 50) + Mercantile + 0.1 Luck + 0.2 Personality) x fatigue term  (uncapped)
//   npc = (Mercantile + 0.1 Luck + 0.2 Personality) x their fatigue term
//   x = fBargainOfferMulti x d + fBargainOfferBase + int(pc - npc)
float Session::haggleChance(int merchantRef, int price, int offer, bool selling, int* dOut)
{
	const Ref& m = w.refs[merchantRef];
	const ActorDef& def = w.game.actors[m.actor];
	const PlayerStats& s = w.stats;
	int d = selling ? (offer != 0 ? 100 * (offer - price) / offer : 0) : (price != 0 ? 100 * (price - offer) / price : 0);
	if (dOut)
		*dOut = d;
	float disp = (float)std::max(0, std::min(100, w.disposition(merchantRef)));
	float pcTerm = (w.game.gmstf("fdispositionmod", 1.0f) * (disp - 50.0f) + s.skills[24] + 0.1f * s.attributes[ATTR_LUCK]
		+ 0.2f * s.attributes[ATTR_PERSONALITY]) * fatigueTermOf(s.fatigue, s.fatigueMax);
	float npcTerm = (def.skills[24] + 0.1f * def.attributes[ATTR_LUCK] + 0.2f * def.attributes[ATTR_PERSONALITY])
		* fatigueTermOf(m.fatigue, m.fatigueMax);
	return w.game.gmstf("fbargainoffermulti", -4.0f) * d + w.game.gmstf("fbargainofferbase", 50.0f) + (int)(pcTerm - npcTerm);
}
