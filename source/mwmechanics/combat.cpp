// Combat and bartering, with Morrowind's formulas (as documented by OpenMW) where the game has
// the numbers:
//   hit chance  (weapon skill + agility / 5 + luck / 10) x fatigue term
//               - (defender agility / 5 + luck / 10) x defender fatigue term     (percent)
//               unaware targets (sneak attacks) are always hit
//   damage      weapon min..max by attack charge, x (0.5 + strength / 100), x condition / max condition,
//               x max(0.25, damage / (damage + armor rating)); sneak attacks on unaware targets x4
//   hand-to-hand  skill x (0.1 .. 0.5 by charge) to fatigue; to health once fatigue is gone (x 0.1)
//   blocking    a raised shield (automatic when facing the blow and not attacking):
//               chance = clamp((block + agility / 5 + luck / 10) x (1 + charge) x fatigue term [x 1.25 standing]
//                              - (attack skill + agility / 5 + luck / 10) x fatigue term, 10, 50)
//   knockdown   damage >= agility x fKnockDownMult and a roll >= agility x iKnockDownOddsMult / 100
//               + iKnockDownOddsBase; fatigue at 0 knocks out
//   wear        weapons lose damage x fWeaponDamageMult per hit, the struck armor piece what it absorbed
//   fleeing     rated (1 - health share) x fAIFleeHealthMult + Flee x fAIFleeFleeMult + distance bias, at 100: stands
//               until the foe comes within its weapon's reach, then runs to a path grid point; chosen again every 3 s
//   prices      base value x (buying: 1 - (pc - npc) / 200, selling: 0.5 - (npc - pc) / 200), where
//               pc = (disposition - 50 + mercantile + luck / 10 + personality / 5) x fatigue term, npc alike
// Crime: assault and murder add a bounty (killing whoever attacked first, or creatures, is free);
// guards come to arrest the player.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

static const float kHeldLightShare = 0.5f;           // a held torch's colour on its holder (Actor::heldLight), against OpenMW

// OpenMW's canFight for a creature that only swims (isPureWaterCreature: it swims, and neither walks, flies nor goes
// on two legs): it can fight the player only when they wade, a quarter of their height under the surface
// (World::isWading; the swim depth is 0.9 of it, fSwimHeightScale), or stand in reach and in sight. One that can't
// doesn't start a fight (Actors::engageCombat), and gives up the one it's in (AiCombat::attack)
bool waterCreatureCanFight(World& w, int ri)
{
	const Ref& r = w.refs[ri];
	if (r.recType != REC_CREA || (w.game.actors[r.actor].afloat & 0x71) != 0x10)
		return true;
	const Player& p = w.player;
	if (w.underWater(p.feet[2] + 95.0f * 0.25f / 0.9f))
		return true;
	float reach = kCombatDistance * 0.9f + 40.0f;
	float dx = p.feet[0] - r.pos[0], dy = p.feet[1] - r.pos[1];
	if (dx * dx + dy * dy > reach * reach || fabsf(p.feet[2] - r.pos[2]) > reach)
		return false;
	float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f }, eyeB[3] = { p.feet[0], p.feet[1], playerEyeZ(p) };
	return w.lineOfSight(eyeA, eyeB);
}

int weaponSkill(const Object* w)
{
	if (!w)
		return SKILL_HAND_TO_HAND;
	switch (w->subtype)
	{
	case 0: return SKILL_SHORT_BLADE;
	case 1: case 2: return SKILL_LONG_BLADE;
	case 3: case 4: case 5: return SKILL_BLUNT;
	case 6: return SKILL_SPEAR;
	case 7: case 8: return SKILL_AXE;
	default: return SKILL_MARKSMAN;
	}
}

// A creature's blow of that kind: with the weapon it carries (skeletons, dremora), the weapon's damage for the swing
// with no Strength bonus (OpenMW's Creature::hit); without one, that one of its own attacks between its minimum and
// maximum (the first, if it has fewer)
float creatureDamage(const ActorDef& def, const Object* wpn, float charge, int kind)
{
	if (wpn)
	{
		const u8* r = kind == 1 ? wpn->slash : kind == 2 ? wpn->thrust : wpn->chop;
		return r[0] + (r[1] - r[0]) * charge;
	}
	const auto& at = def.attack[kind >= 0 && kind < (int)def.attack.size() ? kind : 0];
	return at.first + (at.second - at.first) * charge;
}

// ---- Shared rules

bool Session::shieldBlocks(float blockSkill, float agility, float luck, float fatigue, float fatigueMax, bool still,
	float attackSkill, float attackAgility, float attackLuck, float attackFatigue, float attackFatigueMax, float charge)
{
	float x = blockChance(blockSkill, agility, luck, fatigue, fatigueMax, still, attackSkill, attackAgility, attackLuck,
		attackFatigue, attackFatigueMax, charge);                     // (mwmechanics/formulas.cpp)
	return rand() % 100 < (int)x;
}

void Session::knockDown(int ri, bool out)
{
	if (out && ri >= 0)
		w.refs[ri].knockedOut = true;
	Ref& r = w.refs[ri];
	if (r.dead || r.knockTimer > 0.0f)
		return;
	r.knockTimer = out ? 3.5f : 2.7f;
	r.outCold = out;
	r.hitAt = -1.0f;
	r.castSpell = -1;
	if (Actor* a = w.actorOf(ri))
	{
		ActorSet& set = *w.actorsOf(ri);
		if (actorPlay(set, *a, out ? "KnockOut" : "KnockDown", ANIM_HOLD))
		{
			const AnimGroup& g = actorSkeleton(set, a->skeleton).groups[a->group];
			r.knockTimer = fmaxf(r.knockTimer, g.stop - g.start);
		}
	}
	playSound(ri, "Body Fall Medium");
	logf("combat: %s knocked %s", r.id.c_str(), out ? "out" : "down");
}

