// Spells, potions and ingredients: one effect system for all three.
//
// Casting (Y): the selected spell from the Magic screen. Chance, as in Morrowind:
//   (school skill x 2 + willpower / 5 + luck / 10 - spell cost) x fatigue term; powers always work.
// Self effects apply to the player, touch effects to the actor in front (within reach),
// target effects to the actor nearest the crosshair within kTargetRange.
//
// Effects the game understands; others are skipped (logged once):
//   restore / damage / drain / fire / frost / shock / poison health, magicka, fatigue (per second over
//   the duration, all at once for other actors), fortify attribute / skill / health / magicka /
//   fatigue / attack, shield (armor), jump, levitate (fly), swift swim.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "log.h"
#include "session.h"

enum
{
	EFF_SWIFT_SWIM = 1, EFF_SHIELD = 3, EFF_JUMP = 9, EFF_LEVITATE = 10,
	EFF_FIRE_DAMAGE = 14, EFF_SHOCK_DAMAGE = 15, EFF_FROST_DAMAGE = 16,
	EFF_DRAIN_HEALTH = 18, EFF_DRAIN_MAGICKA = 19, EFF_DRAIN_FATIGUE = 20,
	EFF_DAMAGE_HEALTH = 23, EFF_DAMAGE_MAGICKA = 24, EFF_DAMAGE_FATIGUE = 25, EFF_POISON = 27,
	EFF_RESTORE_HEALTH = 75, EFF_RESTORE_MAGICKA = 76, EFF_RESTORE_FATIGUE = 77,
	EFF_FORTIFY_ATTRIBUTE = 79, EFF_FORTIFY_HEALTH = 80, EFF_FORTIFY_MAGICKA = 81, EFF_FORTIFY_FATIGUE = 82,
	EFF_FORTIFY_SKILL = 83, EFF_FORTIFY_ATTACK = 117,
	EFF_WATER_BREATHING = 0, EFF_WATER_WALKING = 2, EFF_FIRE_SHIELD = 4, EFF_LIGHTNING_SHIELD = 5, EFF_FROST_SHIELD = 6,
	EFF_BURDEN = 7, EFF_FEATHER = 8, EFF_SLOW_FALL = 11, EFF_LOCK = 12, EFF_OPEN = 13, EFF_DRAIN_ATTRIBUTE = 17,
	EFF_DRAIN_SKILL = 21, EFF_DAMAGE_ATTRIBUTE = 22, EFF_DAMAGE_SKILL = 26,
	EFF_WEAKNESS_FIRE = 28, EFF_WEAKNESS_FROST = 29, EFF_WEAKNESS_SHOCK = 30, EFF_WEAKNESS_MAGICKA = 31,
	EFF_WEAKNESS_POISON = 35, EFF_WEAKNESS_NORMAL_WEAPONS = 36, EFF_DISINTEGRATE_WEAPON = 37, EFF_DISINTEGRATE_ARMOR = 38,
	EFF_INVISIBILITY = 39, EFF_CHAMELEON = 40, EFF_LIGHT = 41, EFF_SANCTUARY = 42, EFF_NIGHT_EYE = 43, EFF_CHARM = 44,
	EFF_PARALYZE = 45, EFF_SILENCE = 46, EFF_BLIND = 47, EFF_SOUND = 48, EFF_CALM_HUMANOID = 49, EFF_CALM_CREATURE = 50,
	EFF_FRENZY_HUMANOID = 51, EFF_FRENZY_CREATURE = 52, EFF_DEMORALIZE_HUMANOID = 53, EFF_DEMORALIZE_CREATURE = 54,
	EFF_RALLY_HUMANOID = 55, EFF_RALLY_CREATURE = 56, EFF_DISPEL = 57, EFF_SOULTRAP = 58, EFF_TELEKINESIS = 59, EFF_MARK = 60,
	EFF_RECALL = 61, EFF_DIVINE_INTERVENTION = 62, EFF_ALMSIVI_INTERVENTION = 63, EFF_DETECT_ANIMAL = 64,
	EFF_DETECT_ENCHANTMENT = 65, EFF_DETECT_KEY = 66, EFF_SPELL_ABSORPTION = 67, EFF_REFLECT = 68,
	EFF_CURE_COMMON_DISEASE = 69, EFF_CURE_BLIGHT_DISEASE = 70, EFF_CURE_CORPRUS = 71, EFF_CURE_POISON = 72,
	EFF_CURE_PARALYZATION = 73, EFF_RESTORE_ATTRIBUTE = 74, EFF_RESTORE_SKILL = 78, EFF_FORTIFY_MAX_MAGICKA = 84,
	EFF_ABSORB_ATTRIBUTE = 85, EFF_ABSORB_HEALTH = 86, EFF_ABSORB_MAGICKA = 87, EFF_ABSORB_FATIGUE = 88,
	EFF_ABSORB_SKILL = 89, EFF_RESIST_FIRE = 90, EFF_RESIST_FROST = 91, EFF_RESIST_SHOCK = 92, EFF_RESIST_MAGICKA = 93,
	EFF_RESIST_COMMON_DISEASE = 94, EFF_RESIST_BLIGHT_DISEASE = 95, EFF_RESIST_CORPRUS = 96, EFF_RESIST_POISON = 97,
	EFF_RESIST_NORMAL_WEAPONS = 98, EFF_RESIST_PARALYSIS = 99, EFF_REMOVE_CURSE = 100, EFF_TURN_UNDEAD = 101,
	EFF_COMMAND_CREATURE = 118, EFF_COMMAND_HUMANOID = 119,
	EFF_WEAKNESS_COMMON_DISEASE = 32, EFF_WEAKNESS_BLIGHT_DISEASE = 33, EFF_WEAKNESS_CORPRUS = 34, EFF_CORPRUS = 132,
	EFF_VAMPIRISM = 133, EFF_SUN_DAMAGE = 135, EFF_STUNTED_MAGICKA = 136,
};

static const int kSchoolSkill[6] = { 11, 13, 10, 12, 14, 15 };   // alteration .. restoration
static const char* kSchoolName[6] = { "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration" };
static const float kTargetRange = 3000.0f;

static float frand() { return (rand() % 1000) / 1000.0f; }

// Summoning effects (102-116, 134) and the creature each calls; bound items (120-131)
static const char* summonCreature(int e)
{
	static const char* ids[] = { "scamp_summon", "clannfear_summon", "daedroth_summon", "dremora_summon",
		"ancestor_ghost_summon", "skeleton_summon", "bonewalker_summon", "bonewalker_greater_summ", "bonelord_summon",
		"winged twilight_summon", "hunger_summon", "golden saint_summon", "atronach_flame_summon",
		"atronach_frost_summon", "atronach_storm_summon" };
	if (e >= 102 && e <= 116)
		return ids[e - 102];
	return e == 134 ? "centurion_sphere_summon" : nullptr;
}

