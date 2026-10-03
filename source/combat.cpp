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
//   fleeing     below (10 + flee / 2.5) % health an actor runs for a while, then comes back
//   prices      base value x (buying: 1 - (pc - npc) / 200, selling: 0.5 - (npc - pc) / 200), where
//               pc = (disposition - 50 + mercantile + luck / 10 + personality / 5) x fatigue term, npc alike
// Crime: assault and murder add a bounty (killing whoever attacked first, or creatures, is free);
// guards come to arrest the player.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "audio.h"
#include "log.h"
#include "session.h"
#include "dialogue.h"

enum { SKILL_BLOCK = 0, SKILL_MEDIUM_ARMOR = 2, SKILL_HEAVY_ARMOR = 3, SKILL_BLUNT = 4, SKILL_LONG_BLADE = 5,
       SKILL_AXE = 6, SKILL_SPEAR = 7, SKILL_SECURITY = 18, SKILL_SNEAK = 19, SKILL_LIGHT_ARMOR = 21, SKILL_SHORT_BLADE = 22,
       SKILL_MARKSMAN = 23, SKILL_MERCANTILE = 24, SKILL_HAND_TO_HAND = 26, SKILL_UNARMORED = 17 };

static const float kCombatDistance = 128.0f;      // fCombatDistance: reach 1.0 weapons
static const float kTurnSpeed = 6.0f;              // radians per second
static const float kHostileGiveUp = 5000.0f;
static const float kArmorSlotWeight[11] = { 0.1f, 0.3f, 0.1f, 0.1f, 0.1f, 0.1f, 0.05f, 0.05f, 0.1f, 0.05f, 0.05f };
// Base weights per armor slot (iHelmWeight ...): lighter than 60% light armor, than 90% medium
static const float kArmorSlotBaseWeight[11] = { 5, 30, 10, 10, 15, 20, 5, 5, 15, 5, 5 };
static const float kArcherMinRange = 350.0f, kArcherMaxRange = 1600.0f;

static float fatigueTerm(float fatigue, float fatigueMax)
{
	return 1.25f - 0.5f * (1.0f - (fatigueMax > 0.0f ? fmaxf(0.0f, fatigue) / fatigueMax : 1.0f));
}

static float frand() { return (rand() % 1000) / 1000.0f; }

static bool isRanged(const Object* w) { return w && w->subtype >= WEAP_BOW && w->subtype <= WEAP_THROWN; }

static int weaponSkill(const Object* w)
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

static const char* attackGroup(const Object* w)
{
	if (!w)
		return "AttackHH";
	switch (w->subtype)
	{
	case 2: case 4: case 8: return "Attack2c";
	case 5: case 6: return "Attack2w";
	case WEAP_BOW: return "AttackBow";
	case WEAP_CROSSBOW: return "AttackXbow";
	case WEAP_THROWN: return "AttackThrow";
	default: return "Attack1h";
	}
}

// A creature strikes with its own Attack1; one that has none (biped creatures use the NPC groups) with the
// group for what it holds, or hand to hand
static const char* creatureAttackGroup(const ActorSet& set, const Actor& a, const Object* w)
{
	return actorFindGroup(actorSkeleton(set, a.skeleton), "Attack1") >= 0 ? "Attack1" : attackGroup(w);
}

// First-person animation family of a weapon (tools/convert/firstperson.py group names)
static const char* vmGroup(const Object* w)
{
	if (!w)
		return "HH";
	switch (w->subtype)
	{
	case 2: case 4: case 8: return "2c";
	case 5: case 6: return "2w";
	case WEAP_BOW: return "Bow";
	case WEAP_CROSSBOW: return "Xbow";
	case WEAP_THROWN: return "Throw";
	default: return "1h";
	}
}

static float weaponReach(const Object* w)
{
	return (w && !isRanged(w) ? w->reach : 0.7f) * kCombatDistance + 30.0f;
}

// Damage of one blow: attack kind from how the player moves (thrust forward, slash sideways, chop)
static float weaponDamage(const Object* w, int kind, float charge, int strength)
{
	const u8* r = kind == 1 ? w->slash : kind == 2 ? w->thrust : w->chop;
	float dmg = r[0] + (r[1] - r[0]) * charge;
	return dmg * (0.5f + 0.01f * strength);
}

// An NPC's attack type, the more likely the more it does on average (OpenMW's chooseBestAttack): 0 chop, 1 slash, 2 thrust
static int npcAttackKind(const Object* w)
{
	int slash = (w->slash[0] + w->slash[1]) / 2, chop = (w->chop[0] + w->chop[1]) / 2, thrust = (w->thrust[0] + w->thrust[1]) / 2;
	float roll = frand() * (slash + chop + thrust);
	return roll <= slash ? 1 : roll <= slash + thrust ? 2 : 0;
}

static float applyArmor(float dmg, float armor)
{
	if (armor <= 0.0f || dmg <= 0.0f)
		return dmg;
	return dmg * fmaxf(0.25f, dmg / (dmg + armor));
}

static float angleDiff(float a, float b)
{
	float d = fmodf(a - b + 3.14159265f, 6.2831853f);
	if (d < 0.0f)
		d += 6.2831853f;
	return d - 3.14159265f;
}

// ---- Equipment

InventoryItem* Session::playerWeaponItem()
{
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "WEAP" && o->subtype < WEAP_ARROW)
					return &it;
	return nullptr;
}

const Object* Session::playerWeapon()
{
	InventoryItem* it = playerWeaponItem();
	return it ? w.game.object(it->id) : nullptr;
}

const Object* Session::playerAmmo()
{
	const Object* wpn = playerWeapon();
	if (!wpn || !isRanged(wpn))
		return nullptr;
	if (wpn->subtype == WEAP_THROWN)
		return wpn;
	int want = wpn->subtype == WEAP_BOW ? WEAP_ARROW : WEAP_BOLT;
	for (auto& it : w.inventory)
		if (it.equipped && it.count > 0)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "WEAP" && o->subtype == want)
					return o;
	return nullptr;
}

int Session::itemCondition(const InventoryItem& it)
{
	const Object* o = w.game.object(it.id);
	int max = o ? o->health : 0;
	return it.condition < 0 ? max : it.condition;
}

// Wears an equipped weapon / armor piece down; one of a stack splits off first
void Session::wearItem(InventoryItem* it, float amount)
{
	const Object* o = it ? w.game.object(it->id) : nullptr;
	if (!o || o->health <= 0)
		return;
	int cond = itemCondition(*it);
	if (cond <= 0)
		return;
	int loss = (int)fmaxf(1.0f, amount);
	if (it->count > 1)
	{
		InventoryItem one = *it;
		one.count = 1;
		it->count--;
		it->equipped = false;
		w.inventory.push_back(one);
		it = &w.inventory.back();
	}
	it->condition = cond - loss < 0 ? 0 : cond - loss;
	if (it->condition == 0)
	{
		notify(o->name + (o->type == "WEAP" ? " is broken." : " is broken and gives no protection."));
		logf("combat: %s broke", o->id.c_str());
		if (o->type == "WEAP")
			it->equipped = false;
	}
}

// The armor rating (OpenMW's getArmorRating): each of the nine slots counts what is worn there, its armor
// times the wearer's skill in that weight class over iBaseArmorSkill, times its condition; a slot with no
// armor counts the Unarmored rating (fUnarmoredBase1 x skill) x (fUnarmoredBase2 x skill). Slots weigh
// cuirass 0.3, the shield hand, helmet, greaves, boots and pauldrons 0.1, each hand 0.05; Shield adds.
float Session::playerArmor()
{
	// ARMO subtype -> slot (a bracer takes its hand's slot)
	static const int kSlot[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7 };
	static const float kWeight[9] = { 0.1f, 0.3f, 0.1f, 0.1f, 0.1f, 0.1f, 0.05f, 0.05f, 0.1f };
	const PlayerStats& s = w.stats;
	float unarmored = (w.game.gmstf("funarmoredbase1", 0.1f) * s.skills[SKILL_UNARMORED])
		* (w.game.gmstf("funarmoredbase2", 0.065f) * s.skills[SKILL_UNARMORED]);
	float slotRating[9];
	bool worn[9] = {};
	for (int k = 0; k < 9; k++)
		slotRating[k] = unarmored;
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "ARMO" && o->subtype >= 0 && o->subtype < 11)
				{
					float wt = kArmorSlotBaseWeight[o->subtype];
					int skill = o->weight <= wt * w.game.gmstf("flightmaxmod", 0.6f) + 0.0005f ? SKILL_LIGHT_ARMOR
						: o->weight <= wt * w.game.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? SKILL_MEDIUM_ARMOR : SKILL_HEAVY_ARMOR;
					float r = o->weight == 0.0f ? (float)o->armor
						: o->armor * (float)s.skills[skill] / w.game.gmstf("ibasearmorskill", 30.0f);
					float cond = o->health > 0 ? (float)itemCondition(it) / o->health : 1.0f;
					int slot = kSlot[o->subtype];
					slotRating[slot] = r * cond;
					worn[slot] = true;
				}
	float rating = 0.0f;
	for (int k = 0; k < 9; k++)
		rating += kWeight[k] * slotRating[k];
	return rating + w.effectTotal(3);         // Shield
}