// fatigueOnly: hand-to-hand on someone standing, which only tires (OpenMW: fists hurt fatigue or, on someone down or
// paralyzed, health; never both)
// spell: a damage spell's health, all at once or a tick a frame while it lasts. OpenMW's applyMagicEffect only takes
// the health: the recoil, the hurt sound, the blood and the "Hit" voice come with blows (its onHit). Played on every
// tick, the Hit started over each frame and froze a burning foe in one stride as it ran
void Session::damageNpc(int ri, float dmg, bool fatigueOnly, bool spell)
{
	Ref& r = w.refs[ri];
	if (spell)
	{
		r.health -= dmg;
		enemyRef = ri;
		enemyUntil = w.time + 6.0f;
		if (r.health <= 0.0f)
			killNpc(ri);
		else if (r.fatigue <= 0.0f)
			knockDown(ri, true);
		return;
	}
	if (fatigueOnly)
	{
		r.fatigue -= dmg;
		playSound(ri, "Hand To Hand Hit");
	}
	else
	{
		r.health -= dmg;
		playSound(ri, "Health Damage");
		// the blood splat (Morrowind.ini [Blood]: red, a skeleton's white, a dwemer machine's gold sparks), one of the
		// three models, where the blow lands: on the side facing the player, a quarter of the width either way along x,
		// 20 to 100 % of the height up (OpenMW's getHitContact and spawnBloodEffect). (The picks from a hash of the
		// moment, not rand(): a seeded test's dice stay as they were)
		int blood = r.actor >= 0 ? w.game.actors[r.actor].blood : 0;
		u32 h = ((u32)(w.time * 1000.0f) + (u32)ri * 2654435761u) * 2246822519u;
		float hx = r.hasBox ? 0.5f * (r.boxMax[0] - r.boxMin[0]) : 30.0f, hy = r.hasBox ? 0.5f * (r.boxMax[1] - r.boxMin[1]) : 30.0f;
		float hz = r.hasBox ? r.boxMax[2] - r.boxMin[2] : 128.0f;
		float dx = w.player.feet[0] - r.pos[0], dy = w.player.feet[1] - r.pos[1], dl = sqrtf(dx * dx + dy * dy);
		if (dl > 1.0f)
			dx /= dl, dy /= dl;
		else
			dx = dy = 0.0f;
		float at[3] = { r.pos[0] + dx * hy + hx * 2.0f * (((h >> 8) & 255) / 255.0f * 0.5f - 0.25f), r.pos[1] + dy * hy,
			r.pos[2] + hz * (0.2f + 0.8f * ((h >> 16) & 255) / 255.0f) };
		int model = (int)((h >> 24) % 3);
		std::string id = "blood" + std::to_string(blood >= 0 && blood < 3 ? blood : 0);
		if (model == 0 || !spawnVfx(id + "_" + std::to_string(model), at, 0.5f))
		{
			if (spawnVfx(id, at, 0.5f))
				bloodModels |= 1;
		}
		else
			bloodModels |= 1 << model;
	}
	enemyRef = ri;
	enemyUntil = w.time + 6.0f;
	if (r.health > 0.0f && !fatigueOnly && rand() % 100 < (int)w.game.gmstf("ivoicehitodds", 30.0f))
		npcSayTopic(ri, "Hit");
	if (r.health <= 0.0f)
		killNpc(ri);
	else if (r.fatigue <= 0.0f)
		knockDown(ri, true);
	else if (w.actorOf(ri) && r.hitAt < 0.0f && r.knockTimer <= 0.0f)
	{
		// one of its hits at random (OpenMW's chooseRandomGroup)
		std::string g = actorRandomGroup(*w.actorsOf(ri), *w.actorOf(ri), "Hit");
		actorPlay(*w.actorsOf(ri), *w.actorOf(ri), g.empty() ? "Hit1" : g.c_str(), ANIM_ONCE);
	}
}

// Lava and the like (HurtStandingActor), every frame stood on it: the flash pulses and Health Damage plays as it ends
// (OpenMW's World::updateActors: the sound unless it is playing), not a solid red screen and the sound each frame
void Session::hurtPlayer(float health)
{
	if (testGod || playerDead)
		return;
	w.stats.health -= health;
	if (hurtFlashOngoing())
		playSound(-1, "Health Damage");
	if (w.stats.health <= 0.0f)
		damagePlayer(0.0f, false);      // the death screen
}

void Session::damagePlayer(float dmg, bool fatigueOnly)
{
	if (testGod)
		return;
	PlayerStats& s = w.stats;
	if (fatigueOnly)
	{
		s.fatigue = fmaxf(0.0f, s.fatigue - dmg);
		playSound(-1, "Hand To Hand Hit");
	}
	else
	{
		s.health -= dmg;
		playSound(-1, "Health Damage");
	}
	hurtFlash = 1.0f;
	sheatheTimer = 0.0f;
	if (weaponDrawn && vm.action == VM_IDLE)
		vm.play(VM_HIT, "Hit1");
	if (s.health <= 0.0f && !playerDead)
		playerDies();
}

