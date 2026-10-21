// Effects on actors and the player over time: totals, ticking, ending
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

float World::actorEffect(int ri, int effect) const
{
	const Ref& r = refs[ri];
	float total = 0.0f;
	if (r.actor >= 0)
		for (auto& c : game.actors[r.actor].constEffects)
			if (c.first == effect)
				total += c.second;
	for (auto& t : r.effects)
		if (t.effect == effect)
			total += t.magnitude;
	return total;
}

float World::effectTotal(int effect) const
{
	float total = 0.0f;
	for (auto& a : effects)
		if (a.effect == effect)
			total += a.magnitude;
	for (auto& a : abilities)
		if (a.effect == effect)
			total += a.magnitude;
	return total;
}

void World::reapplyEffects()
{
	for (auto& a : effects)
		if (isFortify(a.effect))
			applyFortify(*this, a, 1.0f);
}

void Session::updateActorEffects(int ri, float dt)
{
	Ref& r = w.refs[ri];
	for (size_t i = 0; i < r.effects.size();)
	{
		const Ref::TimedEffect t = r.effects[i];
		float step = fminf(dt, t.remaining);
		float amount = t.magnitude * step;
		switch (t.effect)
		{
		case EFF_FIRE_DAMAGE: case EFF_SHOCK_DAMAGE: case EFF_FROST_DAMAGE: case EFF_DAMAGE_HEALTH: case EFF_POISON:
			if (!r.dead)
				damageNpc(ri, amount, false, true);
			break;
		case EFF_DAMAGE_FATIGUE: r.fatigue -= amount; break;
		case EFF_DAMAGE_MAGICKA: r.magicka = fmaxf(0.0f, r.magicka - amount); break;
		case EFF_RESTORE_HEALTH: r.health = fminf(r.healthMax, r.health + amount); break;
		case EFF_RESTORE_FATIGUE: r.fatigue = fminf(r.fatigueMax, r.fatigue + amount); break;
		case EFF_RESTORE_MAGICKA: r.magicka = fminf(r.magickaMax, r.magicka + amount); break;
		// Absorb: theirs becomes the player's
		case EFF_ABSORB_HEALTH:
			if (!r.dead)
				damageNpc(ri, amount, false, true);
			w.stats.health = fminf(w.stats.healthMax, w.stats.health + amount);
			break;
		case EFF_ABSORB_FATIGUE:
			r.fatigue -= amount;
			w.stats.fatigue = fminf(w.stats.fatigueMax, w.stats.fatigue + amount);
			break;
		case EFF_ABSORB_MAGICKA:
			r.magicka = fmaxf(0.0f, r.magicka - amount);
			w.stats.magicka = fminf(w.stats.magickaMax, w.stats.magicka + amount);
			break;
		default: break;
		}
		if (r.dead)
		{
			r.effects.clear();              // (death ends every spell on them)
			return;
		}
		r.effects[i].remaining -= step;
		if (r.effects[i].remaining <= 0.0f)
		{
			// A drain gives back what it took (the maximum was never touched); a fortify takes back what it gave, and
			// the current value may fall below 0
			const Ref::TimedEffect& end = r.effects[i];
			if (end.effect == EFF_DRAIN_HEALTH)
				r.health = fminf(r.healthMax, r.health + end.magnitude);
			else if (end.effect == EFF_DRAIN_FATIGUE)
				r.fatigue = fminf(r.fatigueMax, r.fatigue + end.magnitude);
			else if (end.effect == EFF_DRAIN_MAGICKA)
				r.magicka = fminf(r.magickaMax, r.magicka + end.magnitude);
			else if (end.effect == EFF_FORTIFY_HEALTH)
				r.healthMax -= end.magnitude, r.health -= end.magnitude;
			else if (end.effect == EFF_FORTIFY_FATIGUE)
				r.fatigueMax -= end.magnitude, r.fatigue -= end.magnitude;
			else if (end.effect == EFF_FORTIFY_MAGICKA)
				r.magickaMax -= end.magnitude, r.magicka -= end.magnitude;
			r.effects.erase(r.effects.begin() + i);
		}
		else
			i++;
	}
}

