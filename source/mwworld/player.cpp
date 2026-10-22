// The player's race, class, birthsign and derived stats

#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 __ctru_heap_size;
#include <malloc.h>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "components/debug/debuglog.hpp"
#include "components/debug/profile.hpp"
#include "mwbase/sessionhelpers.hpp"

const RaceDef* World::race() const
{
	for (auto& r : game.races)
		if (strcasecmp(r.id.c_str(), stats.race.c_str()) == 0)
			return &r;
	return nullptr;
}

const ClassDef* World::playerClass() const
{
	if (!stats.customClass.id.empty() && strcasecmp(stats.customClass.id.c_str(), stats.cls.c_str()) == 0)
		return &stats.customClass;
	for (auto& c : game.classes)
		if (strcasecmp(c.id.c_str(), stats.cls.c_str()) == 0)    // (Athletics asks every frame a run lasts: no copies)
			return &c;
	return nullptr;
}

const BirthDef* World::birthsign() const
{
	for (auto& b : game.birthsigns)
		if (strcasecmp(b.id.c_str(), stats.birthsign.c_str()) == 0)
			return &b;
	return nullptr;
}

// Character creation rules: race base attributes, +10 for the class's favored ones;
// skills 5, minors +10, majors +25, race bonuses, +5 for the class specialization;
// constant abilities from race and birthsign on top.
void World::recomputeStats()
{
	const RaceDef* r = race();
	const ClassDef* c = playerClass();
	const BirthDef* b = birthsign();
	std::fill(std::begin(stats.attrBelow), std::end(stats.attrBelow), 0);
	std::fill(std::begin(stats.skillBelow), std::end(stats.skillBelow), 0);
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = r ? r->attributes[i][stats.female ? 1 : 0] : 40;
		if (c && (c->attributes[0] == i || c->attributes[1] == i))
			stats.attributes[i] += 10;
		stats.attrCreation[i] = stats.attributes[i];
	}
	for (int s = 0; s < 27; s++)
	{
		stats.skills[s] = 5;
		if (c)
		{
			for (int k = 0; k < 5; k++)
			{
				if (c->major[k] == s) stats.skills[s] += 25;
				if (c->minor[k] == s) stats.skills[s] += 10;
			}
			if (s < (int)game.skills.size() && game.skills[s].specialization == c->specialization)
				stats.skills[s] += 5;
		}
	}
	if (r)
		for (auto& bonus : r->skillBonus)
			if (bonus.first >= 0 && bonus.first < 27)
				stats.skills[bonus.first] += bonus.second;
	for (int s = 0; s < 27; s++)
		stats.skillCreation[s] = stats.skills[s];

	float magickaMult = game.gmstf("fpcbasemagickamult", 1.0f);
	std::vector<std::string> abilities;
	if (r) abilities.insert(abilities.end(), r->spells.begin(), r->spells.end());
	// Worn constant-effect enchantments count as abilities while worn
	for (auto& it : inventory)
		if (it.equipped)
			if (const SpellDef* en = enchantmentOf(it))
				if (en->type == ENCH_CONSTANT)
					abilities.push_back(en->id);
	if (b) abilities.insert(abilities.end(), b->spells.begin(), b->spells.end());
	// The player's own abilities and diseases (AddSpell, caught from creatures) work the same way
	for (auto& id : stats.spells)
	{
		auto sp = game.spells.find(lower(id));
		if (sp != game.spells.end() && sp->second.type >= 1 && sp->second.type <= 3)
			abilities.push_back(id);
	}
	this->abilities.clear();
	// Abilities' and worn constant enchantments' fortifies and drains are modifiers on top (OpenMW's modified value):
	// only the base is held to 100, so the Boots of Blinding Speed take Speed past it
	int attrMod[8] = {}, skillMod[27] = {};
	for (auto& id : abilities)
	{
		auto it = game.spells.find(lower(id));
		if (it == game.spells.end() || ((it->second.type < 1 || it->second.type > 3) && it->second.type != ENCH_CONSTANT))
			continue;                                  // abilities, diseases, worn constant enchantments
		// Corprus: its drains and fortifies grow a point a day since it was caught
		int scale = 1;
		for (auto& e : it->second.effects)
			if (e.effect == 132)
				scale = corprusLevel();
		for (const SpellEffect& e0 : it->second.effects)
		{
			SpellEffect e = e0;
			if (e.effect == 132 && corprusSince < -1.5f)
				continue;                                  // cured: the Corprus effect is gone
			if (e.effect == 17 || e.effect == 79)
				e.min *= scale;
			if (e.effect == 79 && e.attribute >= 0 && e.attribute < 8) attrMod[e.attribute] += e.min;
			else if (e.effect == 83 && e.skill >= 0 && e.skill < 27) skillMod[e.skill] += e.min;
			else if (e.effect == 84) magickaMult += e.min / 10.0f;
			else if (e.effect == 17 && e.attribute >= 0 && e.attribute < 8) attrMod[e.attribute] -= e.min;
			else if (e.effect == 21 && e.skill >= 0 && e.skill < 27) skillMod[e.skill] -= e.min;
			else          // resistances, weaknesses, night eye ...: read while they last (for good)
				this->abilities.push_back({ e.effect, e.attribute, e.skill, (float)e.min, 1e9f, it->second.name });
		}
	}
	for (int i = 0; i < 8; i++)
	{
		stats.attributes[i] = (stats.attributes[i] + stats.attrBonus[i] > 100 ? 100 : stats.attributes[i] + stats.attrBonus[i]) + attrMod[i];
		// OpenMW cuts (base - damage) to a whole number, so part of a point of damage takes a whole point
		stats.attributes[i] = std::max(0, stats.attributes[i] - (int)ceilf(stats.attrDamage[i] - 0.001f));
	}
	for (int k = 0; k < 27; k++)
	{
		stats.skills[k] = (stats.skills[k] + stats.skillBonus[k] > 100 ? 100 : stats.skills[k] + stats.skillBonus[k]) + skillMod[k];
		stats.skills[k] = std::max(0, stats.skills[k] - (int)ceilf(stats.skillDamage[k] - 0.001f));
	}
	const int* a = stats.attributes;
	// OpenMW: base health is fixed at creation (floor of half Strength + Endurance) and grows only by the level-up
	// gains; raised, fortified or drained attributes do not move it (spec/derived.md)
	stats.healthMax = floorf(0.5f * (stats.attrCreation[ATTR_STRENGTH] + stats.attrCreation[ATTR_ENDURANCE])) + stats.healthBonus;
	stats.magickaMult = magickaMult;
	stats.magickaFortify = stats.magickaFlat = 0.0f;     // (reapplyEffects adds the active spells' back)
	stats.magickaMax = a[ATTR_INTELLIGENCE] * magickaMult;
	stats.fatigueMax = a[ATTR_STRENGTH] + a[ATTR_WILLPOWER] + a[ATTR_AGILITY] + a[ATTR_ENDURANCE];
	stats.health = stats.healthMax;
	stats.magicka = stats.magickaMax;
	stats.fatigue = stats.fatigueMax;
	reapplyEffects();
}