// A blow (or arrow) from an actor reaches the player: block, armor, wear, knockdown
void Session::npcHitsPlayer(int ri, float dmg, bool fatigueOnly, int skill, bool unarmed, bool ranged)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	PlayerStats& s = w.stats;
	// Diseased creatures pass their disease on with a blow, not with a shot (mwmechanics/formulas.cpp catchDiseases; OpenMW's
	// diseaseContact is melee only)
	if (!ranged)
		catchDiseases(ri);
	// A knocked-down player takes fCombatKODamageMult x the blow (OpenMW, for every attacker; not for paralysis alone)
	if (w.player.knockTimer > 0.0f && w.effectTotal(45) <= 0.0f)
		dmg *= w.game.gmstf("fcombatkodamagemult", 1.5f);
	const float rawDmg = fatigueOnly ? 0.0f : dmg;            // knockdown judges the blow before armor
	// A raised shield: automatic while the player faces the attacker (mid-swing too, as in OpenMW)
	InventoryItem* shield = nullptr;
	for (auto& it : w.inventory)
		if (it.equipped && w.game.object(it.id) && w.game.object(it.id)->recType == REC_ARMO
			&& w.game.object(it.id)->subtype == ARMO_SHIELD && itemCondition(it) > 0)
			shield = &it;
	float toAttacker = atan2f(r.pos[0] - w.player.feet[0], r.pos[1] - w.player.feet[1]);
	// (OpenMW: within fCombatBlockLeftAngle .. fCombatBlockRightAngle of facing; not while down or paralyzed; melee
	// only, arrows and thrown weapons aren't blocked)
	float blockAngle = angleDiff(toAttacker, w.player.yaw) * 57.2958f;
	if (shield && !ranged && w.player.knockTimer <= 0.0f && w.effectTotal(45) <= 0.0f
		&& blockAngle >= w.game.gmstf("fcombatblockleftangle", -90.0f) && blockAngle <= w.game.gmstf("fcombatblockrightangle", 20.0f)
		&& shieldBlocks(s.skills[SKILL_BLOCK], s.attributes[ATTR_AGILITY], s.attributes[ATTR_LUCK], s.fatigue, s.fatigueMax,
			playerMoveForward <= 0.0f, def.skills[skill], def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK], r.fatigue,
			r.fatigueMax, 0.5f))
	{
		wearItem(shield, dmg);
		useSkill(SKILL_BLOCK, 0);
		blockUntil = w.time + 0.8f;
		// The block tires: fFatigueBlockBase + load x fFatigueBlockMult + the attacker's weapon weight x swing x
		// fWeaponFatigueBlockMult
		const Object* aw = w.game.object(def.weapon);
		s.fatigue -= w.game.gmstf("ffatigueblockbase", 0.0f) + playerLoad * w.game.gmstf("ffatigueblockmult", 0.0f)
			+ (aw && aw->recType == REC_WEAP ? aw->weight * 0.5f * w.game.gmstf("fweaponfatigueblockmult", 0.0f) : 0.0f);
		const Object* so = w.game.object(shield->id);
		float wt = kArmorSlotBaseWeight[ARMO_SHIELD];
		playSound(-1, so->weight <= wt * w.game.gmstf("flightmaxmod", 0.6f) + 0.0005f ? "Light Armor Hit"
			: so->weight <= wt * w.game.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? "Medium Armor Hit" : "Heavy Armor Hit");
		logf("combat: player blocked %s", r.id.c_str());
		return;
	}
	if (!fatigueOnly)
	{
		float after = fmaxf(1.0f, applyArmor(dmg, playerArmor()));        // OpenMW: at least 1
		// The piece of armor that took it (OpenMW's pickRandomArmor: roll 0..99; cuirass below 30, helmet 30s, greaves
		// 40s, boots 50s, left pauldron 60s, right 70s, left hand 80-84, right hand 85-89, shield 90+) wears down by
		// the damage it took off, and trains its armor skill; a bare slot trains Unarmored. A creature's own attack
		// (no weapon) does not wear the armor
		int roll = rand() % 100;
		int slot = roll >= 90 ? ARMO_SHIELD : roll >= 85 ? 7 : roll >= 80 ? 6 : roll >= 70 ? 3 : roll >= 60 ? 2
			: roll >= 50 ? 5 : roll >= 40 ? 4 : roll >= 30 ? 0 : 1;
		bool armored = false;
		for (auto& it : w.inventory)
			if (it.equipped)
				if (const Object* o = w.game.object(it.id))
					if (o->recType == REC_ARMO && (o->subtype == slot || (slot == 6 && o->subtype == 9) || (slot == 7 && o->subtype == 10)))
					{
						armored = true;
						float wt = kArmorSlotBaseWeight[o->subtype];
						useSkill(o->weight <= wt * w.game.gmstf("flightmaxmod", 0.6f) + 0.0005f ? SKILL_LIGHT_ARMOR
							: o->weight <= wt * w.game.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? SKILL_MEDIUM_ARMOR : SKILL_HEAVY_ARMOR, 0);
						float wear = -floorf(after - dmg);           // (OpenMW: floor(adjusted - raw), taken off)
						if (!(unarmed && def.creature) && wear > 0.0f)
							wearItem(&it, wear);
						break;
					}
		if (!armored)
			useSkill(SKILL_UNARMORED, 0);      // a blow on a bare spot trains Unarmored
		dmg = after;
	}
	dmg *= difficultyScale(true);
	logf("combat: %s hits the player for %.1f%s", r.id.c_str(), dmg, fatigueOnly ? " fatigue" : "");
	damagePlayer(dmg, fatigueOnly);
	// Knockdown: a heavy blow (before armor) the player's agility doesn't hold against (OpenMW's applyStagger)
	int agi = s.attributes[ATTR_AGILITY];
	if (!playerDead && rawDmg > 0.0f && rawDmg >= agi * w.game.gmstf("fknockdownmult", 0.6f) && rand() % 100 >= knockdownOdds(agi))
	{
		w.player.knockTimer = 2.0f;
		logf("combat: player knocked down");
	}
	if (!playerDead && s.fatigue <= 0.0f && w.player.knockTimer <= 0.0f)
		w.player.knockTimer = 3.0f;
}

// A weapon blow's roll against its hit chance (percent). Vanilla: it lands or misses. Glancing Blows and
// Crits (GBAC 1.2.1, Options > Combat): it always lands; what would miss glances (x0.5), and a crit
// ((chance / 10)^2 percent) does x4 in melee, x1.5 at range; every blow is scaled by
//   f = (hit + crit) / (glance x 0.5 + hit + crit x critMod)
// so the average damage stays vanilla's. Skill progress takes the same factors (glance 0.5 f, crit critMod f).
// vanilla: this blow keeps vanilla's rules even under GBAC (sneak attacks, plain weapons on the immune)
Session::BlowRoll Session::rollBlow(float chance, bool ranged, bool vanilla)
{
	BlowRoll b;
	if (combatMode != COMBAT_GBAC || vanilla)
	{
		b.lands = rand() % 100 < (int)chance;
		return b;
	}
	float h = fmaxf(0.0f, chance);
	float crit = (h * 0.1f) * (h * 0.1f);
	float hit = h - crit, glance = 100.0f - h;
	float critMod = ranged ? 1.5f : 4.0f, glanceMod = 0.5f;
	float f = (hit + crit) / (glance * glanceMod + hit + crit * critMod);
	if (h < (float)(rand() % 100 + 1))
		b.kind = 0;
	else
		b.kind = crit >= (float)(rand() % 100 + 1) ? 2 : 1;
	float k = b.kind == 0 ? glanceMod : b.kind == 2 ? critMod : 1.0f;
	b.damage = k * f;
	b.skill = k * f;
	return b;
}

void Session::blowSound(int ref, const BlowRoll& b)
{
	if (b.kind == 0)
		playSound(ref, "Hand To Hand Hit 2");
	else if (b.kind == 2 && combatMode == COMBAT_GBAC)
		playSound(ref, "critical damage");
}

// OpenMW's applyElementalShields, on any blow or shot that lands: the victim's Fire / Lightning / Frost Shield burns
// the attacker, less the attacker's own resistance to that element. False when the attacker died of it
bool Session::playerShieldsBurn(int attacker)
{
	Ref& r = w.refs[attacker];
	const ActorDef& def = w.game.actors[r.actor];
	for (int e = 4; e <= 6; e++)
	{
		float mag = w.effectTotal(e);
		if (mag <= 0.0f)
			continue;
		int element = e == 4 ? 14 : e == 5 ? 15 : 16;
		float dmg = elementalShieldDamage(mag, def.skills[10], def.attributes[ATTR_WILLPOWER], def.attributes[ATTR_LUCK],
			r.fatigue, r.fatigueMax, actorResistBase(attacker, element), rand() % 100);
		damageNpc(attacker, dmg * difficultyScale(false), false);
		logf("combat: %s burned by the player's shield for %.1f", r.id.c_str(), dmg);
		if (r.dead)
			return false;
	}
	return true;
}