// ---- Shared rules

bool Session::shieldBlocks(float blockSkill, float agility, float luck, float fatigue, float fatigueMax, bool still,
	float attackSkill, float attackAgility, float attackLuck, float attackFatigue, float attackFatigueMax, float charge)
{
	float x = blockChance(blockSkill, agility, luck, fatigue, fatigueMax, still, attackSkill, attackAgility, attackLuck,
		attackFatigue, attackFatigueMax, charge);                     // (formulas.cpp)
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

// ---- NPCs

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
			if (i != ri && o.type == "NPC_" && !o.dead && !o.ally && o.knockTimer <= 0.0f && w.active(i)
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
	r.knockTimer = 0.0f;
	if (r.gold > 0)
		r.contents.emplace_back(r.gold, "gold_001");
	r.gold = 0;
	if (Actor* a = w.actorOf(ri))
	{
		a->showWeapon = false;
		if (!actorPlay(*w.actorsOf(ri), *a, "Death1", ANIM_HOLD))
			logf("combat: %s has no death animation", r.id.c_str());
	}
	w.syncActor(ri);
	playSound(ri, "Body Fall Medium");
	logf("combat: %s died", r.id.c_str());
}

// fatigueOnly: hand-to-hand on someone standing, which only tires (OpenMW: fists hurt fatigue or, on someone down or
// paralyzed, health; never both)
void Session::damageNpc(int ri, float dmg, bool fatigueOnly)
{
	Ref& r = w.refs[ri];
	if (fatigueOnly)
	{
		r.fatigue -= dmg;
		playSound(ri, "Hand To Hand Hit");
	}
	else
	{
		r.health -= dmg;
		playSound(ri, "Health Damage");
		// the blood splat (Morrowind.ini [Blood]: red, a skeleton's white, a dwemer machine's gold sparks)
		int blood = r.actor >= 0 ? w.game.actors[r.actor].blood : 0;
		float at[3] = { r.pos[0], r.pos[1], r.pos[2] + 80.0f };
		spawnVfx("blood" + std::to_string(blood >= 0 && blood < 3 ? blood : 0), at, 0.5f);
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
		actorPlay(*w.actorsOf(ri), *w.actorOf(ri), "Hit1", ANIM_ONCE);
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
	// (the save is read here once: reading it every frame of the screen cost 70 ms a frame)
	deathHaveSave = savePath && !World::savedCell(savePath).empty();
	openScreen(SCR_DEATH);
}

// A blow (or arrow) from an actor reaches the player: block, armor, wear, knockdown
void Session::npcHitsPlayer(int ri, float dmg, bool fatigueOnly, int skill, bool unarmed, bool ranged)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	PlayerStats& s = w.stats;
	// Diseased creatures pass their disease on with a blow, not with a shot (formulas.cpp catchDiseases; OpenMW's
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
		if (it.equipped && w.game.object(it.id) && w.game.object(it.id)->type == "ARMO"
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
			+ (aw && aw->type == "WEAP" ? aw->weight * 0.5f * w.game.gmstf("fweaponfatigueblockmult", 0.0f) : 0.0f);
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
					if (o->type == "ARMO" && (o->subtype == slot || (slot == 6 && o->subtype == 9) || (slot == 7 && o->subtype == 10)))
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

void Session::addIndicator(const std::string& text, u32 rgb)
{
	if (!hitIndicators)
		return;
	// a random spot within 10% x 5% of the screen around the middle (the mod's OFFSET_RANGE)
	float ox = ((rand() % 1000) / 1000.0f - 0.5f) * 0.1f * 400.0f;
	float oy = ((rand() % 1000) / 1000.0f - 0.5f) * 0.05f * 240.0f;
	indicators.push_back({ text, rgb, 200.0f + ox, 110.0f + oy, 1.0f });
}

// Drawn with the HUD (the crosshair's depth); 0.1 of the screen's height a second upward, fading out
void Session::drawIndicators(float dt)
{
	for (size_t i = 0; i < indicators.size();)
	{
		Indicator& d = indicators[i];
		float alpha = fmaxf(0.0f, fminf(1.0f, d.timer));
		u32 col = C2D_Color32((d.rgb >> 16) & 255, (d.rgb >> 8) & 255, d.rgb & 255, (u8)(alpha * 255.0f));
		uiTextCentered(d.x + aimShift, d.y, 0.55f, col, d.text);
		d.y -= 0.1f * 240.0f * dt;
		d.timer -= dt;
		if (d.timer <= 0.0f)
			indicators.erase(indicators.begin() + i);
		else
			i++;
	}
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
	if (wpn && (wpn->type != "WEAP" || isRanged(wpn)))
		wpn = nullptr;
	float charge = frand();                    // the swing's strength (OpenMW's AI: uniform 0..1)
	// The swing tires them: fFatigueAttackBase + fWeaponFatigueMult x weapon weight x swing (OpenMW's applyFatigueLoss)
	r.fatigue -= w.game.gmstf("ffatigueattackbase", 2.0f) + (wpn ? wpn->weight : 0.0f) * charge * w.game.gmstf("fweaponfatiguemult", 0.25f);
	// OpenMW's getHitChance: the attack term less the player's evasion (none while down or paralyzed), rounded
	bool helpless = w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f;
	float chance = roundf(attackTermOf(def.skills[weaponSkill(wpn)], def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK],
		r.fatigue, r.fatigueMax, w.actorEffect(ri, 117), w.actorEffect(ri, 47)) - playerDefense(helpless));
	BlowRoll roll = rollBlow(chance, false, false);
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
	if (def.creature && !def.attack.empty())
	{
		// Creatures: one of their attacks, between its minimum and maximum
		const auto& at = def.attack[rand() % def.attack.size()];
		npcHitsPlayer(ri, (at.first + (at.second - at.first) * charge) * roll.damage, false, weaponSkill(nullptr), true);
	}
	else if (wpn)
		npcHitsPlayer(ri, weaponDamage(wpn, npcAttackKind(wpn), charge, def.attributes[ATTR_STRENGTH]) * roll.damage, false,
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

// Walks toward `target`: straight when nothing is in the way, else along the path grid; steps
// sideways when stuck. faceMove turns the actor along its way. False when it can't make progress.
bool Session::npcMoveTo(int ri, const float target[3], float speed, float dt, bool faceMove)
{
	Ref& r = w.refs[ri];
	Cell* cell = w.cellOf(ri);
	if (!cell)
		return false;
	r.repathTimer -= dt;
	if (r.repathTimer <= 0.0f)
	{
		r.repathTimer = 0.8f + frand() * 0.4f;
		float a[3] = { r.pos[0], r.pos[1], r.pos[2] + 50.0f }, b[3] = { target[0], target[1], target[2] + 50.0f };
		// straight when nothing is in the way, unless an edge stopped the straight way lately
		if (w.time >= r.gridUntil && w.lineOfSight(a, b))
			r.path.clear();
		else if (!w.findPath(r.pos, target, r.path))
			r.path.clear();
	}
	float goal[3] = { target[0], target[1], target[2] };
	while (!r.path.empty())
	{
		const PathPoint& p = w.pathPoints[r.path.front()];
		float dx = p.pos[0] - r.pos[0], dy = p.pos[1] - r.pos[1];
		// a waypoint counts as reached close to it: from 50 away the guard on the prison ship cut the
		// corner round the stairwell opening and walked over its edge
		if (dx * dx + dy * dy > 20.0f * 20.0f)
		{
			memcpy(goal, p.pos, sizeof(goal));
			break;
		}
		r.path.erase(r.path.begin());
	}
	float dx = goal[0] - r.pos[0], dy = goal[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist < 1.0f)
		return true;
	dx /= dist;
	dy /= dist;
	// Stuck against something: slide sideways for a moment
	if (r.stuckTimer < 0.0f)
	{
		r.stuckTimer += dt;
		float sx = -dy, sy = dx;
		if (((int)(r.pos[0] + r.pos[1]) & 1) != 0) { sx = -sx; sy = -sy; }
		dx = (dx + sx * 1.5f) * 0.4f;
		dy = (dy + sy * 1.5f) * 0.4f;
	}
	if (faceMove)
	{
		float diff = angleDiff(atan2f(dx, dy), r.rot[2]);
		r.rot[2] += fmaxf(-kTurnSpeed * dt, fminf(kTurnSpeed * dt, diff));
	}
	float before[2] = { r.pos[0], r.pos[1] };
	if (!npcStep(ri, *cell, dx, dy, fminf(speed * dt, dist)) && r.path.empty() && w.time >= r.gridUntil)
	{
		// the straight way runs over an edge (a stairwell opening): the path grid goes round it
		r.gridUntil = w.time + 15.0f;
		r.repathTimer = 0.0f;
	}
	float moved = sqrtf((r.pos[0] - before[0]) * (r.pos[0] - before[0]) + (r.pos[1] - before[1]) * (r.pos[1] - before[1]));
	if (r.stuckTimer >= 0.0f)
	{
		r.stuckTimer = moved < speed * dt * 0.25f ? r.stuckTimer + dt : 0.0f;
		if (r.stuckTimer > 0.5f)
		{
			r.stuckTimer = -0.7f;
			r.repathTimer = 0.0f;
		}
	}
	return moved > 0.0f;
}

// A spell from the actor's list: heal itself when hurt, else a harmful one at the player
// (target spells from afar, touch spells up close). Starts the cast; npcCombat releases it.
void Session::npcCastSpell(int ri)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	float dist = w.distanceToPlayer(ri);
	float reach = weaponReach(nullptr) + 40.0f;
	int pick = -1, range = -1;
	for (size_t i = 0; i < def.combatSpells.size(); i++)
	{
		auto it = w.game.spells.find(def.combatSpells[i]);
		if (it == w.game.spells.end() || (it->second.type != 5 && it->second.cost > r.magicka))
			continue;
		const SpellDef& sp = it->second;
		for (auto& e : sp.effects)
		{
			if (e.effect == 75 && e.range == 0 && r.health < r.healthMax * 0.5f)
			{
				pick = i;
				range = 0;
			}
			else if (e.range == 2 && dist > reach && range != 0)
			{
				pick = i;
				range = 2;
			}
			else if (e.range == 1 && dist <= reach && range != 0)
			{
				pick = i;
				range = 1;
			}
		}
		if (range == 0)
			break;
	}
	if (pick < 0 || (range == 2 && rand() % 3 == 0))
		return;
	const SpellDef& sp = w.game.spells[def.combatSpells[pick]];
	if (sp.type != 5)
		r.magicka -= sp.cost;
	r.castSpell = pick;
	r.shooting = false;
	float windup = 0.6f;
	if (Actor* a = w.actorOf(ri))
	{
		ActorSet& set = *w.actorsOf(ri);
		a->showWeapon = false;
		if (actorPlay(set, *a, range == 0 ? "CastSelf" : range == 1 ? "CastTouch" : "CastTarget", ANIM_ONCE))
		{
			const AnimGroup& g = actorSkeleton(set, a->skeleton).groups[a->group];
			windup = g.loopStart - g.start;
		}
	}
	r.hitAt = w.time + windup;
	r.attackTimer = windup + 1.0f;
	static const char* school[6] = { "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration" };
	playSound(ri, std::string(school[spellSchool(sp)]) + " cast");
	logf("combat: %s casts %s", r.id.c_str(), sp.name.c_str());
}

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

void Session::npcCombat(int ri, float dt)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = w.game.object(def.weapon);
	float dx = w.player.feet[0] - r.pos[0], dy = w.player.feet[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist > kHostileGiveUp || playerDead)
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

	// Knocked down: helpless until they get up
	if (r.knockTimer > 0.0f)
	{
		r.knockTimer -= dt;
		if (r.knockTimer <= 0.0f && a)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		return;
	}

	// Fleeing: badly hurt actors run for a while, then come back
	// OpenMW's vanillaRateFlee: Flee 100 and up always flees; else (1 - health share) x fAIFleeHealthMult + Flee x
	// fAIFleeFleeMult, plus the distance bias (iFightDistanceBase - fFightDistanceMultiplier x distance) when not 0;
	// it flees at 100 or more (the action choice is made again every second or so)
	const float fleeRating = fleeRatingOf(ri, dist);
	if (!r.fleeing && fleeRating >= 100.0f)
	{
		r.fleeing = true;
		r.fleeTimer = 1.0f + frand();
		npcSayTopic(ri, "Flee");
		r.hitAt = -1.0f;
		r.castSpell = -1;
		logf("combat: %s flees", r.id.c_str());
	}
	if (r.fleeing)
	{
		r.fleeTimer -= dt;
		if (r.fleeTimer <= 0.0f)
		{
			// the choice again: still rated 100, keep running
			if (fleeRating >= 100.0f)
				r.fleeTimer = 1.0f + frand();
			else
				r.fleeing = false;
		}
		else
		{
			float away[3] = { r.pos[0] - dx / fmaxf(dist, 1.0f) * 600.0f, r.pos[1] - dy / fmaxf(dist, 1.0f) * 600.0f, r.pos[2] };
			if (dist < w.game.gmstf("ffleedistance", 3000.0f))
			{
				npcMoveTo(ri, away, actorRunSpeed(ri), dt, true);
				if (a && !actorPlay(*set, *a, "RunForward", ANIM_LOOP))
					actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
			}
			else if (a && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
			w.syncActor(ri);
			return;
		}
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
	float reach = def.creature ? kCombatDistance * 0.9f : weaponReach(wpn);
	bool attacking = r.hitAt >= 0.0f;
	float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
	float eyeB[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };

	// Spells now and then
	r.castTimer -= dt;
	if (!attacking && r.castTimer <= 0.0f && !def.combatSpells.empty())
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
	bool level = ranged || fabsf(w.player.feet[2] - r.pos[2]) < 180.0f;
	if ((dist > keep || !see || !level) && !attacking)
	{
		// Chase: straight at the player, or along the path grid around walls. Can't get any closer (a
		// ledge, a gap the grid doesn't cover): stand rather than run in place, and run again once it moves.
		npcMoveTo(ri, w.player.feet, actorRunSpeed(ri), dt, false);
		// progress: how far it really got each second (sliding to and fro against a door or a wall, or the
		// player walking up to it, doesn't count)
		r.chaseWindow += dt;
		if (r.chaseWindow >= 1.0f)
		{
			float mx = r.pos[0] - r.chaseAnchor[0], my = r.pos[1] - r.chaseAnchor[1];
			r.chaseStill = mx * mx + my * my < 60.0f * 60.0f ? r.chaseStill + r.chaseWindow : 0.0f;
			r.chaseAnchor[0] = r.pos[0];
			r.chaseAnchor[1] = r.pos[1];
			r.chaseWindow = 0.0f;
		}
		if (r.chaseStill >= 2.0f)
		{
			if (a && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		else if (a && !actorPlay(*set, *a, "RunForward", ANIM_LOOP))
			actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
	}
	else
	{
		r.chaseStill = 0.0f;
		r.chaseWindow = 0.0f;
		r.chaseAnchor[0] = r.pos[0];
		r.chaseAnchor[1] = r.pos[1];
		if (a && a->mode == ANIM_LOOP)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		r.attackTimer -= dt;
		if (!attacking && r.attackTimer <= 0.0f && fabsf(diff) < 0.6f && (!ranged || dist > kArcherMinRange * 0.5f))
		{
			float windup = 0.35f, length = 1.0f;
			if (a)
				a->showWeapon = wpn != nullptr;
			if (!def.creature && rand() % 100 < (int)w.game.gmstf("ivoiceattackodds", 10.0f))
				npcSayTopic(ri, "Attack");
			if (a && actorPlay(*set, *a, def.creature ? creatureAttackGroup(*set, *a, wpn) : attackGroup(wpn), ANIM_ONCE))
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
		if (r.castSpell >= 0)
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
					else if (e.range == 1 && dist <= reach + 60.0f)
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
		else if (dist <= reach + 40.0f && fabsf(angleDiff(want, r.rot[2])) < 1.0f)
			npcStrike(ri);
	}
	w.syncActor(ri);
}

static const float kNpcSwimDepth = 95.0f;          // as the player's: feet below the surface

// Moves an NPC `step` units along (dirX, dirY), following the floor and stopped by walls;
// false when blocked (no floor there)
bool Session::npcStep(int ri, Cell& cell, float dirX, float dirY, float step)
{
	Ref& r = w.refs[ri];
	float body[3] = { r.pos[0] + dirX * step, r.pos[1] + dirY * step, r.pos[2] + 70.0f };
	collisionPushSphere(cell.collision, body, 22.0f, true);   // about a person's half width (Morrowind's boxes: 20..29)
	float fz;
	// people swim in deep water, the head out (creatures keep to their depth)
	float swimZ = cell.hasWater() && r.type == "NPC_" ? cell.waterZ - kNpcSwimDepth : -1e9f;
	// the floor under the feet, not under one point: the highest of the middle and four spots around it
	// (as the player's), so a crack or a hatch's corner under the middle isn't a drop
	auto footFloor = [&](float* out) {
		static const float offs[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
		bool found = false;
		for (auto& o : offs)
		{
			float z;
			if (collisionFloor(cell.collision, body[0] + o[0] * 15.0f, body[1] + o[1] * 15.0f, r.pos[2] + 40.0f,
					r.pos[2] - 300.0f, &z) && (!found || z > *out))
			{
				*out = z;
				found = true;
			}
		}
		return found;
	};
	if (!footFloor(&fz))
	{
		if (swimZ < -1e8f || r.pos[2] > swimZ + 1.0f)
			return false;
		fz = swimZ;
	}
	fz = fmaxf(fz, swimZ);
	// A step goes down stairs and slopes, not over an edge: dropping more than a stair's height in one
	// step (the prison ship's guard walked into the stairwell opening and ended up inside the stairs)
	if (fz < r.pos[2] - 48.0f && fz > swimZ + 0.5f)
		return false;
	if (Actor* a = w.actorOf(ri))
		a->swimming = fz <= swimZ + 0.5f;
	float moved[3] = { body[0] - r.pos[0], body[1] - r.pos[1], fz - r.pos[2] };
	r.pos[0] = body[0];
	r.pos[1] = body[1];
	r.pos[2] = fz;
	r.fitBox();
	r.moved = true;
	return moved[0] * moved[0] + moved[1] * moved[1] > step * step * 0.1f;
}

// Peaceful NPCs: face the player while talking to them; otherwise stroll to random spots within
// their wander distance of home, pausing between walks
// Talking to the player (or saying a line): stand, face them, no walk cycle (whatever the NPC was
// doing: wandering, a travel / follow / escort package). True when that's what they do this frame.
bool Session::npcTalkStand(int ri, float dt)
{
	Ref& r = w.refs[ri];
	if (!((dlg.open && dlg.ref == ri) || !sayDone(ri)))
		return false;
	float want = atan2f(w.player.feet[0] - r.pos[0], w.player.feet[1] - r.pos[1]);
	float diff = angleDiff(want, r.rot[2]);
	r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
	r.moved = true;
	Actor* a = w.actorOf(ri);
	if (a && (r.wandering || a->mode == ANIM_LOOP))
	{
		logf("ai: %s stops to talk", r.id.c_str());
		actorPlay(*w.actorsOf(ri), *a, "Idle", ANIM_IDLE);
	}
	r.wandering = false;
	r.wanderTimer = fmaxf(r.wanderTimer, 4.0f);
	w.syncActor(ri);
	return true;
}

void Session::npcWander(int ri, float dt)
{
	Ref& r = w.refs[ri];
	const ActorDef& def = w.game.actors[r.actor];
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	Cell* cell = w.cellOf(ri);
	if (!cell)
		return;
	(void)a;
	(void)set;
	int range = r.aiPackage == AIPKG_WANDER && r.aiRange >= 0 ? r.aiRange : def.wander;
	// a walk / run cycle left over from a fight, a flight or a finished package (combat ends when the
	// player goes away, StopCombat, ...): standing, not going anywhere
	if (!r.wandering && a && a->mode == ANIM_LOOP)
		actorPlay(*set, *a, "Idle", ANIM_IDLE);
	if (range <= 0)
		return;
	if (!r.wandering)
	{
		r.wanderTimer -= dt;
		if (r.wanderTimer > 0.0f)
			return;
		// Somewhere to stroll to: a path grid point (Morrowind's walkable spots) within their wander
		// distance of home that they can walk to, straight or along the grid (not one across a wall
		// they would only walk into); else a spot in the open at least a few steps away
		std::vector<int> spots;
		for (size_t i = 0; i < w.pathPoints.size(); i++)
		{
			const PathPoint& pp = w.pathPoints[i];
			if (pp.cell < 0 || !w.cells[pp.cell].live)
				continue;
			float hx = pp.pos[0] - r.home[0], hy = pp.pos[1] - r.home[1];
			float px = pp.pos[0] - r.pos[0], py = pp.pos[1] - r.pos[1];
			if (hx * hx + hy * hy <= (float)range * range && px * px + py * py > 150.0f * 150.0f
				&& fabsf(pp.pos[2] - r.pos[2]) < 200.0f)
				spots.push_back(i);
		}
		r.path.clear();
		bool picked = false, anySpot = !spots.empty();
		for (int tries = 0; tries < 4 && !spots.empty() && !picked; tries++)
		{
			int k = rand() % spots.size();
			const PathPoint& pp = w.pathPoints[spots[k]];
			spots.erase(spots.begin() + k);
			float from[3] = { r.pos[0], r.pos[1], r.pos[2] + 50.0f }, to[3] = { pp.pos[0], pp.pos[1], pp.pos[2] + 50.0f };
			if (!w.lineOfSight(from, to) && !w.findPath(r.pos, pp.pos, r.path))
				continue;
			r.wanderTo[0] = pp.pos[0];
			r.wanderTo[1] = pp.pos[1];
			picked = true;
		}
		if (!picked && !anySpot)
		{
			float ang = frand() * 6.2831853f, d = fmaxf(150.0f, frand() * range);
			r.wanderTo[0] = r.home[0] + sinf(ang) * d;
			r.wanderTo[1] = r.home[1] + cosf(ang) * d;
			picked = true;
		}
		if (!picked)
		{
			// nowhere reachable just now: look again in a while
			r.wanderTimer = 5.0f + frand() * 8.0f;
			return;
		}
		r.wandering = true;
		// give up on a walk after a while: the way's length at walking pace, and some
		float way = 0.0f, at[2] = { r.pos[0], r.pos[1] };
		for (int pi : r.path)
		{
			way += hypotf(w.pathPoints[pi].pos[0] - at[0], w.pathPoints[pi].pos[1] - at[1]);
			at[0] = w.pathPoints[pi].pos[0];
			at[1] = w.pathPoints[pi].pos[1];
		}
		way += hypotf(r.wanderTo[0] - at[0], r.wanderTo[1] - at[1]);
		r.wanderTimer = 8.0f + way / 70.0f;
		r.wanderCheck = 1.0f;
		r.wanderFrom[0] = r.pos[0];
		r.wanderFrom[1] = r.pos[1];
		if (a)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);    // turn first, then walk
		if (a)
			actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
	}
	// The next grid point on the way (passed ones dropped), else the spot itself
	float goal[2] = { r.wanderTo[0], r.wanderTo[1] };
	while (!r.path.empty())
	{
		const PathPoint& p = w.pathPoints[r.path.front()];
		float px = p.pos[0] - r.pos[0], py = p.pos[1] - r.pos[1];
		if (px * px + py * py > 50.0f * 50.0f)
		{
			goal[0] = p.pos[0];
			goal[1] = p.pos[1];
			break;
		}
		r.path.erase(r.path.begin());
	}
	float dx = goal[0] - r.pos[0], dy = goal[1] - r.pos[1];
	float dist = sqrtf(dx * dx + dy * dy);
	r.wanderTimer -= dt;
	bool arrived = (r.path.empty() && dist < 60.0f) || r.wanderTimer <= 0.0f;
	if (!arrived)
	{
		float want = atan2f(dx, dy);
		float diff = angleDiff(want, r.rot[2]);
		r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
		// Turn in place until facing the spot, then walk (fWalkSpeed-ish pace), still turning a
		// little on the way. Walking while turning hard is what made them circle spots beside them.
		if (fabsf(diff) < 0.35f)
		{
			if (a && a->mode != ANIM_LOOP)
				actorPlay(*set, *a, "WalkForward", ANIM_LOOP);
			if (!npcStep(ri, *cell, dx / dist, dy / dist, fminf(110.0f * dt, dist)) && r.path.empty())
				arrived = true;
			// Stuck against something: covered under a third of a second's walk in a second
			r.wanderCheck -= dt;
			if (r.wanderCheck <= 0.0f)
			{
				float mx = r.pos[0] - r.wanderFrom[0], my = r.pos[1] - r.wanderFrom[1];
				if (mx * mx + my * my < 40.0f * 40.0f)
					arrived = true;
				r.wanderCheck = 1.0f;
				r.wanderFrom[0] = r.pos[0];
				r.wanderFrom[1] = r.pos[1];
			}
		}
		else if (a && a->mode == ANIM_LOOP)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		r.moved = true;
	}
	if (arrived)
	{
		r.wandering = false;
		r.path.clear();
		r.wanderTimer = 5.0f + frand() * 12.0f;
		if (a)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
	}
	w.syncActor(ri);
}

// ---- Voices: what people say on their own (the voice topics' lines that fit them)

bool Session::npcSayTopic(int ri, const char* voiceTopic)
{
	const Ref& r = w.refs[ri];
	if (r.dead || r.type != "NPC_" || !sayDone(ri) || w.distanceToPlayer(ri) > 2000.0f)
		return false;
	const Topic* t = w.game.topic(voiceTopic);
	if (!t)
		return false;
	const Info* info = dialogueVoiced(w, *t, ri);
	if (!info || info->sound.empty())
		return false;
	say(ri, info->sound, info->text);
	return true;
}

// ---- Gravity for people and creatures

static const float kActorGravity = 627.0f;      // as the player's (player.cpp)

void Session::actorGravity(float dt)
{
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if ((r.type != "NPC_" && r.type != "CREA") || !r.visible() || r.actor < 0)
			continue;
		// Creatures fall like anyone, but not the ones that fly, nor the ones that swim while they're in water
		// (as in OpenMW, where every actor falls unless it flies or is held up by water)
		bool creature = r.type == "CREA";
		u8 afloat = w.game.actors[r.actor].afloat;
		if (creature && !r.dead && (afloat & 0x20))
			continue;
		if (creature && !r.dead && (afloat & 0x10) && w.underWater(r.pos[2]))
			continue;
		// The floor under them: every quarter second while standing, every frame while falling
		if (!r.falling)
		{
			r.floorCheck -= dt;
			if (r.floorCheck > 0.0f)
				continue;
			r.floorCheck = 0.25f + (i % 8) * 0.01f;
		}
		float best = -1e9f;
		for (LoadedCell* l : w.loaded)
		{
			float z;
			if (collisionFloor(l->cell.collision, r.pos[0], r.pos[1], r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z) && z > best)
				best = z;
		}
		// deep water holds people up (swimming); creatures that walk wade through it
		float swimZ = -1e9f;
		for (LoadedCell* l : w.loaded)
			if (l->cell.hasWater() && !creature)
				swimZ = fmaxf(swimZ, l->cell.waterZ - kNpcSwimDepth);
		bool swim = swimZ > -1e8f && swimZ > best;
		if (swim)
			best = swimZ;
		if (Actor* a = w.actorOf(i))
			a->swimming = swim && !r.dead && r.pos[2] <= swimZ + 40.0f;
		if (best < -1e8f)
		{
			r.falling = false;          // nothing at all below: the land isn't there, leave them be
			continue;
		}
		if (!r.falling)
		{
			if (best > r.pos[2] - 40.0f)
			{
				// standing; placed a little above the floor (Ganciele Douar), or a step down: onto it
				float dz = best - r.pos[2];
				if (dz < -0.5f && !r.dead)
				{
					r.pos[2] = best;
					r.fitBox();
					r.moved = true;
					w.syncActor(i);
				}
				continue;
			}
			r.falling = true;
			r.fallVz = 0.0f;
			r.fallTop = r.pos[2];
			if (r.pos[2] - best > 200.0f)
				logf("world: %s falls from %.0f (floor %.0f)", r.id.c_str(), r.pos[2], best);
		}
		r.fallVz = fmaxf(r.fallVz - kActorGravity * dt, -3000.0f);
		float z = r.pos[2] + r.fallVz * dt;
		bool landed = z <= best;
		if (landed)
			z = best;
		r.pos[2] = z;
		r.fitBox();
		r.moved = true;
		w.syncActor(i);
		if (!landed)
			continue;
		r.falling = false;
		// The player's fall damage, with their own Acrobatics
		float h = r.fallTop - z;
		if (r.dead || swim)
			continue;                   // water breaks a fall
		int acro = w.game.actors[r.actor].skills[20];
		float x = fallDamage(h, acro, 0.0f);
		if (x > 0.0f)
		{
			float ft = fatigueTermOf(r.fatigue, r.fatigueMax);
			logf("world: %s lands after %.0f units, %.1f damage", r.id.c_str(), h, x * (1.0f - 0.25f * ft));
			r.health -= x * (1.0f - 0.25f * ft);
			if (r.health <= 0.0f)
				killNpc(i, false);
			else if (x > acro * ft)
				knockDown(i, false);
		}
	}
}

// ---- Summoned allies

// The nearest actor fighting the player: walk up and strike it (creature attack damage) every second
// or so; with nobody to fight, follow the player
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
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	const Ref& f = w.refs[foe];
	if (best > 120.0f)
	{
		npcMoveTo(ri, f.pos, actorRunSpeed(ri), dt, true);
		r.moved = true;
		if (a && a->mode != ANIM_LOOP)
			actorPlay(*set, *a, "RunForward", ANIM_LOOP);
	}
	else
	{
		float diff = angleDiff(atan2f(f.pos[0] - r.pos[0], f.pos[1] - r.pos[1]), r.rot[2]);
		r.rot[2] += fmaxf(-kTurnSpeed * dt, fminf(kTurnSpeed * dt, diff));
		r.attackTimer -= dt;
		if (r.attackTimer <= 0.0f)
		{
			r.attackTimer = 1.2f;
			const ActorDef& def = w.game.actors[r.actor];
			float dmg = 5.0f;
			if (!def.attack.empty())
				dmg = def.attack[0].first + frand() * (def.attack[0].second - def.attack[0].first);
			if (a)
				actorPlay(*set, *a, creatureAttackGroup(*set, *a, nullptr), ANIM_ONCE);
			damageNpc(foe, dmg, false);
		}
	}
	w.syncActor(ri);
}

// ---- AI packages

// A package that ran its hours or got where it was going is over: GetAIPackageDone reports 1, and OpenMW drops it, so
// the actor stands with no package (GetCurrentAIPackage -1) unless the script asked for it to repeat. The package
// still counts as the current one until the next tick (aiSettle), so a script polling both sees the pair once
static void aiFinish(World& w, Ref& r)
{
	r.aiDone = true;
	if (r.aiRepeat)
		r.aiStart = w.gameHour;
}

// The tick after a package finished: it is gone, nothing runs in its place
static void aiSettle(Ref& r)
{
	if (!r.aiDone || r.aiRepeat || r.aiPackage <= AIPKG_NONE || r.aiPackage == AIPKG_IDLE)
		return;
	r.aiPackage = AIPKG_IDLE;
	r.aiTarget.clear();
	r.aiCell.clear();
	r.aiDest[0] = r.aiDest[1] = r.aiDest[2] = 0.0f;
	r.aiDuration = 0.0f;
	r.path.clear();
	r.wandering = false;
}

// Travel to a spot, follow someone, escort the player to a spot (waiting when they fall behind), or
// walk up to something and use it. Duration counts game hours; then GetAIPackageDone reports 1.
void Session::npcPackage(int ri, float dt)
{
	Ref& r = w.refs[ri];
	Actor* a = w.actorOf(ri);
	ActorSet* set = w.actorsOf(ri);
	aiSettle(r);
	if (r.aiDuration > 0.0f && r.aiActive && w.gameHour - r.aiStart >= r.aiDuration)
		aiFinish(w, r);
	// Where they're headed: the destination, or whoever they follow / escort / activate
	const float* who = nullptr;
	if (r.aiTarget == "player")
		who = w.player.feet;
	else if (!r.aiTarget.empty())
	{
		int t = w.findRef(r.aiTarget);
		if (t >= 0 && w.active(t))
			who = w.refs[t].pos;
	}
	bool anyDest = r.aiDest[0] != 0.0f || r.aiDest[1] != 0.0f || r.aiDest[2] != 0.0f;
	bool hasDest = anyDest;
	// The destination is in another cell: until there, they keep with whoever they follow / escort
	// (going through doors with the player)
	bool elsewhere = !r.aiCell.empty() && lower(w.cells[w.placeOf(ri)].name) != r.aiCell;
	if (elsewhere)
		hasDest = false;
	auto near = [&](const float* p, float d) {
		float dx = p[0] - r.pos[0], dy = p[1] - r.pos[1];
		return dx * dx + dy * dy < d * d;
	};
	const float* goal = nullptr;
	float speed = actorWalkSpeed(ri);   // walking pace (OpenMW: by their Speed)
	bool running = false;               // only a follower far behind runs (OpenMW's AiFollow; the rest walk)
	switch (r.aiPackage)
	{
	case AIPKG_TRAVEL:
	{
		// (OpenMW leaves a destination more than 7168 units away alone: no walking, never done)
		float gap[3] = { r.aiDest[0] - r.pos[0], r.aiDest[1] - r.pos[1], r.aiDest[2] - r.pos[2] };
		if (gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2] > 7168.0f * 7168.0f)
			break;
		if (near(r.aiDest, 64.0f))
			aiFinish(w, r);
		else
			goal = r.aiDest;
		break;
	}
	case AIPKG_FOLLOW:
	{
		// A scripted follower starts once it sees who it follows within follow distance + 384 (looked for twice a second)
		if (!r.aiActive)
		{
			if (who && near(who, 640.0f) && (int)(w.time * 2.0f) != (int)((w.time - dt) * 2.0f))
			{
				float eyeA[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f }, eyeB[3] = { who[0], who[1], who[2] + 100.0f };
				if (w.lineOfSight(eyeA, eyeB))
				{
					r.aiActive = true;
					r.aiStart = w.gameHour;
				}
			}
			break;
		}
		// Arrived: in an interior, the cell it was told; outdoors, none (OpenMW)
		const LevelCell& here = w.cells[w.placeOf(ri)];
		if (anyDest && near(r.aiDest, 256.0f) && (here.interior ? lower(here.name) == r.aiCell : r.aiCell.empty()))
		{
			aiFinish(w, r);
			if (!r.aiRepeat)
				break;
		}
		if (who && !near(who, 256.0f))
		{
			goal = who;
			// running to catch up: from 450 units away, until within 325 (a dead zone, as in OpenMW, so they don't flip)
			bool wasRunning = a && a->mode == ANIM_LOOP && a->group == actorFindGroup(actorSkeleton(*set, a->skeleton), "RunForward");
			if (!near(who, wasRunning ? 325.0f : 450.0f))
			{
				speed = actorRunSpeed(ri);
				running = true;
			}
		}
		break;
	}
	case AIPKG_ESCORT:
		if (hasDest && near(r.aiDest, 128.0f))
			aiFinish(w, r);
		// Lead the way, but wait when the escorted one falls behind (OpenMW: stop past half extent + 450, go on again
		// within half extent + 250)
		else if (hasDest && (!who || near(who, r.escortWaiting ? 300.0f : 500.0f)))
		{
			r.escortWaiting = false;
			goal = r.aiDest;
		}
		else if (hasDest && who && !elsewhere)
			r.escortWaiting = true;
		else if (elsewhere && who && !near(who, 180.0f))
			goal = who;
		break;
	case AIPKG_ACTIVATE:
	{
		// Goes up to it and uses it again and again, never done by that (OpenMW): only when it's gone or disabled
		int t = w.findRef(r.aiTarget);
		if (t < 0 || !w.refs[t].visible())
		{
			aiFinish(w, r);
			break;
		}
		if (who)
		{
			float dz = who[2] - r.pos[2], dx = who[0] - r.pos[0], dy = who[1] - r.pos[1];
			float reach = w.game.gmstf("imaxactivatedist", 192.0f);
			if (dx * dx + dy * dy + dz * dz <= reach * reach)
			{
				if (w.refs[t].script >= 0)
					w.scripts[w.refs[t].script].activated = true;
			}
			else
				goal = who;
		}
		break;
	}
	}
	if (goal)
	{
		npcMoveTo(ri, goal, speed, dt, true);
		r.moved = true;
		// no headway for 2 s (a travel point beyond a wall): stand rather than walk in place; walk again
		// once it gets somewhere
		r.chaseWindow += dt;
		if (r.chaseWindow >= 1.0f)
		{
			float mx = r.pos[0] - r.chaseAnchor[0], my = r.pos[1] - r.chaseAnchor[1];
			r.chaseStill = mx * mx + my * my < 40.0f * 40.0f ? r.chaseStill + r.chaseWindow : 0.0f;
			r.chaseAnchor[0] = r.pos[0];
			r.chaseAnchor[1] = r.pos[1];
			r.chaseWindow = 0.0f;
		}
		if (r.chaseStill >= 2.0f)
		{
			if (a && a->mode == ANIM_LOOP)
				actorPlay(*set, *a, "Idle", ANIM_IDLE);
		}
		else if (a)
			actorPlay(*set, *a, running ? "RunForward" : "WalkForward", ANIM_LOOP);
	}
	else
	{
		if (a && a->mode == ANIM_LOOP)
			actorPlay(*set, *a, "Idle", ANIM_IDLE);
		// Following / escorting: face whoever it is while standing
		if (who && (r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.aiActive)
		{
			float diff = angleDiff(atan2f(who[0] - r.pos[0], who[1] - r.pos[1]), r.rot[2]);
			r.rot[2] += fmaxf(-kTurnSpeed * 0.5f * dt, fminf(kTurnSpeed * 0.5f * dt, diff));
			r.moved = true;
		}
	}
	w.syncActor(ri);
}

// ---- Sneaking

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
	// OpenMW: an observer in line of sight who passes the awareness check (formulas.cpp) sees the player; the check's
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
	if (testGod && !fatigueOnly && !scripted)
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
	// blow, and a blocked blow still casts). Arrows cast theirs in projectile.cpp
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
	// (resistNormalWeapon judges only a weapon); arrows are judged in projectile.cpp
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
	if (!fatigueOnly)
		damage = fmaxf(1.0f, applyArmor(damage, def.armor + w.actorEffect(target, 3)));   // (+ Shield); OpenMW: at least 1
	damage *= difficultyScale(false);
	logf("combat: player hits %s for %.1f%s", t.id.c_str(), damage, fatigueOnly ? " fatigue" : "");
	{
		char n[16];
		snprintf(n, sizeof(n), "%.0f", damage);
		addIndicator(n, fatigueOnly && t.fatigue > 0.0f ? 0x33cc4c : 0xff334c);
	}
	damageNpc(target, damage, fatigueOnly);
	if (t.dead)
		return;
	int agi = def.attributes[ATTR_AGILITY];
	if (rawDamage > 0.0f && rawDamage >= agi * w.game.gmstf("fknockdownmult", 0.6f) && rand() % 100 >= knockdownOdds(agi))
		knockDown(target, false);
	(void)ranged;
}

void Session::playerSwing(float charge, const PlayerInput& in)
{
	// A lockpick or probe in hand works on the lock / trap under the crosshair instead
	if (InventoryItem* tool = playerToolItem())
	{
		useTool(*tool);
		return;
	}
	InventoryItem* wit = playerWeaponItem();
	const Object* wpn = wit ? w.game.object(wit->id) : nullptr;
	PlayerStats& s = w.stats;
	// fFatigueAttackBase + load x fFatigueAttackMult + weapon weight x swing x fWeaponFatigueMult
	s.fatigue = fmaxf(0.0f, s.fatigue - (w.game.gmstf("ffatigueattackbase", 2.0f)
		+ playerLoad * w.game.gmstf("ffatigueattackmult", 0.0f)
		+ (wpn ? wpn->weight : 0.0f) * charge * w.game.gmstf("fweaponfatiguemult", 0.25f)));
	playSound(-1, charge > 0.66f ? "SwishL" : charge > 0.33f ? "SwishM" : "SwishS");

	// Nearest living NPC in front of the player within reach
	float reach = weaponReach(wpn) + 30.0f;
	float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
	int target = -1;
	float best = reach;
	w.forLoadedActors([&](int i) {
		const Ref& r = w.refs[i];
		if (r.actor < 0 || r.dead || !r.visible())
			return;
		float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist > best || fabsf(r.pos[2] - w.player.feet[2]) > 150.0f)
			return;
		if (dist > 1.0f && (dx * fx + dy * fy) / dist < 0.75f)
			return;
		// not through a wall or a door (OpenMW's melee hit needs line of sight)
		float from[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + 100.0f };
		float to[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
		if (!w.lineOfSight(from, to))
			return;
		best = dist;
		target = i;
	});
	if (target < 0)
	{
		logf("combat: swing hits nothing");
		return;
	}

	Ref& t = w.refs[target];
	const ActorDef& def = w.game.actors[t.actor];
	const int* pa = s.attributes;
	int skill = weaponSkill(wpn);
	// OpenMW's getHitChance: attack term (Fortify Attack, Blind) less their evasion, which is 0 while they are unaware,
	// knocked down or paralyzed; rounded
	bool aware = npcAware(target);
	float chance = roundf(attackTermOf(s.skills[skill], pa[ATTR_AGILITY], pa[ATTR_LUCK], s.fatigue, s.fatigueMax,
		w.effectTotal(117), w.effectTotal(47)) - npcDefense(target, !aware || t.knockTimer > 0.0f));
	if (testGod)
		chance = 100.0f;
	// GBAC leaves sneak attacks (vanilla's x4) and plain weapons on the immune to vanilla
	bool sneakAttack = !aware;
	bool immune = normalWeaponResist(target) >= 100.0f && wpn && !((wpn->flags & 3) || wpn->magic);
	BlowRoll roll = rollBlow(chance, false, sneakAttack || immune);
	bool hit = roll.lands;
	logf("combat: swing at %s, chance %d%%, %s", t.id.c_str(), (int)chance,
		!hit ? "miss" : roll.kind == 0 ? "glance" : roll.kind == 2 && combatMode == COMBAT_GBAC ? "crit" : "hit");
	if (!hit)
	{
		addIndicator("Miss (" + std::to_string((int)fmaxf(0.0f, chance)) + "%)", 0xffffff);
		if (wit && !testGod)
			wearItem(wit, 0.0f);          // a miss still wears the weapon by 1 (OpenMW's reduceWeaponCondition)
		bool wasPeaceful = t.ai != AI_COMBAT;
		makeHostile(target);
		if (wasPeaceful && !def.creature && !t.aggressor)
			crimeSeen(CRIME_ASSAULT, 0, target), reportCrime(target, kBountyAssault);
		return;
	}
	useSkill(skill, 0, roll.skill);
	blowSound(target, roll);
	if (!npcShieldsBurn(target))
		return;
	if (wpn)
	{
		float dmg = weaponDamage(wpn, attackKind, charge, pa[ATTR_STRENGTH]);
		if (wpn->health > 0)
			dmg *= (float)itemCondition(*wit) / wpn->health;
		wearItem(playerWeaponItem(), dmg * w.game.gmstf("fweapondamagemult", 0.1f));
		playerHitsNpc(target, dmg * roll.damage, skill, false, false);
	}
	else    // fists: GBAC leaves the damage alone while the target still has fatigue
	{
		// Hand-to-hand x (fMinHandToHandMult + (fMax - fMin) x swing): fatigue damage; on someone down or paralyzed,
		// health damage x fHandtoHandHealthPer (OpenMW's getHandToHandDamage)
		float lo = w.game.gmstf("fminhandtohandmult", 0.1f), hi = w.game.gmstf("fmaxhandtohandmult", 0.5f);
		float dmg = s.skills[SKILL_HAND_TO_HAND] * (lo + (hi - lo) * charge);
		if (t.knockTimer > 0.0f)
			playerHitsNpc(target, dmg * w.game.gmstf("fhandtohandhealthper", 0.1f) * roll.damage, SKILL_HAND_TO_HAND, false, false);
		else
			playerHitsNpc(target, dmg * (t.fatigue > 0.0f ? 1.0f : roll.damage), SKILL_HAND_TO_HAND, true, false);
	}
	(void)in;
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
	else if (s.fatigue < s.fatigueMax && !testNoFatigueRegen)
		s.fatigue = fminf(s.fatigueMax, s.fatigue + (w.game.gmstf("ffatiguereturnbase", 2.5f)
			+ w.game.gmstf("ffatiguereturnmult", 0.02f) * s.attributes[ATTR_ENDURANCE]) * dt);

	updateDetection(dt);

	// X: the first press draws the weapon (or raises the fists); holding winds up, release strikes
	// (a longer wind-up hits harder). Ranged weapons need ammunition equipped.
	bool canFight = !menu && w.controlsEnabled && w.fightingEnabled && w.player.knockTimer <= 0.0f;
	const Object* wpn = playerWeapon();
	sheatheTimer += dt;
	auto ready = [&]() {
		weaponDrawn = true;
		std::string g = vmGroup(wpn);
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
			drawTimer -= dt;
		else if (isRanged(wpn) && !playerAmmo())
		{
			if (attackCharge < -0.5f)
				notify("You have no ammunition equipped.");
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
				vm.play(VM_WINDUP, (std::string(vmGroup(wpn)) + (isRanged(wpn) ? "Shoot" : attackKind == 2 ? "Thrust"
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
				playerFire(fmaxf(0.1f, attackCharge));
			else
				playerSwing(fmaxf(0.1f, attackCharge), in);
			vm.play(VM_FOLLOW, (std::string(vmGroup(wpn)) + (isRanged(wpn) ? "ShootF" : attackKind == 2 ? "ThrustF"
				: attackKind == 1 ? "SlashF" : "ChopF")).c_str());
		}
		else if (vm.action == VM_WINDUP)
			vm.action = VM_IDLE;          // a menu or knockdown cut the wind-up short: the arms must not stay drawn back
		attackCharge = -1.0f;
	}
	else if (attackCharge < -0.5f && !in.attack)
		attackCharge = -1.0f;
	// Put the weapon away after a while without fighting, or when a menu opens
	if (weaponDrawn && !casting && (putAway || sheatheTimer > 20.0f || (menu && screen != SCR_NONE)) && attackCharge < 0.0f)
	{
		weaponDrawn = false;
		vm.play(VM_UNEQUIP, (std::string(vmGroup(wpn)) + "Uneq").c_str());
	}

	float headTrack = w.game.gmstf("fmaxheadtrackdistance", 400.0f);
	w.forLoadedActors([&](int i) {
		Ref& r = w.refs[i];
		if (r.actor < 0 || r.dead || !r.visible())
			return;
		if (!r.effects.empty() && !menu)
			updateActorEffects(i, dt);
		if (r.dead)
			return;
		// (the player's rule: fFatigueReturnBase + fFatigueReturnMult x their Endurance a second)
		if (r.fatigue < r.fatigueMax)
			r.fatigue = fminf(r.fatigueMax, r.fatigue + (w.game.gmstf("ffatiguereturnbase", 2.5f)
				+ w.game.gmstf("ffatiguereturnmult", 0.02f) * w.game.actors[r.actor].attributes[ATTR_ENDURANCE]) * dt);
		// People near the player turn their heads to them (fMaxHeadTrackDistance), within what a neck turns
		if (r.type == "NPC_")
			if (Actor* ac = w.actorOf(i))
			{
				float target = 0.0f;
				if (w.distanceToPlayer(i) < headTrack)
				{
					float diff = angleDiff(atan2f(w.player.feet[0] - r.pos[0], w.player.feet[1] - r.pos[1]), r.rot[2]);
					if (fabsf(diff) < 1.6f)
						target = -fmaxf(-1.0f, fminf(1.0f, diff));
				}
				ac->headYaw += (target - ac->headYaw) * fminf(1.0f, dt * 4.0f);
				if (fabsf(ac->headYaw) < 0.001f)
					ac->headYaw = 0.0f;
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
			float rating = fightTermOf(i, w.distanceToPlayer(i));        // (formulas.cpp)
			bool calm = r.calmUntil > w.time || w.actorEffect(i, w.game.actors[r.actor].creature ? 50 : 49) > 0.0f;
			float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f }, eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
			if (rating >= 100.0f && !menu && !calm && npcAware(i) && w.lineOfSight(head, eye))
			{
				r.aggressor = true;
				makeHostile(i);
			}
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
				&& w.distanceToPlayer(i) < 1200.0f)
				npcSayTopic(i, "Idle");
			return;
		}
		if (!menu)
			npcCombat(i, dt);
	});
	if (!menu)
		updateProjectiles(dt);
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
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if (r.type != "NPC_" || r.dead || r.ally || r.actor < 0 || !w.active(i) || w.time - r.lastSeen > 1.0f
			|| w.distanceToPlayer(i) > radius || r.ai == AI_COMBAT)
			continue;
		const ActorDef& def = w.game.actors[r.actor];
		if (lower(def.cls) == "guard")
			continue;                        // guards arrest instead
		int base = r.fight >= 0 ? r.fight : def.fight;
		float term = fmaxf(0.0f, fminf(fightTerm, 100.0f - base));
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
	}
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
		if (r.type != "NPC_" || r.dead || r.ally || r.actor < 0 || !w.active(i) || w.distanceToPlayer(i) > radius)
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
		crimeAgainstFaction(w.game.actors[w.refs[victim].actor].faction);
	w.bounty += amount;
	w.globals["pccrimelevel"] = (float)w.bounty;
	notify("Crime reported. Your bounty is " + std::to_string(w.bounty) + " gold.");
	logf("crime: +%d, bounty %d", amount, w.bounty);
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
			applyEffectToPlayer(e, o ? o->name : en->id);
		else
			applyEffectToActor(target, e, true, cost, 100.0f, o ? o->name : en->id);
	logf("combat: %s strikes with %s (charge %.0f)", wit->id.c_str(), en->id.c_str(), wit->charge);
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
	logf("combat: %s's %s strikes the player with %s", w.refs[ri].id.c_str(), item->id.c_str(), sp->second.id.c_str());
}

// Pickpocketing (OpenMW's reading of Morrowind): the thief's and the victim's
// (Sneak + Agility / 5 + Luck / 10) x fatigue, the victim's raised by what the item is worth
int Session::pickpocketBar(int victim, float valueTerm)
{
	const Ref& v = w.refs[victim];
	const ActorDef& def = w.game.actors[v.actor];
	const PlayerStats& s = w.stats;
	float x = (0.2f * s.attributes[ATTR_AGILITY] + 0.1f * s.attributes[ATTR_LUCK] + s.skills[SKILL_SNEAK])
		* fatigueTerm(s.fatigue, s.fatigueMax);
	float y = (valueTerm + 0.2f * def.attributes[ATTR_AGILITY] + 0.1f * def.attributes[ATTR_LUCK] + def.skills[SKILL_SNEAK])
		* fatigueTerm(v.fatigue, v.fatigueMax);
	float t = 2.0f * x - y;
	float pcSneak = (float)s.skills[SKILL_SNEAK];
	float minChance = w.game.gmstf("ipickminchance", 5), maxChance = w.game.gmstf("ipickmaxchance", 75);
	if (t < pcSneak / minChance)
		return (int)(pcSneak / minChance);
	return (int)fminf(maxChance, t);
}

bool Session::pickpocketCaught(float valueTerm)
{
	if (containerRef < 0 || w.refs[containerRef].actor < 0)
		return false;
	int roll = persuadeRoll >= 0 ? persuadeRoll : rand() % 100;       // (tests fix the die)
	return roll > pickpocketBar(containerRef, valueTerm);
}

// Seen by someone awake who noticed the player just now
int Session::crimeWitness(int except)
{
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		if (i != except && r.type == "NPC_" && !r.dead && !r.ally && w.active(i) && w.time - r.lastSeen < 1.0f
			&& w.distanceToPlayer(i) < 2000.0f)
			return i;
	}
	return -1;
}

InventoryItem* Session::playerToolItem()
{
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it.id))
				if (o->type == "LOCK" || o->type == "PROB")
					return &it;
	return nullptr;
}

// Security: a lockpick on a locked door / container, a probe on a trapped one (Morrowind's formula:
// (Security + Agility / 5 + Luck / 10) x quality x fatigue, less the lock level for picks). Each try
// uses the tool up a little; picking someone else's lock where an NPC sees it is trespassing
void Session::useTool(InventoryItem& tool)
{
	const Object* o = w.game.object(tool.id);
	if (!o || target < 0)
		return;
	Ref& r = w.refs[target];
	bool pick = o->type == "LOCK";
	if (r.type != "DOOR" && r.type != "CONT")
		return;
	if (pick ? r.lockLevel <= 0 : (r.trap.empty() || r.disarmed))
		return;
	float x;
	if (pick)
		x = lockChance(r.lockLevel, o->quality);
	else
	{
		auto trapSpell = w.game.spells.find(r.trap);
		x = trapChance(trapSpell != w.game.spells.end() ? (float)trapSpell->second.cost : 0.0f, o->quality);
	}
	if (x <= 0.0f)
	{
		notify(w.game.gmst(pick ? "slockimpossible" : "strapimpossible", "Too complex."));
		return;
	}
	// A use of the tool
	if (tool.condition < 0)
		tool.condition = o->uses > 0 ? o->uses : 25;
	bool ok = (float)(persuadeRoll >= 0 ? persuadeRoll : rand() % 100) <= x;      // (tests fix the die)
	logf("security: %s on %s (level %d), chance %d%%, %s", o->id.c_str(), r.id.c_str(), r.lockLevel, (int)x,
		ok ? "done" : "failed");
	if (ok)
	{
		if (pick)
		{
			r.lockLevel = 0;
			playSound(target, "Open Lock");
		}
		else
		{
			r.disarmed = true;
			playSound(target, "Disarm Trap");
		}
		notify(w.game.gmst(pick ? "slocksuccess" : "strapsuccess", "Success!"));
		useSkill(SKILL_SECURITY, pick ? 1 : 0);      // SKDT: 0 disarming a trap, 1 picking a lock
	}
	else
	{
		playSound(target, pick ? "Open Lock Fail" : "Disarm Trap Fail");
		notify(w.game.gmst(pick ? "slockfail" : "strapfail", "Failed."));
	}
	if (pick && w.ownedByOther(target))
	{
		int witness = crimeWitness();
		if (witness >= 0)
			crimeSeen(CRIME_TRESPASS, 0, -1), reportCrime(witness, crimeBounty(CRIME_TRESPASS, 0));
	}
	if (--tool.condition <= 0)
	{
		notify(o->name + " is used up.");
		w.removeItem(tool.id, 1);
	}
}

bool Session::springTrap(int ref)
{
	Ref& r = w.refs[ref];
	if (r.trap.empty() || r.disarmed)
		return false;
	r.disarmed = true;
	auto sp = w.game.spells.find(r.trap);
	logf("trap: %s springs %s", r.id.c_str(), r.trap.c_str());
	if (sp == w.game.spells.end())
		return false;
	playSound(ref, "Disarm Trap Fail");          // (OpenMW's sound for the trap going off)
	for (auto& e : sp->second.effects)
		applyEffectToPlayer(e, sp->second.name);
	return true;
}

void Session::takeOwned(int from, const std::string& item, int count)
{
	const Object* o = w.game.object(item);
	if (from >= 0)
		w.markStolen(item, count, w.refs[from].type == "NPC_" ? w.refs[from].idLower
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
		x = haggleChance(barterRef, price, offer, barterSell, &d);        // (formulas.cpp)
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

// A smith's price for mending an item (OpenMW's formula): the missing condition over
// max condition / value, times fRepairMult, then bartered like a purchase
int Session::repairPrice(int ri, const InventoryItem& it)
{
	const Object* o = w.game.object(it.id);
	if (!o || o->health <= 0)
		return 0;
	float p = fmaxf(1.0f, (float)o->value);
	float r = fmaxf(1.0f, (float)(int)(o->health / p));
	int x = (int)((o->health - itemCondition(it)) / r);
	x = (int)(w.game.gmstf("frepairmult", 1.0f) * x);
	return barterPrice(ri, x < 1 ? 1 : x, true);
}

// ---- First-person view model

// Which arms / weapon / shield the view model shows and how it reacts (hits, blocks, knockdown)
void Session::viewModelUpdate(const PlayerInput& in, float dt)
{
	if (!vm.ready)
		return;
	const Object* wpn = playerWeapon();
	vm.group = vmGroup(wpn);
	std::string shield;
	for (auto& it : w.inventory)
		if (it.equipped && w.game.object(it.id) && w.game.object(it.id)->type == "ARMO"
			&& w.game.object(it.id)->subtype == ARMO_SHIELD)
			shield = it.id;
	bool oneHanded = vm.group == "1h" || vm.group == "HH";
	bool casting = vm.action == VM_CAST;
	bool show = weaponDrawn || vm.action == VM_UNEQUIP;
	vm.rebuild(w, show && !casting && wpn ? wpn->id : "", show && !casting && oneHanded ? shield : "");
	if (weaponDrawn && w.player.knockTimer > 0.0f && vm.action != VM_KNOCKDOWN)
		vm.play(VM_KNOCKDOWN, "KnockDown");
	if (blockUntil > w.time && vm.action == VM_IDLE && !shield.empty())
		vm.play(VM_BLOCK, "Block");
	if (!weaponDrawn && vm.action == VM_IDLE)
		vm.action = VM_NONE;
	if (weaponDrawn && vm.action == VM_NONE)
		vm.action = VM_IDLE;
	bool moving = fabsf(in.moveX) + fabsf(in.moveY) > 0.2f;
	vm.update(dt, moving, !w.player.sneaking);
	// The body seen from outside: dressed as the player is, doing what they do
	if (in.togglePov && !menuOpen())
	{
		thirdPerson = !thirdPerson;
		thirdDistance = 0.0f;
	}
	previewFace = screen == SCR_RACE;          // character creation: the face being chosen
	if (thirdPerson || previewFace)
	{
		body.rebuild(w, weaponDrawn);
		float speed = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY) * w.player.runSpeed * w.player.loadSpeed;
		body.update(w, dt, speed, w.player.swimming, w.player.sneaking, weaponDrawn);
	}
}