static const char* boundItem(int e)
{
	switch (e)
	{
	case 120: return "bound_dagger";
	case 121: return "bound_longsword";
	case 122: return "bound_mace";
	case 123: return "bound_battle_axe";
	case 124: return "bound_spear";
	case 125: return "bound_longbow";
	case 127: return "bound_cuirass";
	case 128: return "bound_helm";
	case 129: return "bound_boots";
	case 130: return "bound_shield";
	case 131: return "bound_gauntlet_left";     // and the right one (below)
	default: return nullptr;
	}
}

static bool harmful(int e)
{
	return e == EFF_FIRE_DAMAGE || e == EFF_SHOCK_DAMAGE || e == EFF_FROST_DAMAGE || e == EFF_DRAIN_HEALTH
		|| e == EFF_DRAIN_MAGICKA || e == EFF_DRAIN_FATIGUE || e == EFF_DAMAGE_HEALTH || e == EFF_DAMAGE_MAGICKA
		|| e == EFF_DAMAGE_FATIGUE || e == EFF_POISON || e == EFF_DRAIN_ATTRIBUTE || e == EFF_DRAIN_SKILL
		|| e == EFF_DAMAGE_ATTRIBUTE || e == EFF_DAMAGE_SKILL || (e >= EFF_WEAKNESS_FIRE && e <= EFF_DISINTEGRATE_ARMOR)
		|| e == EFF_PARALYZE || e == EFF_SILENCE || e == EFF_BLIND || e == EFF_SOUND || e == EFF_BURDEN
		|| e == EFF_FRENZY_HUMANOID || e == EFF_FRENZY_CREATURE || e == EFF_DEMORALIZE_HUMANOID
		|| e == EFF_DEMORALIZE_CREATURE || (e >= EFF_ABSORB_ATTRIBUTE && e <= EFF_ABSORB_SKILL);
}

// Effects that only need to be there while they last: something else reads effectTotal (combat,
// detection, casting, the player's movement, resistances)
static bool isPresence(int e)
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
static bool resistedBy(int e, int& res, int& weak)
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

// The player's resistance to an incoming effect, percent (OpenMW's getEffectResistance; negative is weakness). The
// resisting effects less the weakening ones, shields for fire, frost and shock; then Willpower's roll: an effect
// with a magnitude is cut by a fraction of a percent when the roll under (Willpower + Luck / 10) x fatigue x 50 /
// the cast chance (taken as 100) lands, one without a magnitude (paralysis, silence ...) is turned aside whole
// with that chance, the player's resistance moving the roll. resistable false: this effect can't be resisted
// The roll itself, for any target: `total(effect)` gives the target's magnitude of an effect; castChance is the
// caster's (uncapped) chance for the spell, 100 for potions, enchantments and traps (x = ... x 50 / castChance)
template <class Total>
static float resistRoll(const World& w, int e, bool noMagnitude, Total total, float willpower, float luck, float fatigue,
	float fatigueMax, float castChance, bool& resistable)
{
	int res, weak;
	resistable = resistedBy(e, res, weak);
	if (!resistable)
		return 0.0f;
	float r = total(res) - (weak >= 0 ? total(weak) : 0.0f);
	if (e == EFF_FIRE_DAMAGE) r += total(EFF_FIRE_SHIELD);
	if (e == EFF_FROST_DAMAGE) r += total(EFF_FROST_SHIELD);
	if (e == EFF_SHOCK_DAMAGE) r += total(EFF_LIGHTNING_SHIELD);
	float normalised = floorf(fatigueMax) == 0.0f ? 1.0f : fmaxf(0.0f, fatigue / fatigueMax);
	float ft = w.game.gmstf("ffatiguebase", 1.25f) - w.game.gmstf("ffatiguemult", 0.5f) * (1.0f - normalised);
	float x = (willpower + 0.1f * luck) * ft;
	if (castChance > 0.0f)
		x *= 50.0f / castChance;
	float roll = frand() * 100.0f;
	if (noMagnitude)
		roll -= r;
	if (x <= roll)
		x = 0.0f;
	else
		x = noMagnitude ? 100.0f : roll / fminf(x, 100.0f);
	return fminf(x + r, 100.0f);
}

static float resistance(const World& w, int e, bool noMagnitude, float castChance, bool& resistable)
{
	const PlayerStats& s = w.stats;
	return resistRoll(w, e, noMagnitude, [&](int k) { return w.effectTotal(k); }, (float)s.attributes[ATTR_WILLPOWER],
		(float)s.attributes[ATTR_LUCK], s.fatigue, s.fatigueMax, castChance, resistable);
}

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

// Whether the effect changes a stat for as long as it lasts (undone when it ends)
static bool isFortify(int e)
{
	return e == EFF_FORTIFY_ATTRIBUTE || e == EFF_FORTIFY_SKILL || e == EFF_FORTIFY_HEALTH || e == EFF_FORTIFY_MAGICKA
		|| e == EFF_FORTIFY_FATIGUE || e == EFF_FORTIFY_ATTACK || e == EFF_SHIELD || e == EFF_JUMP
		|| e == EFF_LEVITATE || e == EFF_SWIFT_SWIM || e == EFF_DRAIN_ATTRIBUTE || e == EFF_DRAIN_SKILL
		|| e == EFF_ABSORB_ATTRIBUTE || e == EFF_ABSORB_SKILL || e == EFF_FORTIFY_MAX_MAGICKA;
}

// A lasting change to an attribute or skill: it never shows below 0, the part below 0 is owed back first
static void shiftStat(int& value, int& below, int by)
{
	int total = value + below + by;
	value = std::max(0, total);
	below = std::min(0, total);
}