bool Session::npcShieldsBurn(int victim)
{
	const PlayerStats& s = w.stats;
	for (int e = 4; e <= 6; e++)
	{
		float mag = w.actorEffect(victim, e);
		if (mag <= 0.0f)
			continue;
		int element = e == 4 ? 14 : e == 5 ? 15 : 16;
		float dmg = elementalShieldDamage(mag, s.skills[10], s.attributes[ATTR_WILLPOWER], s.attributes[ATTR_LUCK], s.fatigue,
			s.fatigueMax, resistBase(element), rand() % 100);
		logf("combat: %s's shield burns the player for %.1f", w.refs[victim].id.c_str(), dmg);
		damagePlayer(dmg * difficultyScale(true), false);
		if (playerDead)
			return false;
	}
	return true;
}

// An NPC's blow lands now (the hit moment of its attack animation)
void Session::npcStrike(int ri)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = w.game.object(def.weapon);
	if (wpn && (wpn->recType != REC_WEAP || isRanged(wpn)))
		wpn = nullptr;
	float charge = frand();                    // the swing's strength (OpenMW's AI: uniform 0..1)
	// The swing tires them: fFatigueAttackBase + fWeaponFatigueMult x weapon weight x swing (OpenMW's applyFatigueLoss)
	r.fatigue -= w.game.gmstf("ffatigueattackbase", 2.0f) + (wpn ? wpn->weight : 0.0f) * charge * w.game.gmstf("fweaponfatiguemult", 0.25f);
	// OpenMW's getHitChance: the attack term less the player's evasion (none while down or paralyzed), rounded
	bool helpless = w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f;
	float chance = roundf(attackTermOf(attackSkill(def, wpn), def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK],
		r.fatigue, r.fatigueMax, w.actorEffect(ri, 117), w.actorEffect(ri, 47)) - playerDefense(helpless));
	BlowRoll roll = rollBlow(chance, false, false);
	// (a missed blow follows through as the weakest)
	if (Actor* a = w.actorOf(ri))
		attackFollowThrough(*w.actorsOf(ri), *a, roll.lands ? charge : 0.0f);
	if (!roll.lands)
	{
		playSound(ri, charge > 0.6f ? "SwishL" : "SwishM");
		return;
	}
	blowSound(-1, roll);
	// Its weapon's on-strike enchantment, before the block (OpenMW)
	npcStrikeEnchantment(ri, wpn);
	if (!playerShieldsBurn(ri))
		return;
	if (def.creature && (wpn || !def.attack.empty()))
		npcHitsPlayer(ri, creatureDamage(def, wpn, charge, r.attackKind) * roll.damage, false, weaponSkill(wpn), !wpn);
	else if (wpn)
		npcHitsPlayer(ri, weaponDamage(wpn, r.attackKind, charge, def.attributes[ATTR_STRENGTH]) * roll.damage, false,
			weaponSkill(wpn));
	else    // fists: GBAC leaves them alone while the player still has fatigue (they only tire)
	{
		// Hand to hand: Hand-to-hand x (fMinHandToHandMult + (fMax - fMin) x swing); on a player who is down or
		// paralyzed it is health damage x fHandtoHandHealthPer (OpenMW's getHandToHandDamage)
		float lo = w.game.gmstf("fminhandtohandmult", 0.1f), hi = w.game.gmstf("fmaxhandtohandmult", 0.5f);
		float dmg = def.skills[SKILL_HAND_TO_HAND] * (lo + (hi - lo) * charge);
		if (w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f)
			npcHitsPlayer(ri, dmg * w.game.gmstf("fhandtohandhealthper", 0.1f) * roll.damage, false, SKILL_HAND_TO_HAND, true);
		else
			npcHitsPlayer(ri, dmg * (w.stats.fatigue > 0.0f ? 1.0f : roll.damage), true, SKILL_HAND_TO_HAND, true);
	}
}

// ---- Player

// Test runs: the player's weapon lands a blow on that reference (HIT:id)
void Session::testHit(const std::string& id)
{
	int ri = testFindRef(id);
	if (!w.active(ri) || w.refs[ri].actor < 0)
	{
		logf("test: %s not loaded", id.c_str());
		return;
	}
	const Object* wpn = playerWeapon();
	playerHitsNpc(ri, 5.0f, weaponSkill(wpn), false, false);
	logf("test: hit %s with %s", id.c_str(), wpn ? wpn->id.c_str() : "hands");
}

