#pragma once

// What the combat and AI files share: skill ids, reach, damage, animation groups
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>


enum { SKILL_BLOCK = 0, SKILL_MEDIUM_ARMOR = 2, SKILL_HEAVY_ARMOR = 3, SKILL_BLUNT = 4, SKILL_LONG_BLADE = 5,
       SKILL_AXE = 6, SKILL_SPEAR = 7, SKILL_SECURITY = 18, SKILL_SNEAK = 19, SKILL_LIGHT_ARMOR = 21, SKILL_SHORT_BLADE = 22,
       SKILL_MARKSMAN = 23, SKILL_MERCANTILE = 24, SKILL_HAND_TO_HAND = 26, SKILL_UNARMORED = 17 };
static const float kCombatDistance = 128.0f;      // fCombatDistance: reach 1.0 weapons
static const float kTurnSpeed = 6.0f;              // radians per second
static const float kHostileGiveUp = 5000.0f;
// Base weights per armor slot (iHelmWeight ...): lighter than 60% light armor, than 90% medium
static const float kArmorSlotBaseWeight[11] = { 5, 30, 10, 10, 15, 20, 5, 5, 15, 5, 5 };

// Melee reach as OpenMW measures it (getMeleeWeaponReach): fCombatDistance x the weapon's reach, x fHandToHandReach for
// people's fists, fCombatDistance for a creature's own attack; from the edges of both bodies (getDistanceToBounds: the
// distance less each one's collision box half length, Session::actorHalfLength; the player's 28.5, base_anim.nif's box)
static const float kPlayerHalfLength = 28.5f;

bool waterCreatureCanFight(World& w, int ri);
int weaponSkill(const Object* w);
std::string blowGroup(const ActorSet& set, const Actor& a, Ref& r, const Object* w);
void attackFollowThrough(ActorSet& set, Actor& a, float strength);
const char* vmGroup(const Object* w, bool tool);
float creatureDamage(const ActorDef& def, const Object* wpn, float charge, int kind);
float zMoverTop(World& w, const Ref& r, const Cell& cell);
void aiFinish(World& w, Ref& r);
void aiSettle(Ref& r);
void trailRecord(const World& w);
float trailHeightNear(float x, float y);
const float* followGoal(World& w, Ref& r, const float* who, float dt);

static inline float fatigueTerm(float fatigue, float fatigueMax)
{
	return 1.25f - 0.5f * (1.0f - (fatigueMax > 0.0f ? fmaxf(0.0f, fatigue) / fatigueMax : 1.0f));
}

static inline float frand() { return (rand() % 1000) / 1000.0f; }

static inline bool isRanged(const Object* w) { return w && w->subtype >= WEAP_BOW && w->subtype <= WEAP_THROWN; }

// With a weapon (or fists) out: the ready stance's version of a movement group when the skeleton has one (OpenMW plays
// the group with the weapon's short group on: Idle1h, RunForward2c; bows and thrown weapons fall back on 1h,
// crossbows on 2c)
static inline std::string stanceGroup(const ActorSet& set, const Actor& a, const char* base, const Object* w)
{
	const char* tag = !w ? "HH" : (w->subtype == 2 || w->subtype == 4 || w->subtype == 8 || w->subtype == WEAP_CROSSBOW) ? "2c"
		: (w->subtype == 5 || w->subtype == 6) ? "2w" : "1h";
	std::string g = std::string(base) + tag;
	return actorFindGroup(actorSkeleton(set, a.skeleton), g.c_str()) >= 0 ? g : std::string(base);
}

static inline float meleeReach(World& w, const Object* wpn, bool creature)
{
	if (wpn && wpn->recType == REC_WEAP && !isRanged(wpn))
		return kCombatDistance * wpn->reach;
	return creature ? kCombatDistance : kCombatDistance * w.game.gmstf("fhandtohandreach", 1.0f);
}

// The hit cone (getHitContact): fCombatAngleXY / 90 is the sine of the widest angle off the attacker's facing
static inline float hitAngle(World& w)
{
	return asinf(fminf(1.0f, w.game.gmstf("fcombatanglexy", 60.0f) / 90.0f));
}

// Damage of one blow: attack kind from how the player moves (thrust forward, slash sideways, chop)
static inline float weaponDamage(const Object* w, int kind, float charge, int strength)
{
	const u8* r = kind == 1 ? w->slash : kind == 2 ? w->thrust : w->chop;
	float dmg = r[0] + (r[1] - r[0]) * charge;
	return dmg * (0.5f + 0.01f * strength);
}

static inline float applyArmor(float dmg, float armor)
{
	if (armor <= 0.0f || dmg <= 0.0f)
		return dmg;
	return dmg * fmaxf(0.25f, dmg / (dmg + armor));
}

static inline float angleDiff(float a, float b)
{
	float d = fmodf(a - b + 3.14159265f, 6.2831853f);
	if (d < 0.0f)
		d += 6.2831853f;
	return d - 3.14159265f;
}

// ---- NPCs

// The skill a blow is rolled with: a person's for the weapon (hand-to-hand without one); a creature's Combat, whatever
// it strikes with (OpenMW's Creature::hit: mData.mCombat). Creatures carry Combat, Magic and Stealth over the nine skills
// of each specialization (creature.py), so hand-to-hand and a bow read its Stealth: a scamp struck at 20, not its 50
static inline int attackSkill(const ActorDef& def, const Object* wpn)
{
	return def.creature ? def.skills[0] : def.skills[weaponSkill(wpn)];
}