// Adds (sign 1) or removes (sign -1) a lasting effect's change to the player's numbers
static void applyFortify(World& w, const ActiveEffect& a, float sign)
{
	PlayerStats& s = w.stats;
	float m = a.magnitude * sign;
	switch (a.effect)
	{
	case EFF_FORTIFY_ATTRIBUTE:
		if (a.attribute >= 0 && a.attribute < 8) shiftStat(s.attributes[a.attribute], s.attrBelow[a.attribute], (int)lroundf(m));
		break;
	case EFF_FORTIFY_SKILL: if (a.skill >= 0 && a.skill < 27) shiftStat(s.skills[a.skill], s.skillBelow[a.skill], (int)lroundf(m)); break;
	case EFF_FORTIFY_HEALTH: s.healthMax += m; s.health += m; break;
	case EFF_FORTIFY_MAGICKA: s.magickaMax += m; s.magicka += m; break;
	case EFF_FORTIFY_FATIGUE: s.fatigueMax += m; s.fatigue += m; break;
	case EFF_DRAIN_ATTRIBUTE: case EFF_ABSORB_ATTRIBUTE:
		if (a.attribute >= 0 && a.attribute < 8) shiftStat(s.attributes[a.attribute], s.attrBelow[a.attribute], -(int)lroundf(m));
		break;
	case EFF_DRAIN_SKILL: case EFF_ABSORB_SKILL:
		if (a.skill >= 0 && a.skill < 27) shiftStat(s.skills[a.skill], s.skillBelow[a.skill], -(int)lroundf(m));
		break;
	case EFF_FORTIFY_MAX_MAGICKA:
	{
		float extra = m * 0.1f * s.attributes[ATTR_INTELLIGENCE];
		s.magickaMax += extra;
		s.magicka = fminf(s.magickaMax, s.magicka + (sign > 0 ? extra : 0.0f));
		break;
	}
	default: break;          // attack, shield, jump, levitate, swift swim are read while active
	}
	if (a.effect == EFF_LEVITATE)
		w.player.flying = sign > 0;
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

// Starts an effect on the player (potions, ingredients, self spells, harmful touch from others)
void Session::applyEffectToPlayer(const SpellEffect& e, const std::string& source, int casterRef, float spellCost,
	float castChance)
{
	float magnitude = e.min + (e.max - e.min) * frand();
	float duration = (float)(e.duration > 0 ? e.duration : 1);
	{
		// Reflect and Spell Absorption, each its own roll: the magnitude is the percent chance the effect doesn't land
		// (only for an effect another actor cast at the player, harmful or not). Reflected: it lands on the caster instead.
		// Absorbed: the player gets the spell's cost as magicka.
		if (casterRef >= 0 && w.effectTotal(EFF_REFLECT) > 0.0f && rand() % 100 < (int)w.effectTotal(EFF_REFLECT))
		{
			notify("The spell was reflected.");
			if (w.active(casterRef))
				applyEffectToActor(casterRef, e, false);         // (a reflected spell isn't reflected again)
			return;
		}
		if (casterRef >= 0 && w.effectTotal(EFF_SPELL_ABSORPTION) > 0.0f
			&& rand() % 100 < (int)w.effectTotal(EFF_SPELL_ABSORPTION))
		{
			notify("The spell was absorbed.");
			w.stats.magicka += spellCost;
			return;
		}
	}
	if (harmful(e.effect))
	{
		auto me = w.game.magicEffects.find(e.effect);
		bool resistable = false;
		float r = resistance(w, e.effect, me != w.game.magicEffects.end() && (me->second.flags & 0x8), castChance, resistable);
		if (resistable)
		{
			magnitude *= fmaxf(0.0f, 1.0f - r / 100.0f);
			if (magnitude <= 0.0f)
			{
				notify(w.game.gmst("smagicpcresisted", "You resisted the effects of the spell."));
				return;
			}
		}
	}
	// Absorb is linked to its caster: with none (a self-cast or a potion) it nets nothing (OpenMW drops it)
	if (casterRef < 0 && e.effect >= EFF_ABSORB_ATTRIBUTE && e.effect <= EFF_ABSORB_SKILL)
		return;
	ActiveEffect a = { e.effect, e.attribute, e.skill, magnitude, duration, source };
	if (instantEffect(e.effect, magnitude))
		return;
	// Summons: the creature appears beside the player and fights for them until the spell ends
	if (const char* creature = summonCreature(e.effect))
	{
		float pos[3] = { w.player.feet[0] + sinf(w.player.yaw) * 128.0f, w.player.feet[1] + cosf(w.player.yaw) * 128.0f,
			w.player.feet[2] };
		a.ref = w.spawnActor(creature, pos, w.player.yaw);
		if (a.ref < 0)
			return;
		Ref& r = w.refs[a.ref];
		r.ally = true;
		r.aiPackage = AIPKG_FOLLOW;
		r.aiTarget = "player";
		w.effects.push_back(a);
		return;
	}
	// Bound items: in hand (or worn) until the spell ends, instead of what was there
	if (const char* item = boundItem(e.effect))
	{
		for (int k = 0; k < (e.effect == 131 ? 2 : 1); k++)
		{
			ActiveEffect b = a;
			b.item = k ? "bound_gauntlet_right" : item;
			const Object* o = w.game.object(b.item);
			if (!o)
				continue;
			for (auto& it : w.inventory)
			{
				const Object* io = it.equipped ? w.game.object(it.id) : nullptr;
				if (io && io->type == o->type && (o->type == "WEAP" || io->subtype == o->subtype))
				{
					it.equipped = false;
					b.prev = it.id;
				}
			}
			w.addItem(b.item, 1);
			for (auto& it : w.inventory)
				if (it.id == b.item)
					it.equipped = true;
			w.effects.push_back(b);
		}
		return;
	}
	// Drain Health / Magicka / Fatigue: the magnitude off at once, given back when it ends (Damage is the
	// one that goes on per second)
	if (e.effect == EFF_DRAIN_HEALTH || e.effect == EFF_DRAIN_MAGICKA || e.effect == EFF_DRAIN_FATIGUE)
	{
		float& v = e.effect == EFF_DRAIN_HEALTH ? w.stats.health : e.effect == EFF_DRAIN_MAGICKA ? w.stats.magicka : w.stats.fatigue;
		v -= magnitude;          // (OpenMW lets it go below 0: the maximum stays, the amount comes back at the end)
		if (e.effect == EFF_DRAIN_HEALTH && v <= 0.0f && !playerDead)
			damagePlayer(0.0f, false);      // the death screen
		w.effects.push_back(a);
		return;
	}
	if (isFortify(e.effect))
		applyFortify(w, a, 1.0f);
	else if (!harmful(e.effect) && !isPresence(e.effect) && e.effect != EFF_RESTORE_HEALTH && e.effect != EFF_RESTORE_MAGICKA
		&& e.effect != EFF_RESTORE_FATIGUE && e.effect != EFF_RESTORE_ATTRIBUTE && e.effect != EFF_RESTORE_SKILL)
	{
		if (summonCreature(e.effect) || boundItem(e.effect))
			return;
		static std::unordered_set<int> reported;
		if (reported.insert(e.effect).second)
			logf("magic: effect %d is not supported", e.effect);
		return;
	}
	w.effects.push_back(a);
}

// Effects on the player that happen once: cures, dispel, Mark / Recall, Interventions. True if handled.
bool Session::instantEffect(int effect, float magnitude)
{
	// DisableTeleporting (Dagoth Ur's citadel): no Mark, Recall or Interventions
	if (w.teleportDisabled && (effect == EFF_MARK || effect == EFF_RECALL || effect == EFF_DIVINE_INTERVENTION
		|| effect == EFF_ALMSIVI_INTERVENTION))
	{
		notify(w.game.gmst("steleportdisabled", "Teleportation magic does not work here."));
		return true;
	}
	PlayerStats& s = w.stats;
	auto removeSpells = [&](int type, bool corprusOnly) {
		for (size_t i = 0; i < s.spells.size();)
		{
			auto it = w.game.spells.find(lower(s.spells[i]));
			bool corprus = false;
			if (it != w.game.spells.end())
				for (auto& fx : it->second.effects)
					corprus |= fx.effect == 132;
			if (it != w.game.spells.end() && (corprusOnly ? corprus : it->second.type == type && !corprus))
				s.spells.erase(s.spells.begin() + i);
			else
				i++;
		}
		// The disease's drains go with it (current bars kept)
		float hp = s.health, mp = s.magicka, fp = s.fatigue;
		w.recomputeStats();
		s.health = fminf(s.healthMax, hp);
		s.magicka = fminf(s.magickaMax, mp);
		s.fatigue = fminf(s.fatigueMax, fp);
	};
	auto endEffects = [&](auto pred) {
		for (size_t i = 0; i < w.effects.size();)
			if (pred(w.effects[i]))
				endPlayerEffect(i);              // (undone as at the end: a Dispel gives a drain back)
			else
				i++;
	};
	switch (effect)
	{
	case EFF_CURE_COMMON_DISEASE: removeSpells(3, false); notify("You are cured of common disease."); return true;
	case EFF_CURE_BLIGHT_DISEASE: removeSpells(2, false); notify("You are cured of blight disease."); return true;
	case EFF_CURE_CORPRUS:
		// OpenMW ends the Corprus effect only: the disease stays on the list and its other effects go on
		if (w.pcHasCorprus())
		{
			w.corprusSince = -2.0f;
			float hp = s.health, mp = s.magicka, fp = s.fatigue;
			w.recomputeStats();
			s.health = fminf(s.healthMax, hp);
			s.magicka = fminf(s.magickaMax, mp);
			s.fatigue = fminf(s.fatigueMax, fp);
		}
		return true;
	case EFF_CURE_POISON: endEffects([](const ActiveEffect& a) { return a.effect == EFF_POISON; }); return true;
	case EFF_CURE_PARALYZATION: endEffects([](const ActiveEffect& a) { return a.effect == EFF_PARALYZE; }); return true;
	case EFF_DISPEL:
	{
		// OpenMW: whole spells go, each with magnitude percent chance (one roll per spell); potions, enchantments and
		// abilities stay
		std::unordered_map<std::string, bool> gone;
		for (auto& a : w.effects)
			if (!gone.count(a.source))
			{
				bool spell = false;
				for (auto& sp : w.game.spells)
					if (sp.second.type == 0 && sp.second.name == a.source)
						spell = true;
				for (auto& id : w.madeSpells)
				{
					auto it = w.game.spells.find(lower(id));
					if (it != w.game.spells.end() && it->second.name == a.source)
						spell = true;
				}
				gone[a.source] = spell && rand() % 100 < (int)magnitude;
			}
		endEffects([&](const ActiveEffect& a) { return gone[a.source]; });
		return true;
	}
	case EFF_REMOVE_CURSE:
		removeSpells(4, false);                           // (OpenMW: curse-type spells)
		return true;
	case EFF_MARK:
		w.markCell = w.current;
		memcpy(w.markPos, w.player.feet, sizeof(w.markPos));
		w.markYaw = w.player.yaw;
		notify("This place is marked for Recall.");
		return true;
	case EFF_RECALL:
		if (w.markCell < 0)
			notify("You have not marked a place to Recall to.");
		else
			teleportPlayer(w.markCell, w.markPos, w.markYaw);
		return true;
	case EFF_DIVINE_INTERVENTION: case EFF_ALMSIVI_INTERVENTION:
	{
		// The nearest Imperial shrine / Tribunal temple marker (from inside: from the last spot outdoors)
		const float* from = w.cells[w.current].interior ? w.lastOutside : w.player.feet;
		const auto& list = effect == EFF_DIVINE_INTERVENTION ? w.game.divineMarkers : w.game.templeMarkers;
		int best = -1, bestCell = -1;
		float bestD = 1e30f;
		for (size_t k = 0; k < list.size(); k++)
		{
			int c = w.gridCell((int)floorf(list[k][0] / 8192.0f), (int)floorf(list[k][1] / 8192.0f));
			float dx = list[k][0] - from[0], dy = list[k][1] - from[1];
			if (c >= 0 && dx * dx + dy * dy < bestD)
			{
				bestD = dx * dx + dy * dy;
				best = k;
				bestCell = c;
			}
		}
		if (best < 0)
			notify("There is nowhere within reach for the Intervention to take you.");
		else
		{
			float pos[3] = { list[best][0], list[best][1], list[best][2] };
			teleportPlayer(bestCell, pos, list[best][3]);
		}
		return true;
	}
	default:
		return false;
	}
}

// A spell effect lands on an actor (OpenMW's applyProtections and applyMagicEffect): Reflect and Spell Absorption
// first (only for someone else's spell), then the actor's resistance roll; damage and restoration run a second at a
// time for the duration (at once when it has none); resistances, shields, Sanctuary, Blind ... last while they run
void Session::applyEffectToActor(int ri, const SpellEffect& e, bool byPlayer, float spellCost, float castChance,
	const std::string& source)
{
	Ref& r = w.refs[ri];
	if (r.dead || r.actor < 0)
		return;
	const ActorDef& def = w.game.actors[r.actor];
	auto me = w.game.magicEffects.find(e.effect);
	int flags = me != w.game.magicEffects.end() ? me->second.flags : 0;
	if (byPlayer)
	{
		float reflect = w.actorEffect(ri, EFF_REFLECT), absorb = w.actorEffect(ri, EFF_SPELL_ABSORPTION);
		if (reflect > 0.0f && rand() % 100 < (int)reflect)
		{
			logf("magic: %s reflects %d", r.id.c_str(), e.effect);
			applyEffectToPlayer(e, source.empty() ? "Reflected spell" : source);    // (not reflected a second time)
			return;
		}
		if (absorb > 0.0f && rand() % 100 < (int)absorb)
		{
			logf("magic: %s absorbs %d (+%.0f magicka)", r.id.c_str(), e.effect, spellCost);
			r.magicka += spellCost;
			return;
		}
	}
	// (rolled whole, once for the cast: OpenMW's min plus a die of max - min + 1)
	float mag = (float)e.min;
	if (e.max > e.min)
		mag += (float)std::min(e.max - e.min, (int)(frand() * (e.max - e.min + 1)));
	if (harmful(e.effect))
	{
		bool resistable = false;
		float res = resistRoll(w, e.effect, (flags & 0x8) != 0, [&](int k) { return w.actorEffect(ri, k); },
			(float)def.attributes[ATTR_WILLPOWER], (float)def.attributes[ATTR_LUCK], r.fatigue, r.fatigueMax, castChance, resistable);
		if (resistable)
		{
			mag *= fmaxf(0.0f, 1.0f - res / 100.0f);
			if (mag <= 0.0f)
			{
				logf("magic: %s resists %d", r.id.c_str(), e.effect);
				if (!r.dead)
				{
					bool peaceful = r.ai != AI_COMBAT;
					makeHostile(ri);
					if (peaceful && !def.creature)
						crimeSeen(CRIME_ASSAULT, 0, ri), reportCrime(ri, kBountyAssault);
				}
				return;
			}
		}
	}
	float dur = (float)(e.duration > 0 && !(flags & 0x4) ? e.duration : 0);
	auto timed = [&](float seconds) { r.effects.push_back({ e.effect, mag, seconds }); };
	switch (e.effect)
	{
	// Damage and restoration: per second for the duration, all at once without one
	case EFF_FIRE_DAMAGE: case EFF_SHOCK_DAMAGE: case EFF_FROST_DAMAGE: case EFF_DAMAGE_HEALTH: case EFF_POISON:
		if (dur > 1.0f)
			timed(dur);
		else
			damageNpc(ri, mag, false);               // (the difficulty slider scales blows, not spells)
		break;
	case EFF_DAMAGE_FATIGUE: case EFF_DAMAGE_MAGICKA: case EFF_RESTORE_HEALTH: case EFF_RESTORE_FATIGUE:
	case EFF_RESTORE_MAGICKA: case EFF_ABSORB_HEALTH: case EFF_ABSORB_FATIGUE: case EFF_ABSORB_MAGICKA:
		if (dur > 1.0f)
			timed(dur);
		else
		{
			r.effects.push_back({ e.effect, mag, 1.0f });
			updateActorEffects(ri, 1.0f);            // the whole magnitude now
		}
		break;
	// Drain: the current and the maximum drop by the magnitude until it ends
	case EFF_DRAIN_HEALTH:
		timed(fmaxf(1.0f, dur));
		damageNpc(ri, mag, false);
		break;
	case EFF_DRAIN_FATIGUE:
		r.fatigue -= mag;
		timed(fmaxf(1.0f, dur));
		break;
	case EFF_DRAIN_MAGICKA:
		r.magicka -= mag;
		timed(fmaxf(1.0f, dur));
		break;
	// Attributes and skills: kept as timed effects with their key, read by EXPECT:refattr / refskill (OpenMW changes
	// every actor's; skills exist on people only). An Absorb also fortifies the player for as long
	case EFF_DRAIN_ATTRIBUTE: case EFF_FORTIFY_ATTRIBUTE: case EFF_ABSORB_ATTRIBUTE:
	case EFF_DRAIN_SKILL: case EFF_FORTIFY_SKILL: case EFF_ABSORB_SKILL:
	{
		bool skill = e.effect == EFF_DRAIN_SKILL || e.effect == EFF_FORTIFY_SKILL || e.effect == EFF_ABSORB_SKILL;
		int key = skill ? e.skill : e.attribute;
		if (key < 0 || key >= (skill ? 27 : 8) || (skill && def.creature))
			return;
		r.effects.push_back({ e.effect, mag, fmaxf(1.0f, dur), key });
		if (e.effect == EFF_ABSORB_ATTRIBUTE || e.effect == EFF_ABSORB_SKILL)
		{
			ActiveEffect f = { skill ? EFF_FORTIFY_SKILL : EFF_FORTIFY_ATTRIBUTE, skill ? -1 : key, skill ? key : -1, mag,
				fmaxf(1.0f, dur), source.empty() ? "Absorb" : source };
			applyFortify(w, f, 1.0f);
			w.effects.push_back(f);
		}
		break;
	}
	case EFF_FORTIFY_HEALTH: case EFF_FORTIFY_FATIGUE: case EFF_FORTIFY_MAGICKA:
		// raises the current value and the maximum, both taken back at the end
		(e.effect == EFF_FORTIFY_HEALTH ? r.healthMax : e.effect == EFF_FORTIFY_FATIGUE ? r.fatigueMax : r.magickaMax) += mag;
		(e.effect == EFF_FORTIFY_HEALTH ? r.health : e.effect == EFF_FORTIFY_FATIGUE ? r.fatigue : r.magicka) += mag;
		timed(fmaxf(1.0f, dur));
		break;
	// Read while they last: resistances and weaknesses, shields, Sanctuary, Chameleon, Invisibility, Blind, Silence,
	// Sound, Reflect, Spell Absorption, Fortify Attack
	case EFF_SHIELD: case EFF_FIRE_SHIELD: case EFF_LIGHTNING_SHIELD: case EFF_FROST_SHIELD: case EFF_SANCTUARY:
	case EFF_CHAMELEON: case EFF_INVISIBILITY: case EFF_BLIND: case EFF_SILENCE: case EFF_SOUND: case EFF_REFLECT:
	case EFF_SPELL_ABSORPTION: case EFF_FORTIFY_ATTACK:
		timed(fmaxf(1.0f, dur));
		break;
	// Minds: calm stops a fight, frenzy starts one, demoralize makes them flee, rally stops that. The Humanoid
	// ones (odd ids) touch only NPCs, the Creature ones only creatures (OpenMW's modifyAiSetting)
	case EFF_CALM_HUMANOID: case EFF_CALM_CREATURE:
		if ((e.effect & 1) == (def.creature ? 1 : 0))
			return;
		if (r.ai == AI_COMBAT)
		{
			r.ai = AI_IDLE;
			r.aggressor = false;
			r.fleeing = false;
		}
		r.calmUntil = w.time + fmaxf(1.0f, dur);
		return;
	case EFF_FRENZY_HUMANOID: case EFF_FRENZY_CREATURE:
		if ((e.effect & 1) == (def.creature ? 1 : 0))
			return;
		r.aggressor = true;
		r.calmUntil = 0.0f;
		makeHostile(ri);
		return;
	case EFF_DEMORALIZE_HUMANOID: case EFF_DEMORALIZE_CREATURE: case EFF_TURN_UNDEAD:
		if (e.effect != EFF_TURN_UNDEAD && (e.effect & 1) == (def.creature ? 1 : 0))
			return;
		r.fleeing = true;
		r.fleeTimer = fmaxf(r.fleeTimer, dur > 0.0f ? dur : 10.0f);
		return;
	case EFF_RALLY_HUMANOID: case EFF_RALLY_CREATURE:
		if ((e.effect & 1) == (def.creature ? 1 : 0))
			return;
		r.fleeing = false;
		return;
	case EFF_CHARM:
		timed(fmaxf(1.0f, dur));              // (their disposition reads it while it lasts: OpenMW's Charm)
		return;
	case EFF_COMMAND_HUMANOID: case EFF_COMMAND_CREATURE:
		// Follows the player for the spell's duration (game hours from its seconds)
		r.ai = AI_IDLE;
		r.aiPackage = AIPKG_FOLLOW;
		r.aiTarget = "player";
		r.aiStart = w.gameHour;
		r.aiDuration = (dur > 0.0f ? dur : 10.0f) / 3600.0f * w.timescale();
		r.aiDone = false;
		return;
	case EFF_PARALYZE:
		r.knockTimer = fmaxf(r.knockTimer, fmaxf(1.0f, dur));
		timed(fmaxf(1.0f, dur));
		break;
	case EFF_SOULTRAP:
		r.soulTrapUntil = w.time + fmaxf(1.0f, dur);
		break;
	default:
		if ((e.effect >= EFF_WEAKNESS_FIRE && e.effect <= EFF_WEAKNESS_NORMAL_WEAPONS)
			|| (e.effect >= EFF_RESIST_FIRE && e.effect <= EFF_RESIST_PARALYSIS))
			timed(fmaxf(1.0f, dur));
		break;
	}
	if (harmful(e.effect) && !r.dead)
	{
		bool peaceful = r.ai != AI_COMBAT;
		makeHostile(ri);
		if (peaceful && !def.creature)
			crimeSeen(CRIME_ASSAULT, 0, ri), reportCrime(ri, kBountyAssault);
	}
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
				damageNpc(ri, amount, false);
			break;
		case EFF_DAMAGE_FATIGUE: r.fatigue -= amount; break;
		case EFF_DAMAGE_MAGICKA: r.magicka = fmaxf(0.0f, r.magicka - amount); break;
		case EFF_RESTORE_HEALTH: r.health = fminf(r.healthMax, r.health + amount); break;
		case EFF_RESTORE_FATIGUE: r.fatigue = fminf(r.fatigueMax, r.fatigue + amount); break;
		case EFF_RESTORE_MAGICKA: r.magicka = fminf(r.magickaMax, r.magicka + amount); break;
		// Absorb: theirs becomes the player's
		case EFF_ABSORB_HEALTH:
			if (!r.dead)
				damageNpc(ri, amount, false);
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

// An active effect ends (its time is up, a cure or Dispel took it, or the same spell was cast again): what it changed
// is undone and it is removed
void Session::endPlayerEffect(size_t i)
{
	PlayerStats& s = w.stats;
	ActiveEffect a = w.effects[i];
	w.effects.erase(w.effects.begin() + i);
	if (isFortify(a.effect))
		applyFortify(w, a, -1.0f);
	// a drain's points come back
	if (a.effect == EFF_DRAIN_HEALTH && !playerDead)
		s.health = fminf(s.healthMax, s.health + a.magnitude);
	if (a.effect == EFF_DRAIN_MAGICKA)
		s.magicka = fminf(s.magickaMax, s.magicka + a.magnitude);
	if (a.effect == EFF_DRAIN_FATIGUE)
		s.fatigue = fminf(s.fatigueMax, s.fatigue + a.magnitude);
	if (a.ref >= 0)
		w.despawn(a.ref);              // the summoned creature goes back
	if (!a.item.empty())
	{
		// The bound item vanishes; what it replaced comes back
		w.removeItem(a.item, w.itemCount(a.item));
		for (auto& it : w.inventory)
			if (!a.prev.empty() && it.id == a.prev)
				it.equipped = true;
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
				if (const Object* o = it.equipped ? w.game.object(it.id) : nullptr)
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
	// Damage / Restore Attribute or Skill moved a whole point: the numbers follow (current bars kept)
	if (statsChanged)
	{
		float health = s.health, magicka = s.magicka, fatigue = s.fatigue;
		w.recomputeStats();
		s.health = fminf(s.healthMax, health);
		s.magicka = fminf(s.magickaMax, magicka);
		s.fatigue = fminf(s.fatigueMax, fatigue);
	}
}

// ---- Spells

std::vector<std::string> Session::knownSpells()
{
	std::vector<std::string> out;
	auto add = [&](const std::string& id) {
		std::string l = lower(id);
		auto it = w.game.spells.find(l);
		if (it == w.game.spells.end() || (it->second.type != 0 && it->second.type != 5))
			return;                                       // abilities and diseases aren't cast
		for (auto& o : out)
			if (o == l)
				return;
		out.push_back(l);
	};
	if (const RaceDef* r = w.race())
		for (auto& s : r->spells)
			add(s);
	if (const BirthDef* b = w.birthsign())
		for (auto& s : b->spells)
			add(s);
	for (auto& s : w.stats.spells)
		add(s);
	return out;
}

// One effect's cost the way the success chance counts it (OpenMW's calcSpellBaseSuccessChance: no
// duration offset, area as given, unlike the magicka cost of a spell being made)
static float successEffectCost(const World& w, const SpellEffect& e, int* school)
{
	auto it = w.game.magicEffects.find(e.effect);
	if (it == w.game.magicEffects.end())
		return 0.0f;
	float x = (float)e.duration;
	if (!(it->second.flags & 0x1000))                  // applied once: the duration as it is, else at least 1
		x = fmaxf(1.0f, x);
	x *= 0.1f * it->second.cost;
	x *= 0.5f * (e.min + e.max);
	x += e.area * 0.05f * it->second.cost;
	if (e.range == 2)
		x *= 1.5f;
	if (school)
		*school = it->second.school;
	return x * w.game.gmstf("feffectcostmult", 0.5f);
}

// The school a spell counts as (its chance and the skill it trains): the effect with the lowest
// twice-the-skill minus its cost (OpenMW's calcSpellBaseSuccessChance). lowest: that effect's 2 x skill
int Session::spellSchool(const SpellDef& sp, float* lowest)
{
	int school = 5;
	float y = 1e30f, lowestSkill = 0.0f;
	for (auto& e : sp.effects)
	{
		int sch = -1;
		float x = successEffectCost(w, e, &sch);
		if (sch < 0 || sch >= 6)
			continue;
		float s = 2.0f * w.stats.skills[kSchoolSkill[sch]];
		if (s - x < y)
		{
			y = s - x;
			school = sch;
			lowestSkill = s;
		}
	}
	if (lowest)
		*lowest = lowestSkill;
	return school;
}

int Session::castChance(const SpellDef& sp)
{
	if (sp.type == 5)
		return 100;
	const PlayerStats& s = w.stats;
	float lowest = 0.0f;
	spellSchool(sp, &lowest);
	// (twice the skill of the weakest effect - the cost + Willpower / 5 + Luck / 10 - Sound) x the fatigue term
	float chance = (lowest - sp.cost + s.attributes[ATTR_WILLPOWER] * 0.2f + s.attributes[ATTR_LUCK] * 0.1f
		- w.effectTotal(EFF_SOUND)) * (1.25f - 0.5f * (1.0f - s.fatigue / fmaxf(1.0f, s.fatigueMax)));
	return chance < 0 ? 0 : chance > 100 ? 100 : (int)chance;
}

// An enchanted item's cast: from its charge (scrolls are used up); no chance roll, no magicka
// What an enchanted item's cast takes from its charge: each point of Enchant over 10 takes a percent off, down
// to 1 (OpenMW's getEffectiveEnchantmentCastCost)
float Session::enchantCastCost(float cost)
{
	float r = cost - (cost / 100.0f) * (w.stats.skills[9] - 10);
	return (float)(int)(r < 1.0f ? 1.0f : r);
}

void Session::castItem(const std::string& itemId)
{
	InventoryItem* item = nullptr;
	for (auto& it : w.inventory)
		if (it.id == itemId && w.enchantmentOf(it))
			item = &it;
	if (!item)
	{
		notify("You don't have that item any more.");
		w.stats.selectedSpell.clear();
		return;
	}
	if (castPending)
		finishCast();                      // (one cast at a time: the last one lands first)
	const SpellDef sp = *w.enchantmentOf(*item);
	const Object* o = w.game.object(itemId);
	if (sp.type == ENCH_USE)
	{
		float charge = w.chargeOf(*item);
		const float cost = enchantCastCost(sp.cost);
		if (charge < cost)
		{
			notify(w.game.gmst("smagicinsufficientcharge", "This item does not have enough charge."));
			return;
		}
		item->charge = charge - cost;
	}
	else if (sp.type == ENCH_ONCE)
		w.removeItem(itemId, 1);
	else
		return;
	if (sp.type == ENCH_USE)
		useSkill(9, 1);                   // Enchant: using a magic item (SKDT use 1)
	SpellDef named = sp;
	named.name = o ? o->name : sp.id;
	logf("magic: %s cast from %s", sp.id.c_str(), itemId.c_str());
	beginCast(named, spellSchool(sp), false);     // (OpenMW plays no animation for an enchanted item)
}

void Session::castSpell()
{
	if (castPending)
		finishCast();                      // (one cast at a time: the last one lands first)
	if (w.stats.selectedSpell.compare(0, 5, "item:") == 0)
	{
		castItem(w.stats.selectedSpell.substr(5));
		return;
	}
	auto it = w.game.spells.find(w.stats.selectedSpell);
	if (it == w.game.spells.end())
	{
		notify("Choose a spell on the Magic screen first.");
		return;
	}
	const SpellDef& sp = it->second;
	PlayerStats& s = w.stats;
	if (w.effectTotal(EFF_SILENCE) > 0.0f)
	{
		notify("You are silenced and can't cast spells.");
		return;
	}
	if (sp.type == 5)
	{
		int day = (int)(w.gameHour / 24.0f);
		auto used = s.powerUsedDay.find(sp.id);
		if (used != s.powerUsedDay.end() && used->second == day)
		{
			notify("You can use that power only once a day.");
			return;
		}
		s.powerUsedDay[lower(sp.id)] = day;
	}
	else if (s.magicka < sp.cost)
	{
		notify("You don't have enough magicka.");
		return;
	}
	else
	{
		s.magicka -= sp.cost;
		testCastTaken = (float)sp.cost;
		// casting tires too: cost x (fFatigueSpellBase + load x fFatigueSpellMult)
		s.fatigue = fmaxf(0.0f, s.fatigue - sp.cost * (w.game.gmstf("ffatiguespellbase", 0.0f)
			+ playerLoad * w.game.gmstf("ffatiguespellmult", 0.0f)));
	}
	// (the magicka is paid now; the chance is rolled and the spell lands when the animation gets to its release)
	beginCast(sp, spellSchool(sp), true);
}

// The cast starts: its sound and the glow at the hands, then the animation. Without one the spell lands at once, and
// the next cast waits a moment anyway
void Session::beginCast(const SpellDef& sp, int school, bool animate)
{
	playSound(-1, spellSound(sp, 0));
	{
		// the cast's visual at the hand (a self spell's shows around the player)
		bool away = false;
		for (auto& e : sp.effects)
			away |= e.range != 0;
		float cp0 = cosf(w.player.pitch);
		float hand[3] = { w.player.feet[0] + sinf(w.player.yaw) * cp0 * 40.0f, w.player.feet[1] + cosf(w.player.yaw) * cp0 * 40.0f,
			playerEyeZ(w.player) - 25.0f };
		// Hit visuals are modelled around the actor's feet (OpenMW attaches them to the actor's own node), so a
		// self spell's sits there: its Shield sphere then wraps the whole body
		float feet[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] };
		spellVfx(sp, away ? 0 : 1, away ? hand : feet);
	}
	castDef = sp;
	castSchool = school;
	if (animate)
	{
		// the first effect's range picks the animation, as in OpenMW
		int range = sp.effects.empty() ? 0 : sp.effects[0].range;
		vm.speed = 1.0f;
		const char* group = range == 2 ? "CastTarget" : range == 1 ? "CastTouch" : "CastSelf";
		vm.play(VM_CAST, group);
		castPending = true;
		// the arms are the cast's until its whole group has played (OpenMW's one upper-body state)
		if (vm.action == VM_CAST)
			castLock = fmaxf(0.1f, vm.groupLength(group));
		if (vm.action != VM_CAST)
		{
			castWait = 0.5f;              // no animation to wait for
			finishCast();
		}
	}
	else
		releaseSpell(castDef, school);
}

// The release: the chance is rolled (a failed spell is spent all the same), then the spell lands
void Session::finishCast()
{
	castPending = false;
	const SpellDef sp = castDef;
	int school = castSchool;
	if (rand() % 100 >= castChance(sp))
	{
		static const char* failure[6] = { "Spell Failure Alteration", "Spell Failure Conjuration",
			"Spell Failure Destruction", "Spell Failure Illusion", "Spell Failure Mysticism", "Spell Failure Restoration" };
		playSound(-1, failure[school >= 0 && school < 6 ? school : 2]);
		notify("You failed casting the spell.");
		return;
	}
	if (sp.type != 5)
		useSkill(kSchoolSkill[school], 0);
	releaseSpell(sp, school);
}

void Session::castUpdate(float dt)
{
	if (castWait > 0.0f)
		castWait -= dt;
	if (castLock > 0.0f)
	{
		castLock -= dt;
		// the group is over (or something else took the arms: a knockdown): the lock goes
		if (vm.action != VM_CAST || playerDead || w.player.knockTimer > 0.0f)
			castLock = 0.0f;
	}
	if (!castPending)
		return;
	// A cast cut short (a knockdown, a death, the animation replaced) does not release: the magicka stays spent, as in
	// OpenMW's CharacterController::cancel (mCanCast = false)
	if (playerDead || w.player.knockTimer > 0.0f || vm.action != VM_CAST)
	{
		logf("cast: %s cut short (view model action %d)", castDef.id.c_str(), (int)vm.action);
		castPending = false;
		castLock = 0.0f;
		return;
	}
	if (vm.releaseReached())
		finishCast();
}

std::string Session::spellSound(const SpellDef& sp, int kind)
{
	static const char* kKind[4] = { " cast", " bolt", " hit", " area" };
	for (auto& e : sp.effects)
	{
		auto me = w.game.magicEffects.find(e.effect);
		if (me != w.game.magicEffects.end() && !me->second.snd[kind].empty())
			return me->second.snd[kind];
		break;
	}
	int school = spellSchool(sp);
	return std::string(kSchoolName[school >= 0 && school < 6 ? school : 2]) + kKind[kind];
}

// The cast itself: self effects on the player, touch on who's in front, target as a bolt
void Session::releaseSpell(const SpellDef& sp, int school)
{
	bool anyTarget = false, anyTouch = false;
	for (auto& e : sp.effects)
	{
		anyTarget |= e.range == 2;
		anyTouch |= e.range == 1;
	}
	castCount++;
	// The same spell cast again ends the old copy (OpenMW's addToSpells): one magnitude, a fresh timer
	for (size_t k = 0; k < w.effects.size();)
		if (!sp.name.empty() && w.effects[k].source == sp.name)
			endPlayerEffect(k);
		else
			k++;
	if (anyTarget)
	{
		// Target effects fly as a bolt along the view
		float cp = cosf(w.player.pitch);
		float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
		float from[3] = { w.player.feet[0] + dir[0] * 30.0f, w.player.feet[1] + dir[1] * 30.0f, playerEyeZ(w.player) - 15.0f };
		fireProjectile(-1, from, dir, 1200.0f, "", "", 1.0f, lower(sp.id));
	}

	// Who touch and target effects land on
	auto aim = [&](float range, float minCos) {
		int best = -1;
		float bestD = range;
		float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
		w.forLoadedActors([&](int i) {
			const Ref& r = w.refs[i];
			if (r.actor < 0 || r.dead || !r.visible())
				return;
			float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
			float d = sqrtf(dx * dx + dy * dy);
			if (d < bestD && (d < 1.0f || (dx * fx + dy * fy) / d > minCos))
			{
				bestD = d;
				best = i;
			}
		});
		return best;
	};
	int touch = -2, target = -2;
	for (auto& e : sp.effects)
	{
		if ((e.effect == EFF_OPEN || e.effect == EFF_LOCK) && e.range != 0 && this->target >= 0)
		{
			// Open / Lock on the door or container under the crosshair
			Ref& o = w.refs[this->target];
			int m = (int)(e.min + (e.max - e.min) * frand());
			if (o.type != "DOOR" && o.type != "CONT")
				continue;
			if (e.effect == EFF_OPEN)
			{
				// OpenMW: an Open on someone else's lock is an unlock attempt (trespassing where seen), open or not
				if (w.ownedByOther(this->target) && o.lockLevel > 0)
				{
					int witness = crimeWitness();
					if (witness >= 0)
						crimeSeen(CRIME_TRESPASS, 0, -1), reportCrime(witness, crimeBounty(CRIME_TRESPASS, 0));
				}
				if (o.lockLevel > 0 && m >= o.lockLevel)
				{
					o.lockLevel = 0;
					playSound(this->target, "Open Lock");
					notify(w.game.gmst("smagicopensuccess", "The lock opens."));
				}
				else if (o.lockLevel > 0)
				{
					playSound(this->target, "Open Lock Fail");
					notify("The lock is too strong for the spell.");
				}
			}
			else if (o.lockLevel < m)
			{
				// Lock: raised to the magnitude unless already locked higher
				o.lockLevel = m;
				playSound(this->target, "Open Lock");
				notify(w.game.gmst("smagiclocksuccess", "The lock is locked."));
			}
			continue;
		}
		if (e.range == 0)
			applyEffectToPlayer(e, sp.name);
		else
		{
			if (e.range == 2)
				continue;                                   // the bolt carries them
			int& who = e.range == 1 ? touch : target;
			if (who == -2)
				who = e.range == 1 ? aim(200.0f, 0.7f) : aim(kTargetRange, 0.97f);
			if (who >= 0)
			{
				applyEffectToActor(who, e, true, (float)sp.cost, (float)castChance(sp), sp.name);
				playSound(who, spellSound(sp, 2));
				spellVfx(sp, 1, w.refs[who].pos);
			}
		}
	}
	// the self and touch effects with an area reach the others around
	{
		float at[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + 60.0f };
		areaBurst(sp, 0, at, -1, -1);
		if (touch >= 0)
		{
			float tat[3] = { w.refs[touch].pos[0], w.refs[touch].pos[1], w.refs[touch].pos[2] + 60.0f };
			areaBurst(sp, 1, tat, touch, -1);
		}
	}
}

void Session::areaBurst(const SpellDef& sp, int range, const float at[3], int struck, int owner)
{
	bool any = false;
	for (auto& e : sp.effects)
		any |= e.range == range && e.area > 0;
	if (!any)
		return;
	playSound(-1, spellSound(sp, 3));
	spellVfx(sp, 2, at);
	const float kUnitsPerFoot = 21.333333f;
	for (auto& e : sp.effects)
	{
		if (e.range != range || e.area <= 0)
			continue;
		float rad = e.area * kUnitsPerFoot;
		std::vector<int> hit;
		w.forLoadedActors([&](int k) {
			const Ref& r = w.refs[k];
			if (k == struck || r.actor < 0 || r.dead || !r.visible() || k == owner)
				return;
			float dx = r.pos[0] - at[0], dy = r.pos[1] - at[1], dz = r.pos[2] + 60.0f - at[2];
			if (dx * dx + dy * dy + dz * dz <= rad * rad)
				hit.push_back(k);
		});
		for (int k : hit)
			if (owner < 0)                                   // an actor's area spell only touches the player
				applyEffectToActor(k, e, true, (float)sp.cost, 100.0f, sp.name);
		logf("magic: %s area %d ft, %d caught", sp.id.c_str(), e.area, owner < 0 ? (int)hit.size() : 0);
		// the player (the caster too, but not for a self spell: that one already landed)
		if (struck != -1 && !playerDead)
		{
			float dx = w.player.feet[0] - at[0], dy = w.player.feet[1] - at[1], dz = w.player.feet[2] + 60.0f - at[2];
			if (dx * dx + dy * dy + dz * dz <= rad * rad)
				applyEffectToPlayer(e, sp.name);
		}
	}
}

// ---- Potions and ingredients

void Session::consume(const std::string& itemId)
{
	const Object* o = w.game.object(itemId);
	if (!o || o->effects.empty())
		return;
	for (auto& e : o->effects)
		applyEffectToPlayer(e, o->name);
	playSound(-1, o->type == "INGR" ? "Swallow" : "Drink");
	w.removeItem(itemId, 1);
	if (o->type == "INGR")
		useSkill(16, 1);              // Alchemy: eating ingredients teaches their effects (SKDT use 1)
}