// The player's blow or arrow reaches an NPC (hit chance already rolled)
void Session::playerHitsNpc(int target, float damage, int skill, bool fatigueOnly, bool ranged)
{
	{
		// HitOnMe: which weapon hit it (scripts ask, e.g. the Heart of Lorkhan and Sunder / Keening)
		const Object* wpn = playerWeapon();
		w.refs[target].hitBy = wpn ? lower(wpn->id) : "";
		w.refs[target].attacked = true;
	}
	Ref& t = w.refs[target];
	const ActorDef& def = w.game.actors[t.actor];
	const PlayerStats& s = w.stats;
	// harness GOD: one blow (armor and resistances still apply), but not where the script runs the fight
	// (the Heart of Lorkhan resets its health and counts Sunder and Keening hits)
	bool scripted = t.script >= 0 && w.scripts[t.script].script && w.scripts[t.script].script->scriptedHits;
	if (testGod && testGodBlows && !fatigueOnly && !scripted)
		damage = fmaxf(damage, t.health * 4.0f + 50.0f);
	// OpenMW: a blow on someone not in combat who fails to notice the player (the awareness check) is a critical
	// strike in melee (fCombatCriticalStrikeMult), and x fCombatKODamageMult at range; on someone knocked down, x
	// fCombatKODamageMult either way
	bool unaware = !npcAware(target);
	bool sneakAttack = unaware && !ranged;
	// (knocked down, not just paralyzed: Paralyze holds them with the same timer, but OpenMW counts only a knockdown)
	bool knockedDown = w.refs[target].knockTimer > 0.0f && w.actorEffect(target, 45) <= 0.0f;

	// Being attacked makes them fight back; attacking someone peaceful is assault (the guards
	// don't mind you killing mudcrabs, or whoever attacked you first). A companion forgives the
	// first blows in melee (OpenMW: the 4th friendly hit is no longer forgiven, shots never are; dialogue's Friendly Hit
	// counts them)
	bool wasPeaceful = t.ai != AI_COMBAT;
	bool forgiven = false;
	if (wasPeaceful && !ranged && w.followsPlayer(target))
		forgiven = ++t.friendlyHits < 4;
	if (!forgiven)
	{
		makeHostile(target);
		if (wasPeaceful && !def.creature && !t.aggressor)
			crimeSeen(CRIME_ASSAULT, 0, target), reportCrime(target, kBountyAssault);
	}

	// The weapon's on-strike enchantment: before the block and the damage (OpenMW; Soul Trap then works on a killing
	// blow, and a blocked blow still casts). Arrows cast theirs in mwworld/projectilemanager.cpp
	if (!ranged && !fatigueOnly)
		strikeEnchantment(target);

	// Their shield, if they see it coming
	const Object* shield = w.game.object(def.shield);
	float toPlayer = atan2f(w.player.feet[0] - t.pos[0], w.player.feet[1] - t.pos[1]);
	float npcBlockAngle = angleDiff(toPlayer, t.rot[2]) * 57.2958f;
	// (OpenMW's blockMeleeAttack: not down or paralyzed, the shield out; being unaware or mid-swing doesn't stop it)
	if (shield && !ranged && t.knockTimer <= 0.0f
		&& npcBlockAngle >= w.game.gmstf("fcombatblockleftangle", -90.0f) && npcBlockAngle <= w.game.gmstf("fcombatblockrightangle", 20.0f)
		&& shieldBlocks(def.skills[SKILL_BLOCK], def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK], t.fatigue,
			t.fatigueMax, true, s.skills[skill], s.attributes[ATTR_AGILITY], s.attributes[ATTR_LUCK], s.fatigue,
			s.fatigueMax, fmaxf(0.0f, attackCharge)))
	{
		if (Actor* a = w.actorOf(target))
			actorPlay(*w.actorsOf(target), *a, "Block", ANIM_ONCE);
		// the block tires them: fFatigueBlockBase + the player's weapon weight x swing x fWeaponFatigueBlockMult
		// (their load isn't kept: OpenMW adds load x fFatigueBlockMult)
		const Object* pw = playerWeapon();
		t.fatigue -= w.game.gmstf("ffatigueblockbase", 0.0f)
			+ (pw ? pw->weight * fmaxf(0.0f, attackCharge) * w.game.gmstf("fweaponfatigueblockmult", 0.0f) : 0.0f);
		float wt = kArmorSlotBaseWeight[ARMO_SHIELD];
		playSound(target, shield->weight <= wt * w.game.gmstf("flightmaxmod", 0.6f) + 0.0005f ? "Light Armor Hit"
			: shield->weight <= wt * w.game.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? "Medium Armor Hit" : "Heavy Armor Hit");
		logf("combat: %s blocked", t.id.c_str());
		return;
	}
	// OpenMW's order: Resist Normal Weapons, then the critical / knockdown multipliers (they stack), then armor.
	// Ghosts, daedra ...: ordinary weapons do less; silver, magical or enchanted ones get through, and so do fists
	// (resistNormalWeapon judges only a weapon); arrows are judged in mwworld/projectilemanager.cpp
	const Object* wpnNow = playerWeapon();
	float normalResist = normalWeaponResist(target);
	if (normalResist != 0.0f && wpnNow && !ranged)
	{
		bool special = (wpnNow->flags & 3) || wpnNow->magic;         // 1 magical, 2 silver
		if (!special)
			damage *= 1.0f - fminf(1.0f, normalResist / 100.0f);
		if (!special && normalResist >= 100.0f)
			notify(w.game.gmst("smagictargetresistsweapons", "Your weapon has no effect."));
	}
	if (sneakAttack)
	{
		damage *= w.game.gmstf("fcombatcriticalstrikemult", 4.0f);
		notify(w.game.gmst("stargetcriticalstrike", "Critical Strike!"));
		if (!fatigueOnly)
			playSound(target, "critical damage");
		logf("combat: critical strike on %s", t.id.c_str());
	}
	if (ranged && unaware)
		damage *= w.game.gmstf("fcombatkodamagemult", 1.5f);
	if (knockedDown)
		damage *= w.game.gmstf("fcombatkodamagemult", 1.5f);
	const float rawDamage = fatigueOnly ? 0.0f : damage;       // knockdown judges the blow before armor
	// (+ Shield); OpenMW: armor leaves at least 1, but a blow that Resist Normal Weapons took down to nothing does none
	// (Creeper's "immune to normal weapons": an ordinary dagger does not scratch him)
	if (!fatigueOnly)
		damage = damage > 0.0f ? fmaxf(1.0f, applyArmor(damage, def.armor + w.actorEffect(target, 3))) : 0.0f;
	damage *= difficultyScale(false);
	logf("combat: player hits %s for %.1f%s", t.id.c_str(), damage, fatigueOnly ? " fatigue" : "");
	{
		char n[16];
		snprintf(n, sizeof(n), "%.0f", damage);
		addIndicator(n, fatigueOnly && t.fatigue > 0.0f ? 0x33cc4c : 0xff334c);
	}
	// A blow of the player's on someone whose script has OnPCHitMe sets it (OpenMW: the script clears it itself)
	if (t.script >= 0)
		if (float* hitMe = w.scripts[t.script].local("onpchitme"))
			*hitMe = 1.0f;
	damageNpc(target, damage, fatigueOnly);
	if (t.dead)
		return;
	int agi = def.attributes[ATTR_AGILITY];
	if (rawDamage > 0.0f && rawDamage >= agi * w.game.gmstf("fknockdownmult", 0.6f) && rand() % 100 >= knockdownOdds(agi))
		knockDown(target, false);
	(void)ranged;
}

// Bow, crossbow or thrown weapon: the projectile leaves along the view
void Session::playerFire(float charge)
{
	const Object* wpn = playerWeapon();
	const Object* ammo = playerAmmo();
	if (!wpn || !ammo)
		return;
	// (no fatigue spent: OpenMW charges it only for melee blows)
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	float from[3] = { w.player.feet[0] + dir[0] * 20.0f, w.player.feet[1] + dir[1] * 20.0f, playerEyeZ(w.player) - 12.0f };
	bool thrown = wpn->subtype == WEAP_THROWN;
	float lo = thrown ? w.game.gmstf("fthrownweaponminspeed", 300.0f) : w.game.gmstf("fprojectileminspeed", 400.0f);
	float hi = thrown ? w.game.gmstf("fthrownweaponmaxspeed", 1000.0f) : w.game.gmstf("fprojectilemaxspeed", 3000.0f);
	std::string ammoId = lower(ammo->id), wpnId = lower(wpn->id);
	fireProjectile(-1, from, dir, lo + (hi - lo) * charge, ammoId, wpnId, charge, "");
	w.removeItem(ammo->id, 1);
	// The last of the stack: equip the next of the same kind, if any
	if (!playerAmmo())
		for (auto& it : w.inventory)
			if (lower(it.id) == ammoId && it.count > 0)
				it.equipped = true;
	playSound(-1, wpn->subtype == WEAP_CROSSBOW ? "crossbowShoot" : wpn->subtype == WEAP_BOW ? "bowShoot" : "SwishM");
	logf("combat: player fires %s (%.0f%%)", ammoId.c_str(), charge * 100.0f);
}

