// Resisting magic: resistances and the resist roll
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

// Effects that only need to be there while they last: something else reads effectTotal (combat,
// detection, casting, the player's movement, resistances)
bool isPresence(int e)
{
	switch (e)
	{
	case EFF_WATER_BREATHING: case EFF_WATER_WALKING: case EFF_FIRE_SHIELD: case EFF_LIGHTNING_SHIELD:
	case EFF_FROST_SHIELD: case EFF_BURDEN: case EFF_FEATHER: case EFF_SLOW_FALL: case EFF_INVISIBILITY:
	case EFF_CHAMELEON: case EFF_LIGHT: case EFF_SANCTUARY: case EFF_NIGHT_EYE: case EFF_PARALYZE: case EFF_SILENCE:
	case EFF_BLIND: case EFF_SOUND: case EFF_TELEKINESIS: case EFF_DETECT_ANIMAL: case EFF_DETECT_ENCHANTMENT:
	case EFF_DETECT_KEY: case EFF_SPELL_ABSORPTION: case EFF_REFLECT: case EFF_DISINTEGRATE_WEAPON:
	case EFF_DISINTEGRATE_ARMOR: case EFF_CORPRUS: case EFF_STUNTED_MAGICKA: case EFF_SUN_DAMAGE: case EFF_VAMPIRISM:
		return true;
	default:
		return (e >= EFF_WEAKNESS_FIRE && e <= EFF_WEAKNESS_NORMAL_WEAPONS) || (e >= EFF_RESIST_FIRE && e <= EFF_RESIST_PARALYSIS);
	}
}

// Which effect resists an incoming one, and which makes it stronger (OpenMW's getResistanceEffect / WeaknessEffect):
// fire, frost, shock, poison, paralysis and the diseases have their own; drain, damage (not fire, frost or
// shock), absorb, weakness, burden, charm, silence, blind, sound, calm, frenzy, demoralize, rally and turn
// undead go by Resist / Weakness to Magicka; nothing else can be resisted
bool resistedBy(int e, int& res, int& weak)
{
	weak = -1;
	switch (e)
	{
	case EFF_FIRE_DAMAGE: res = EFF_RESIST_FIRE; weak = EFF_WEAKNESS_FIRE; return true;
	case EFF_FROST_DAMAGE: res = EFF_RESIST_FROST; weak = EFF_WEAKNESS_FROST; return true;
	case EFF_SHOCK_DAMAGE: res = EFF_RESIST_SHOCK; weak = EFF_WEAKNESS_SHOCK; return true;
	case EFF_POISON: res = EFF_RESIST_POISON; weak = EFF_WEAKNESS_POISON; return true;
	case EFF_PARALYZE: res = EFF_RESIST_PARALYSIS; return true;
	case EFF_VAMPIRISM: res = EFF_RESIST_COMMON_DISEASE; weak = EFF_WEAKNESS_COMMON_DISEASE; return true;
	case EFF_CORPRUS: res = EFF_RESIST_CORPRUS; weak = EFF_WEAKNESS_CORPRUS; return true;
	default: break;
	}
	bool magicka = e == EFF_BURDEN || e == EFF_CHARM || e == EFF_SILENCE || e == EFF_BLIND || e == EFF_SOUND
		|| e == EFF_TURN_UNDEAD || (e >= EFF_DRAIN_ATTRIBUTE && e <= EFF_DAMAGE_SKILL)
		|| (e >= EFF_WEAKNESS_FIRE && e <= EFF_WEAKNESS_NORMAL_WEAPONS)
		|| (e >= EFF_CALM_HUMANOID && e <= EFF_RALLY_CREATURE) || (e >= EFF_ABSORB_ATTRIBUTE && e <= EFF_ABSORB_SKILL);
	if (!magicka)
		return false;
	res = EFF_RESIST_MAGICKA;
	weak = EFF_WEAKNESS_MAGICKA;
	return true;
}

float Session::resistBase(int effect)
{
	int res, weak;
	if (!resistedBy(effect, res, weak))
		return 0.0f;
	float r = w.effectTotal(res) - (weak >= 0 ? w.effectTotal(weak) : 0.0f);
	if (effect == EFF_FIRE_DAMAGE) r += w.effectTotal(EFF_FIRE_SHIELD);
	if (effect == EFF_FROST_DAMAGE) r += w.effectTotal(EFF_FROST_SHIELD);
	if (effect == EFF_SHOCK_DAMAGE) r += w.effectTotal(EFF_LIGHTNING_SHIELD);
	return r;
}

// An actor's resist less weakness to an effect (OpenMW's getEffectResistanceAttribute), e.g. the element an
// elemental shield burns its attacker with
float Session::actorResistBase(int ri, int effect)
{
	int res, weak;
	if (!resistedBy(effect, res, weak))
		return 0.0f;
	return w.actorEffect(ri, res) - (weak >= 0 ? w.actorEffect(ri, weak) : 0.0f);
}

float Session::resistX()
{
	const PlayerStats& s = w.stats;
	return (s.attributes[ATTR_WILLPOWER] + 0.1f * s.attributes[ATTR_LUCK]) * playerFatigueTerm() * 0.5f;
}