// Paralyzed: no moving, no fighting
void Session::playerControls(PlayerInput& in, bool menu)
{
	if (w.effectTotal(45) > 0.0f)
	{
		in.moveX = in.moveY = 0.0f;
		in.jump = in.attack = in.cast = false;
	}
	if (w.forceSneak || testSneak)
		in.sneak = true;
	w.player.waterWalk = w.effectTotal(2) > 0.0f;
	w.player.slowFall = w.effectTotal(11) > 0.0f;
	w.player.levitate = w.levitationDisabled ? 0.0f : w.effectTotal(10);     // (DisableLevitation: no flying)
	w.player.swimBoost = w.effectTotal(1);
	// Encumbrance: the load (plus Burden, less Feather) against fEncumbranceStrMult x Strength; over it
	// the player can't move, under it the load slows them (fEncumberedMoveEffect)
	{
		// (the pack weighed every fourth frame: a lookup by id per item each frame; a pickup shows within a few frames)
		static float packWeight = 0.0f;
		if (w.frame % 4 == 0 || w.frame < 4 || !cadenceOn(CAD_WEIGHT))
		{
			packWeight = 0.0f;
			for (auto& it : w.inventory)
				if (const Object* o = w.game.object(it))
					packWeight += o->weight * it.count;
		}
		float weight = w.effectTotal(7) - w.effectTotal(8) + packWeight;
		float capacity = w.game.gmstf("fencumbrancestrmult", 5.0f) * w.stats.attributes[ATTR_STRENGTH];
		float load = capacity > 0.0f ? fmaxf(0.0f, weight) / capacity : 0.0f;
		bool over = load > 1.0f;
		if (over && !overEncumbered)
			notify(w.game.gmst("snotifymessage59", "You are carrying too much to move."));
		overEncumbered = over;
		// (OpenMW: over the limit nothing moves, levitating or not)
		w.player.loadSpeed = over ? 0.0f : 1.0f - w.game.gmstf("fencumberedmoveeffect", 0.3f) * fminf(1.0f, load);
		playerLoad = fminf(1.0f, load);
		// Storms (ash, blight, blizzard) blow out from Red Mountain: walking into them is slower,
		// by fStromWalkMult x the angle between the wind and the way the player goes (OpenMW)
		int wx = w.weatherNow;
		if ((wx == 6 || wx == 7 || wx == 9) && w.current >= 0 && w.cells[w.current].underSky()
			&& (fabsf(in.moveX) + fabsf(in.moveY)) > 0.1f)
		{
			float sx = w.player.feet[0] - 20480.0f, sy = w.player.feet[1] - 69632.0f;     // from Red Mountain
			float sl = sqrtf(sx * sx + sy * sy);
			float yaw = w.player.yaw, mx = sinf(yaw) * in.moveY + cosf(yaw) * in.moveX,
				my = cosf(yaw) * in.moveY - sinf(yaw) * in.moveX;
			float ml = sqrtf(mx * mx + my * my);
			if (sl > 1.0f && ml > 0.01f)
			{
				float c = fmaxf(-1.0f, fminf(1.0f, (sx * mx + sy * my) / (sl * ml)));
				w.player.loadSpeed *= 1.0f - w.game.gmstf("fstromwalkmult", 0.25f) * (acosf(c) / 3.14159265f);
			}
		}

		// Speed from the stats (OpenMW's reading of Morrowind): walking by the Speed attribute between
		// fMinWalkSpeed and fMaxWalkSpeed, running that times fBaseRunMultiplier plus Athletics; swimming
		// and sneaking fractions of it; the jump by Acrobatics, Jump, the load, running and fatigue
		const PlayerStats& st = w.stats;
		float speedAttr = (float)st.attributes[ATTR_SPEED], athletics = (float)st.skills[8];
		float walk = walkSpeedFor((int)speedAttr);
		float run = runSpeedFor((int)speedAttr, (int)athletics);          // (mwmechanics/formulas.cpp)
		w.player.runSpeed = run;
		// Levitating: fMinFlySpeed + 0.01 x (Speed + Levitate) x (fMaxFlySpeed - fMinFlySpeed) (OpenMW's getMaxSpeed)
		w.player.flySpeed = w.game.gmstf("fminflyspeed", 5.0f) + 0.01f * (speedAttr + w.player.levitate)
			* (w.game.gmstf("fmaxflyspeed", 60.0f) - w.game.gmstf("fminflyspeed", 5.0f));
		w.player.swimFactor = (0.01f * athletics * w.game.gmstf("fswimrunathleticsmult", 0.1f)
			+ w.game.gmstf("fswimrunbase", 0.5f));
		w.player.sneakFactor = walk * w.game.gmstf("fsneakspeedmultiplier", 0.75f) / fmaxf(1.0f, run);
		bool running = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY) > 0.75f;      // (full tilt)
		// in the air the pad steers fJumpMoveBase + fJumpMoveMult x Acrobatics / 100 of the run speed, at most all of it
		w.player.airControl = fminf(1.0f, w.game.gmstf("fjumpmovebase", 0.5f) + w.game.gmstf("fjumpmovemult", 0.5f) * st.skills[20] / 100.0f);
		w.player.jumpSpeed = jumpSpeedFor(st.skills[20], w.effectTotal(9), load, running, fatigueTermOf(st.fatigue, st.fatigueMax));
	}
	if (w.current >= 0 && !w.cells[w.current].interior)
		memcpy(w.lastOutside, w.player.feet, sizeof(w.lastOutside));
	// Invisibility ends when the player attacks or casts
	if ((in.attack || in.cast) && !menu)
		for (size_t i = 0; i < w.effects.size();)
			if (w.effects[i].effect == 39)
				w.effects.erase(w.effects.begin() + i);
			else
				i++;
	if (menu || !w.controlsEnabled)
	{
		in.moveX = in.moveY = 0.0f;
		in.jump = false;
		if (menu)
			in.lookX = in.lookY = in.gyroYaw = in.gyroPitch = 0.0f;
	}
	if (!w.jumpingEnabled)
		in.jump = false;
	if (w.controlsOff & 1u)
		in.lookX = in.lookY = in.gyroYaw = in.gyroPitch = 0.0f;        // DisablePlayerLooking
}