void Session::combatUpdate(const PlayerInput& in, float dt, bool menu)
{
	PlayerStats& s = w.stats;
	playerMoveForward = in.moveY;
	hurtFlash = fmaxf(0.0f, hurtFlash - dt * 2.0f);
	if (playerDead)
		return;

	// Fatigue comes back: fFatigueReturnBase + fFatigueReturnMult x endurance per second
	if (testFatiguePin >= 0.0f)
		s.fatigue = fminf(s.fatigueMax, testFatiguePin);     // (a spec test holds the level it set)
	// (not under a menu: OpenMW pauses the world in them, and the barter screen's prices count fatigue)
	else if (s.fatigue < s.fatigueMax && !testNoFatigueRegen && !menu)
		s.fatigue = fminf(s.fatigueMax, s.fatigue + (w.game.gmstf("ffatiguereturnbase", 2.5f)
			+ w.game.gmstf("ffatiguereturnmult", 0.02f) * s.attributes[ATTR_ENDURANCE]) * dt);

	updateDetection(dt);

	// X: the first press draws the weapon (or raises the fists); holding winds up, release strikes
	// (a longer wind-up hits harder). Ranged weapons need ammunition equipped.
	bool canFight = !menu && w.controlsEnabled && w.fightingEnabled && w.player.knockTimer <= 0.0f;
	const Object* wpn = playerWeapon();
	bool tool = playerToolItem() != nullptr;
	// a lockpick or probe has its own draw, use and put away (OpenMW's PickProbe; the "1h" stance otherwise)
	bool pickProbe = tool && !wpn && vm.groupLength("PickProbe") > 0.0f;
	sheatheTimer += dt;
	auto ready = [&]() {
		weaponDrawn = true;
		std::string g = pickProbe ? "PickProbe" : vmGroup(wpn, tool);
		vm.play(VM_EQUIP, (g + "Eq").c_str());
		drawTimer = fmaxf(0.3f, vm.groupLength((g + "Eq").c_str()));
		// The weapon's own "up" sound by its type (OpenMW's weapon type table); raised fists make none
		if (wpn)
		{
			static const char* up[] = { "Shortblade", "Longblade", "Longblade", "Blunt", "Blunt", "Blunt", "Spear", "Blunt", "Blunt",
				"Bow", "Crossbow", "Blunt" };
			int t = wpn->subtype;
			playSound(-1, std::string("Item Weapon ") + up[t >= 0 && t <= 11 ? t : 3] + " Up");
		}
	};
	// Ready Weapon (Xbox layout: X): out if it's away; away if it's out (below)
	// (a cast is one upper-body state for its whole animation: no drawing, sheathing or blow until it ends)
	bool casting = castLock > 0.0f || vm.action == VM_CAST;
	bool putAway = in.sheathe || (in.readyToggle && weaponDrawn);
	if (canFight && !casting && in.readyToggle && !weaponDrawn)
	{
		sheatheTimer = 0.0f;
		ready();
	}
	// The next blow waits for the last one's follow-through, and for a cast to end (OpenMW: one attack animation at a time)
	bool busy = attackCharge < 0.0f && (vm.action == VM_FOLLOW || casting);
	if (canFight && in.attack && !busy)
	{
		sheatheTimer = 0.0f;
		if (!weaponDrawn)
			ready();
		if (drawTimer > 0.0f)
		{
			// (held until it's up: the try follows, even let go the next frame, as a weapon's wind-up started then would)
			drawTimer -= dt;
			if (drawTimer <= 0.0f && pickProbe)
				pickQueued = true;
		}
		else if (isRanged(wpn) && !playerAmmo())
		{
			if (attackCharge < -0.5f)
				notify("You have no ammunition equipped.");
			attackCharge = -0.9f;
		}
		else if (pickProbe)
		{
			// No wind-up: its animation (a key's turn) played through, the lock or trap tried as it starts, one try a
			// press (OpenMW's PickProbe: straight to the release, played to its end). It was the dagger's chop
			if (attackCharge < -0.95f)
			{
				vm.play(VM_FOLLOW, "PickProbe");
				swingCount++;
				playerSwing(1.0f, in);
			}
			pickQueued = false;
			attackCharge = -0.9f;
		}
		else
		{
			if (attackCharge < 0.0f)
			{
				attackCharge = 0.0f;
				attackKind = isRanged(wpn) ? 0 : in.moveY > 0.3f ? 2 : fabsf(in.moveX) > 0.3f ? 1 : 0;
				if (isRanged(wpn))
					playSound(-1, wpn->subtype == WEAP_CROSSBOW ? "crossbowPull" : wpn->subtype == WEAP_BOW ? "bowPull" : "SwishS");
				vm.speed = fmaxf(0.5f, wpn ? wpn->speed : 1.0f);
				vm.play(VM_WINDUP, (std::string(vmGroup(wpn, tool)) + (isRanged(wpn) ? "Shoot" : attackKind == 2 ? "Thrust"
					: attackKind == 1 ? "Slash" : "Chop")).c_str(), true);
			}
			attackCharge = fminf(1.0f, attackCharge + dt / (0.8f / fmaxf(0.5f, wpn ? wpn->speed : 1.0f)));
			vm.charge = attackCharge;
		}
	}
	else if (attackCharge >= 0.0f && canFight && !vm.windupReached())
	{
		// let go too soon: the wind-up plays on to its min attack mark first
	}
	else if (attackCharge >= 0.0f)
	{
		if (canFight)
		{
			swingCount++;
			if (isRanged(wpn))
			{
				playerFire(fmaxf(0.1f, attackCharge));
				vm.play(VM_FOLLOW, (std::string(vmGroup(wpn, tool)) + "ShootF").c_str());
				blowStrength = 1.0f;
			}
			else
			{
				// the follow-through by how hard the blow was (small / medium / large)
				float strength = playerSwing(fmaxf(0.1f, attackCharge), in);
				blowStrength = strength;        // (the body seen from outside follows through the same way)
				vm.playFollow((std::string(vmGroup(wpn, tool)) + (attackKind == 2 ? "ThrustF" : attackKind == 1 ? "SlashF"
					: "ChopF")).c_str(), strength);
			}
		}
		else if (vm.action == VM_WINDUP)
			vm.action = VM_IDLE;          // a menu or knockdown cut the wind-up short: the arms must not stay drawn back
		attackCharge = -1.0f;
	}
	else if (pickQueued && pickProbe && canFight && !busy && weaponDrawn)
	{
		// let go while it came up: tried once it's up, as a weapon's early let-go still swings when its wind-up gets there
		if ((drawTimer -= dt) <= 0.0f)
		{
			pickQueued = false;
			vm.play(VM_FOLLOW, "PickProbe");
			swingCount++;
			playerSwing(1.0f, in);
		}
	}
	else if (attackCharge < -0.5f && !in.attack)
		attackCharge = -1.0f;
	// Put the weapon away after a while without fighting, or when a menu opens
	if (weaponDrawn && !casting && (putAway || sheatheTimer > 20.0f || (menu && screen != SCR_NONE)) && attackCharge < 0.0f)
	{
		weaponDrawn = false;
		pickQueued = false;
		vm.play(VM_UNEQUIP, (std::string(pickProbe ? "PickProbe" : vmGroup(wpn, tool)) + "Uneq").c_str());
	}

	float headTrack = w.game.gmstf("fmaxheadtrackdistance", 400.0f);
	float headTrackIn = w.game.gmstf("finteriorheadtrackmult", 0.5f);
	const bool torches = w.useTorches();
	// (the player's own body in third person, as see-through as anyone under Invisibility or Chameleon)
	if (!body.set.actors.empty())
		body.set.actors[0].alpha = actorSeeThrough(true, w.effectTotal(39), w.effectTotal(40));
	w.forLoadedActors([&](int i) {
		Ref& r = w.refs[i];
		if (r.actor < 0 || r.dead || !r.visible())
			return;
		// A torch is held out when it's time for torches, in place of the shield; in a fight the shield comes back
		// (OpenMW's updateEquippedLight). Only by someone who carries one: OpenMW equips a light from the inventory,
		// and without one nothing changes (the arm was raised round an empty hand, and the shield put away for nothing)
		if (Actor* ta = w.actorOf(i))
		{
			bool out = false;
			for (const ActorMesh& m : ta->meshes)
				out |= torches && m.hand == 1;
			if (out && r.ai == AI_COMBAT)
				for (const ActorMesh& m : ta->meshes)
					if (m.hand == 2)
					{
						out = false;
						break;
					}
			if (out != ta->torchOut)
			{
				// The torch lights the one holding it (its own colour, the side of a body facing a hand-held light):
				// people's torches light no one else and nothing around, the console can't spare the vertex work
				const Object* npc = w.game.object(r.id);
				const Object* light = nullptr;
				for (size_t k = 0; out && npc && !light && k < npc->items.size(); k++)
					if (const Object* o = w.game.object(npc->items[k].second))
						if (o->recType == REC_LIGH && o->lightRadius > 0 && (o->flags & 2))     // (LHDT 0x2: can be carried)
							light = o;
				for (int k = 0; k < 3; k++)
					ta->heldLight[k] = light ? light->lightColor[k] / 255.0f * kHeldLightShare : 0.0f;
			}
			ta->torchOut = out;
			ta->alpha = actorSeeThrough(false, w.actorEffect(i, 39), w.actorEffect(i, 40));
		}
		// (held in a pose for a side-by-side screenshot, tools/test/sidebyside.py: no AI, no head turning)
		if (const Actor* fa = w.actorOf(i))
			if (fa->frozen)
				return;
		if (!r.effects.empty() && !menu)
			updateActorEffects(i, dt);
		if (r.dead)
			return;
		if (w.bounty <= 0)
			r.alarmed = false;            // the crime is paid for: calm again (OpenMW's crime id)
		// (the player's rule: fFatigueReturnBase + fFatigueReturnMult x their Endurance a second)
		if (r.fatigue < r.fatigueMax && !menu)
			r.fatigue = fminf(r.fatigueMax, r.fatigue + (w.game.gmstf("ffatiguereturnbase", 2.5f)
				+ w.game.gmstf("ffatiguereturnmult", 0.02f) * w.game.actors[r.actor].attributes[ATTR_ENDURANCE]) * dt);
		// Out cold under 0 fatigue, whatever brought it there (OpenMW: a hit, a Damage Fatigue spell ...), and the time on
		// the floor for everyone down, in a fight or not (it was counted only in a fight: anyone else stayed down)
		if (!menu)
		{
			if (r.fatigue < 0.0f && r.knockTimer <= 0.0f)
				knockDown(i, true);
			if (r.knockTimer > 0.0f)
			{
				npcKnockUpdate(i, dt);
				return;
			}
		}
		// People near the player turn their heads to them (OpenMW's updateHeadTracking): within fMaxHeadTrackDistance
		// (x fInteriorHeadTrackMult indoors), only when the player is in front of them; the head turns at most 30 degrees
		// and tilts at most 40 up or down to the player's eyes, easing there at 5 a second. (OpenMW also wants line of
		// sight and that they noticed the player, and turns them to other people too: left out, for the 3DS's time)
		if (r.recType == REC_NPC_)
			if (Actor* ac = w.actorOf(i))
			{
				float yaw = 0.0f, pitch = 0.0f;
				float reach = headTrack * (w.cells[w.current].interior ? headTrackIn : 1.0f);
				float dx = w.player.feet[0] - r.pos[0], dy = w.player.feet[1] - r.pos[1];
				float dz = playerEyeZ(w.player) - (r.pos[2] + 110.0f);
				float d = sqrtf(dx * dx + dy * dy + dz * dz);
				if (w.distanceToPlayer(i) < reach && d > 1.0f)
				{
					float diff = angleDiff(atan2f(dx, dy), r.rot[2]);
					if (fabsf(diff) < 1.5708f)
					{
						float up = dz / d;
						const float kYaw = 0.5236f, kPitch = 0.6981f;
						yaw = fmaxf(-kYaw, fminf(kYaw, -diff * (1.0f - up * up)));
						pitch = fmaxf(-kPitch, fminf(kPitch, asinf(up)));
					}
				}
				float ease = fminf(1.0f, dt * 5.0f);
				ac->headYaw += (yaw - ac->headYaw) * ease;
				ac->headPitch += (pitch - ac->headPitch) * ease;
				if (fabsf(ac->headYaw) < 0.001f)
					ac->headYaw = 0.0f;
				if (fabsf(ac->headPitch) < 0.001f)
					ac->headPitch = 0.0f;
			}
		// ToggleAI off (the console): they stand where they are, their packages and fights held
		if (!w.aiEnabled)
		{
			if (Actor* fa = w.actorOf(i))
				if (fa->mode == ANIM_LOOP)
					actorPlay(*w.actorsOf(i), *fa, "Idle", ANIM_IDLE);
			r.wandering = false;
			return;
		}
		if (r.ai == AI_IDLE)
		{
			// A timed wander runs out (the other packages count theirs in npcPackage)
			aiSettle(r);
			if (r.aiPackage == AIPKG_WANDER && r.aiDuration > 0.0f && w.gameHour - r.aiStart >= r.aiDuration)
				aiFinish(w, r);
			// The aggressive attack on sight (when they notice the player)
			// Morrowind's attack on sight: fight + iFightDistanceBase - fFightDistanceMultiplier x distance
			// (+ (50 - disposition) x fFightDispMult for people) reaching 100 (fight 90: within 2000 units,
			// a rat's 85: 1000, a mudcrab's 83: 600), once they've noticed the player
			// (OpenMW's getFightTerm: fight + int(the distance and disposition terms); creatures count disposition 50;
			// then line of sight and the awareness check)
			float rating = fightTermOf(i, w.distanceToPlayer(i));        // (mwmechanics/formulas.cpp)
			bool calm = r.calmUntil > w.time || w.actorEffect(i, w.game.actors[r.actor].creature ? 50 : 49) > 0.0f;
			float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f }, eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
			if (r.duelFoe >= 0 && !menu)
				npcDuel(i, dt);
			else if (rating >= 100.0f && !menu && !calm && npcAware(i) && w.lineOfSight(head, eye) && waterCreatureCanFight(w, i))
			{
				r.aggressor = true;
				makeHostile(i);
			}
			else if (!menu && r.recType == REC_NPC_ && (r.duelFoe = guardFoe(i)) >= 0)
				npcDuel(i, dt);
			else if (npcTalkStand(i, dt))
				;
			else if (w.bounty > 0 && lower(w.game.actors[r.actor].cls) == "guard" && !menu)
				guardCheck(i, dt);
			else if (r.ally && !menu)
				allyFight(i, dt);
			else if (r.aiPackage > AIPKG_WANDER)
				npcPackage(i, dt);
			else if (w.distanceToPlayer(i) < kActorViewDistance + 500.0f)
				npcWander(i, dt);         // as far as they're drawn: a far one left mid-walk would walk in place
			else if (Actor* fa = w.actorOf(i))
			{
				if (fa->mode == ANIM_LOOP)
				{
					actorPlay(*w.actorsOf(i), *fa, "Idle", ANIM_IDLE);
					r.wandering = false;
				}
			}
			if (!menu && (rand() % 10000) < w.game.gmstf("fvoiceidleodds", 10.0f) * dt
				&& w.distanceToPlayer(i) < 1200.0f && idleVoiceOk(i))
				npcSayTopic(i, "Idle");
			return;
		}
		if (!menu)
			npcCombat(i, dt);
	});
	if (!menu)
	{
		updateProjectiles(dt);
		npcSummonsUpdate();
	}
}