// Per-frame ticking of the player's active effects
void Session::updateEffects(float dt)
{
	PlayerStats& s = w.stats;
	bool statsChanged = false;
	// Sun Damage (vampires): outdoors by day, as strong as the sun is high; clouds, rain and storms
	// block some (fMagicSunBlockedMult)
	float sun = w.effectTotal(135);
	// (not while a menu is up: resting and waiting fast-forward the hours, the game is paused)
	if (sun > 0.0f && !playerDead && !menuOpen() && w.current >= 0 && !w.cells[w.current].interior)
	{
		float h = fmodf(w.gameHour, 24.0f);
		float light = h > 6.0f && h < 20.0f ? sinf(3.14159f * (h - 6.0f) / 14.0f) : 0.0f;
		if (w.weatherNext > WEATHER_CLOUDY)
			light *= w.game.gmstf("fmagicsunblockedmult", 0.5f);
		s.health -= sun * light * dt;
		if (light > 0.0f)
			hurtFlashOngoing();
	}
	for (size_t i = 0; i < w.effects.size();)
	{
		ActiveEffect& a = w.effects[i];
		float step = fminf(dt, a.remaining);
		switch (a.effect)
		{
		case EFF_RESTORE_HEALTH: s.health = fminf(s.healthMax, s.health + a.magnitude * step); break;
		case EFF_RESTORE_MAGICKA: s.magicka = fminf(s.magickaMax, s.magicka + a.magnitude * step); break;
		case EFF_RESTORE_FATIGUE: s.fatigue = fminf(s.fatigueMax, s.fatigue + a.magnitude * step); break;
		case EFF_DAMAGE_MAGICKA: s.magicka = fmaxf(0.0f, s.magicka - a.magnitude * step); break;
		case EFF_DAMAGE_ATTRIBUTE: case EFF_RESTORE_ATTRIBUTE:
			if (a.attribute >= 0 && a.attribute < 8)
			{
				float& d = s.attrDamage[a.attribute];
				int before = (int)ceilf(d - 0.001f);
				d = a.effect == EFF_DAMAGE_ATTRIBUTE ? d + a.magnitude * step : fmaxf(0.0f, d - a.magnitude * step);
				statsChanged |= (int)ceilf(d - 0.001f) != before;
			}
			break;
		case EFF_DAMAGE_SKILL: case EFF_RESTORE_SKILL:
			if (a.skill >= 0 && a.skill < 27)
			{
				float& d = s.skillDamage[a.skill];
				int before = (int)ceilf(d - 0.001f);
				d = a.effect == EFF_DAMAGE_SKILL ? d + a.magnitude * step : fmaxf(0.0f, d - a.magnitude * step);
				statsChanged |= (int)ceilf(d - 0.001f) != before;
			}
			break;
		case EFF_DISINTEGRATE_WEAPON: case EFF_DISINTEGRATE_ARMOR:
			for (auto& it : w.inventory)
				if (const Object* o = it.equipped ? w.game.object(it) : nullptr)
					if (o->type == (a.effect == EFF_DISINTEGRATE_WEAPON ? "WEAP" : "ARMO"))
					{
						wearItem(&it, a.magnitude * step);
						break;
					}
			break;
		case EFF_DAMAGE_FATIGUE: s.fatigue = fmaxf(0.0f, s.fatigue - a.magnitude * step); break;
		case EFF_FIRE_DAMAGE: case EFF_SHOCK_DAMAGE: case EFF_FROST_DAMAGE:
		case EFF_DAMAGE_HEALTH: case EFF_POISON:
			if (!playerDead)
			{
				s.health -= a.magnitude * step;
				if (a.magnitude > 0.0f)
					hurtFlashOngoing();
				if (s.health <= 0.0f)
					damagePlayer(0.0f, false);      // the death screen
			}
			break;
		default: break;
		}
		a.remaining -= dt;
		if (a.remaining <= 0.0f)
			endPlayerEffect(i);
		else
			i++;
	}
	// Damage / Restore Attribute or Skill moved a whole point: the numbers follow (current bars kept; magicka keeps its
	// share of the maximum, full from none, as refitMagicka)
	if (statsChanged)
	{
		float health = s.health, fatigue = s.fatigue;
		float share = s.magickaMax > 0.0f ? s.magicka / s.magickaMax : 1.0f;
		w.recomputeStats();
		s.health = fminf(s.healthMax, health);
		s.magicka = s.magickaMax * share;
		s.fatigue = fminf(s.fatigueMax, fatigue);
	}
}