void Session::playerMove(const PlayerInput& in, float dt, u64& t)
{
	bool jumped = in.jump && w.player.onGround && !w.player.flying;
	Scene scene = w.scene();
	profAdd(PROF_OTHER, t);
	playerUpdate(w.player, scene, in, dt);
	profAdd(PROF_PLAYER, t);
	if (jumped)
		useSkill(20, 0);          // Acrobatics
	// Under water: fHoldBreathTime to come up, then fSuffocationDamage a second (Water Breathing: never)
	{
		Cell& here = w.here();
		bool under = w.player.swimming && here.hasWater() && playerEyeZ(w.player) < here.waterZ;
		if (!under || w.effectTotal(0) > 0.0f)
			breath = -1.0f;
		else
		{
			if (breath < 0.0f)
				breath = w.game.gmstf("fholdbreathtime", 20.0f);
			breath -= dt;
			if (breath <= 0.0f)
			{
				breath = 0.0f;
				w.stats.health -= w.game.gmstf("fsuffocationdamage", 3.0f) * dt;
				hurtFlashOngoing();
			}
		}
	}
	// A hard landing hurts (OpenMW's reading of Morrowind): past fFallDamageDistanceMin, less with
	// Acrobatics and Jump, and teaches Acrobatics
	if (w.player.landedFall > 0.0f)
	{
		float h = w.player.landedFall;
		w.player.landedFall = 0.0f;
		if (h > 4.0f && !w.player.swimming)
			playSound(-1, "DefaultLand");      // OpenMW: every landing makes this one, a jump's too
		float x = w.player.swimming ? 0.0f : fallDamage(h, w.stats.skills[20], w.effectTotal(9));   // (mwmechanics/formulas.cpp)
		if (x > 0.0f)
		{
			float ft = fatigueTermOf(w.stats.fatigue, w.stats.fatigueMax);
			logf("fall: %.0f units, %.1f damage", h, x * (1.0f - 0.25f * ft));
			damagePlayer(x * (1.0f - 0.25f * ft), false);
			// a fall past Acrobatics x fatigue knocks the player down; a softer one trains Acrobatics
			if (x > w.stats.skills[20] * ft)
				w.player.knockTimer = fmaxf(w.player.knockTimer, 2.0f);
			else
				useSkill(20, 1);
		}
	}
	// Athletics: per second spent running (or swimming)
	if (!w.player.flying && (fabsf(in.moveX) + fabsf(in.moveY)) > 0.5f)
		useSkill(8, w.player.swimming ? 1 : 0, dt);
	// Fatigue spent (fFatigue*Base + load x fFatigue*Mult): running at full tilt, swimming, sneaking
	// per second, and each jump
	{
		float tilt = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY);
		const char* kind = nullptr;
		if (w.player.swimming && tilt > 0.2f)
			kind = tilt > 0.75f ? "swimrun" : "swimwalk";
		else if (w.player.sneaking && tilt > 0.2f)
			kind = "sneak";
		else if (tilt > 0.75f && w.player.onGround && !w.player.flying && w.player.levitate <= 0.0f)
			kind = "run";
		if (kind)
		{
			static const float defBase[4] = { 5.0f, 2.5f, 2.0f, 1.5f }, defMult[4] = { 2.0f, 4.0f, 1.5f, 2.0f };
			int k = !strcmp(kind, "run") ? 0 : !strcmp(kind, "swimrun") ? 1 : !strcmp(kind, "sneak") ? 2 : 3;
			// (the names as fixed texts: gmstf's cache keys on the text's address)
			static const char* const kBase[4] = { "ffatiguerunbase", "ffatigueswimrunbase", "ffatiguesneakbase", "ffatigueswimwalkbase" };
			static const char* const kMult[4] = { "ffatiguerunmult", "ffatigueswimrunmult", "ffatiguesneakmult", "ffatigueswimwalkmult" };
			float cost = w.game.gmstf(kBase[k], defBase[k]) + playerLoad * w.game.gmstf(kMult[k], defMult[k]);
			w.stats.fatigue = fmaxf(0.0f, w.stats.fatigue - cost * dt);
		}
		if (w.player.jumpedNow)
			w.stats.fatigue = fmaxf(0.0f, w.stats.fatigue - (w.game.gmstf("ffatiguejumpbase", 5.0f)
				+ playerLoad * w.game.gmstf("ffatiguejumpmult", 0.0f)));
		w.player.jumpedNow = false;
	}
}