// OpenMW's applyOnStrikeEnchantment: on every blow that lands, before the block and the damage (so Soul Trap works on
// a killing blow, and a blocked blow still casts); an arrow, bolt or thrown weapon casts its own (no charge kept on it)
void Session::strikeEnchantment(int target, const Object* missile)
{
	if (missile)
	{
		auto sp = missile->ench.empty() ? w.game.spells.end() : w.game.spells.find(missile->ench);
		if (sp == w.game.spells.end() || sp->second.type != ENCH_STRIKE)
			return;
		for (auto& e : sp->second.effects)
			if (e.range == 0)
				applyEffectToPlayer(e, missile->name);
			else
				applyEffectToActor(target, e, true, 0.0f, 100.0f, missile->name);
		strikeEffects(sp->second, target);
		logf("combat: %s strikes with %s", missile->id.c_str(), sp->second.id.c_str());
		return;
	}
	InventoryItem* wit = playerWeaponItem();
	const SpellDef* en = wit ? w.enchantmentOf(*wit) : nullptr;
	if (!en || en->type != ENCH_STRIKE)
		return;
	float charge = w.chargeOf(*wit);
	const float cost = enchantCastCost(en->cost);
	if (charge < cost)
		return;
	wit->charge = charge - cost;
	useSkill(9, 3);                       // Enchant: an enchantment cast on a strike (SKDT use 3)
	const Object* o = w.game.object(wit->id);
	for (auto& e : en->effects)
		if (e.range == 0)
			applyEffectToPlayer(e, o ? o->name.str() : en->id);
		else
			applyEffectToActor(target, e, true, cost, 100.0f, o ? o->name.str() : en->id);
	strikeEffects(*en, target);
	logf("combat: %s strikes with %s (charge %.0f)", wit->id.c_str(), en->id.c_str(), wit->charge);
}

// An on-strike enchantment that lands shows and sounds as a spell's hit on whoever it struck (-1 the player): OpenMW
// casts it (CastSpell::inflict, playEffects)
void Session::strikeEffects(const SpellDef& en, int target)
{
	const bool player = target < 0 || target >= (int)w.refs.size();
	spellVfx(en, 1, player ? w.player.feet : w.refs[target].pos, player ? -1 : target, player ? w.player.yaw : w.refs[target].rot[2]);
	playSound(player ? -1 : target, spellSound(en, 2));
}

// An NPC's or creature's enchanted weapon (or arrow) on a blow at the player: its effects land on the player (Reflect
// can send them back), its self effects on the attacker. NPCs' items keep no charge here: it always casts
void Session::npcStrikeEnchantment(int ri, const Object* item)
{
	if (!item || item->ench.empty())
		return;
	auto sp = w.game.spells.find(item->ench);
	if (sp == w.game.spells.end() || sp->second.type != ENCH_STRIKE)
		return;
	for (auto& e : sp->second.effects)
		if (e.range == 0)
			applyEffectToActor(ri, e, false, 0.0f, 100.0f, item->name);
		else
			applyEffectToPlayer(e, item->name, ri);
	strikeEffects(sp->second, -1);
	logf("combat: %s's %s strikes the player with %s", w.refs[ri].id.c_str(), item->id.c_str(), sp->second.id.c_str());
}
