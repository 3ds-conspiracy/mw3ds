// Animation choices: stance and attack groups, swings, knockdown, the first-person view model
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <malloc.h>

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

// An NPC's attack type, the more likely the more it does on average (OpenMW's chooseBestAttack): 0 chop, 1 slash, 2
// thrust; with no weapon, any of them (CharacterController::getRandomAttackType)
static int npcAttackKind(const Object* w)
{
	if (!w)
		return rand() % 3;
	int slash = (w->slash[0] + w->slash[1]) / 2, chop = (w->chop[0] + w->chop[1]) / 2, thrust = (w->thrust[0] + w->thrust[1]) / 2;
	float roll = frand() * (slash + chop + thrust);
	return roll <= slash ? 1 : roll <= slash + thrust ? 2 : 0;
}

// The blow an NPC or creature starts, its kind kept in the Ref for the damage at its hit. A creature with its own
// attacks: one of Attack1..3 at random (OpenMW's chooseRandomAttackAnimation; Creature::hit deals the attack the group
// names); else (people, biped creatures) the group for what it holds, by its chop / slash / thrust (Attack1h,
// Attack1hSl, Attack1hTh)
std::string blowGroup(const ActorSet& set, const Actor& a, Ref& r, const Object* w)
{
	std::string own = actorRandomGroup(set, a, "Attack");
	if (!own.empty())
	{
		r.attackKind = (u8)(own.back() - '1');
		return own;
	}
	r.attackKind = w && isRanged(w) ? 0 : (u8)npcAttackKind(w);
	static const char* kTag[] = { "", "Sl", "Th" };
	std::string g = std::string(attackGroup(w)) + kTag[r.attackKind];
	return actorFindGroup(actorSkeleton(set, a.skeleton), g.c_str()) >= 0 ? g : std::string(attackGroup(w));
}

// At the hit of a melee blow (Attack1h ...), the follow-through by its strength: under 0.33 the small one ("Attack1hS"),
// under 0.66 the medium one ("Attack1hM"), else the large one the group runs on into (OpenMW's character controller)
void attackFollowThrough(ActorSet& set, Actor& a, float strength)
{
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	if (strength >= 0.66f || a.mode != ANIM_ONCE || a.group < 0 || a.group >= (int)sk.groups.size()
		|| strncmp(sk.groups[a.group].name, "Attack", 6) != 0)
		return;
	std::string next = std::string(sk.groups[a.group].name) + (strength < 0.33f ? "S" : "M");
	if (actorFindGroup(sk, next.c_str()) >= 0)
		actorPlay(set, a, next.c_str(), ANIM_ONCE);
}

// First-person animation family of a weapon (tools/convert/firstperson.py group names); a lockpick or probe in
// hand moves as a one-handed weapon does (OpenMW's PickProbe weapon type uses the "1h" short group)
const char* vmGroup(const Object* w, bool tool)
{
	if (!w)
		return tool ? "1h" : "HH";
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

// On the floor (OpenMW's refreshHitRecoilAnims): knocked down, its KnockDown played through (the getting up in it);
// knocked out, lying in its KnockOut's loop for as long as the fatigue is under 0, then the rest of it (getting up).
// It was 3.5 s out cold whatever the fatigue
void Session::npcKnockUpdate(int ri, float dt)
{
	Ref& r = w.refs[ri];
	Actor* a = w.actorOf(ri);
	const AnimGroup* g = a && a->group >= 0 ? &actorSkeleton(*w.actorsOf(ri), a->skeleton).groups[a->group] : nullptr;
	if (r.outCold && r.fatigue < 0.0f)
	{
		if (g && g->loopStop > g->loopStart && a->time > g->loopStop)
			a->time = g->loopStart;
		r.knockTimer = fmaxf(r.knockTimer, 0.5f);
		return;
	}
	if (r.outCold)
	{
		// come round: the rest of the group, then up
		r.outCold = false;
		if (g)
			r.knockTimer = fmaxf(0.1f, g->stop - a->time);
	}
	r.knockTimer -= dt;
	if (r.knockTimer <= 0.0f)
	{
		r.knockTimer = 0.0f;
		if (a)
			actorPlay(*w.actorsOf(ri), *a, "Idle", ANIM_IDLE);
	}
}

// Whether a swing now would reach this actor: living, in front of the player (within about 40 degrees) within the
// weapon's reach of its edge (meleeReach; it was the reach + 30 from its middle: fists 0.7), less than the reach above
// or below, and not through a wall or a door (OpenMW's melee hit needs line of
// sight): chest to chest, or eye to head (over a counter: a trader behind a bar is hit from across it, the chest line
// clipped the bar's top). dist: how far, across
bool Session::inSwing(int i, float* distOut)
{
	const Ref& r = w.refs[i];
	if (r.actor < 0 || r.dead || !r.visible())
		return false;
	InventoryItem* wit = playerWeaponItem();
	const float meleeAt = meleeReach(w, wit ? w.game.object(*wit) : nullptr, false);
	float reach = meleeAt + kPlayerHalfLength + actorHalfLength(i);
	float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float dist = sqrtf(dx * dx + dy * dy);
	if (dist >= reach || fabsf(r.pos[2] - w.player.feet[2]) >= meleeAt)
		return false;
	if (dist > 1.0f && (dx * fx + dy * fy) / dist < 0.75f)
		return false;
	float from[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + 100.0f };
	float to[3] = { r.pos[0], r.pos[1], r.pos[2] + 100.0f };
	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f };
	if (!w.lineOfSight(from, to) && !w.lineOfSight(eye, head))
		return false;
	if (distOut)
		*distOut = dist;
	return true;
}

// Who a swing now would strike: the nearest actor it would reach
int Session::swingTarget()
{
	int target = -1;
	float best = 1e9f;
	w.forLoadedActors([&](int i) {
		float dist;
		if (inSwing(i, &dist) && dist <= best)
		{
			best = dist;
			target = i;
		}
	});
	return target;
}

float Session::playerSwing(float charge, const PlayerInput& in)
{
	// A lockpick or probe in hand works on the lock / trap under the crosshair instead
	if (InventoryItem* tool = playerToolItem())
	{
		useTool(*tool);
		return charge;
	}
	InventoryItem* wit = playerWeaponItem();
	const Object* wpn = wit ? w.game.object(*wit) : nullptr;
	PlayerStats& s = w.stats;
	// fFatigueAttackBase + load x fFatigueAttackMult + weapon weight x swing x fWeaponFatigueMult
	s.fatigue = fmaxf(0.0f, s.fatigue - (w.game.gmstf("ffatigueattackbase", 2.0f)
		+ playerLoad * w.game.gmstf("ffatigueattackmult", 0.0f)
		+ (wpn ? wpn->weight : 0.0f) * charge * w.game.gmstf("fweaponfatiguemult", 0.25f)));
	playSound(-1, charge > 0.66f ? "SwishL" : charge > 0.33f ? "SwishM" : "SwishS");

	int target = swingTarget();
	if (target < 0)
	{
		logf("combat: swing hits nothing");
		return charge;
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
	if (testGod && testGodBlows)
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
		// uber quest tests give the player skills and Fortify Attack enough that no blow can miss: one that does is a
		// broken hit chance (or a test whose setup fell short)
		if (testLegit)
			monitorOnce(("legitmiss:" + t.id).c_str(), "the player's blow at %s missed (chance %d%%) in a LEGIT run", t.id.c_str(), (int)chance);
		if (wit && !(testGod && testGodBlows))
			wearItem(wit, 0.0f);          // a miss still wears the weapon by 1 (OpenMW's reduceWeaponCondition)
		bool wasPeaceful = t.ai != AI_COMBAT;
		makeHostile(target);
		if (wasPeaceful && !def.creature && !t.aggressor)
			crimeSeen(CRIME_ASSAULT, 0, target), reportCrime(target, kBountyAssault);
		return 0.0f;      // (a miss follows through as the weakest blow: OpenMW)
	}
	useSkill(skill, 0, roll.skill);
	blowSound(target, roll);
	if (!npcShieldsBurn(target))
		return charge;
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
	return charge;
}

// ---- First-person view model

// Which arms / weapon / shield the view model shows and how it reacts (hits, blocks, knockdown)
void Session::viewModelUpdate(const PlayerInput& in, float dt)
{
	if (!vm.ready)
		return;
	const Object* wpn = playerWeapon();
	InventoryItem* toolItem = playerToolItem();
	vm.group = vmGroup(wpn, toolItem != nullptr);
	std::string shield;
	for (auto& it : w.inventory)
		if (it.equipped && w.game.object(it) && w.game.object(it)->recType == REC_ARMO
			&& w.game.object(it)->subtype == ARMO_SHIELD)
			shield = it.id;
	bool oneHanded = vm.group == "1h" || vm.group == "HH";
	bool casting = vm.action == VM_CAST;
	bool show = weaponDrawn || vm.action == VM_UNEQUIP;
	// The arrow or bolt in the bow or crossbow (OpenMW's attachArrow / releaseArrow): an arrow goes on the string at
	// "shoot attach" and leaves it at "shoot release"; a crossbow holds a bolt from the moment it is drawn, lets it go
	// at "shoot release", takes the next at "shoot follow attach" and gives it up as it is put away
	std::string ammoIn;
	const Object* ammo = show && !casting && wpn ? playerAmmo() : nullptr;
	if (ammo && ammo != wpn && (vm.group == "Bow" || vm.group == "Xbow") && vm.ready)
	{
		const Skeleton& sk = vm.set.skeletons[0];
		const Actor& arms = vm.set.actors[0];
		const char* g = arms.group >= 0 && arms.group < (int)sk.groups.size() ? sk.groups[arms.group].name : "";
		auto mark = [&](const char* name) {
			int k = actorFindGroup(sk, name);
			return k >= 0 ? sk.groups[k].start : 0.0f;
		};
		bool in = vm.group == "Bow" ? strncmp(g, "BowShoot", 16) == 0 && arms.time >= mark("BowAttach")
			: strncmp(g, "XbowUneq", 16) != 0 && (strncmp(g, "XbowShootF", 16) != 0 || arms.time >= mark("XbowFAttach"));
		if (in)
			ammoIn = ammo->id;
	}
	vm.rebuild(w, show && !casting ? (wpn ? wpn->id.str() : toolItem ? toolItem->id : "") : "", show && !casting && oneHanded ? shield : "",
		ammoIn);
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
	// Dead, the view goes outside to see the body fall (OpenMW's useDeathCamera); alive again (a save loaded), back in
	if (playerDead && !thirdPerson && !deathView)
	{
		thirdPerson = deathView = true;
		thirdDistance = 0.0f;
		body.acting = BODY_NONE;
	}
	else if (!playerDead && deathView)
	{
		thirdPerson = deathView = false;
		body.acting = BODY_NONE;
	}
	// The body seen from outside: dressed as the player is, doing what they do
	if (in.togglePov && !menuOpen())
	{
		thirdPerson = !thirdPerson;
		thirdDistance = 0.0f;
		body.acting = BODY_NONE;              // a blow or cast under way when the view changes starts afresh
	}
	previewFace = screen == SCR_RACE;          // character creation: the face being chosen
	if (!previewFace)
		previewSpin = 0.0f;
	if (thirdPerson || previewFace)
	{
		body.rebuild(w, weaponDrawn);
		float speed = sqrtf(in.moveX * in.moveX + in.moveY * in.moveY) * w.player.runSpeed * w.player.loadSpeed;
		// The arms do what the first-person ones do: a blow by the weapon's NPC group (a lockpick or probe swings as a
		// one-handed weapon), a cast by its first effect's range, as NPCs do (npcCastSpell)
		BodyAct act;
		// the blow by the kind the player swings (chop, slash, thrust: Attack1h, Attack1hSl, Attack1hTh)
		static const char* kTag[] = { "", "Sl", "Th" };
		bodyAttack = std::string(!wpn && toolItem ? "Attack1h" : attackGroup(wpn));
		if (body.ready && !body.set.actors.empty() && attackKind >= 0 && attackKind < 3 && !isRanged(wpn)
			&& actorFindGroup(actorSkeleton(body.set, body.set.actors[0].skeleton), (bodyAttack + kTag[attackKind]).c_str()) >= 0)
			bodyAttack += kTag[attackKind];
		act.group = bodyAttack.c_str();
		act.moveX = in.moveX;
		act.moveY = in.moveY;
		// (a jump, or a real fall: not a step down a slope, a frame off the ground)
		act.airborne = !w.player.swimming && !w.player.flying && !w.player.onGround && (w.player.jumpFlight || w.player.vz < -150.0f);
		act.flying = (w.player.levitate > 0.0f || w.player.flying) && !w.player.swimming;
		act.action = vm.action == VM_WINDUP ? BODY_WINDUP : vm.action == VM_FOLLOW ? BODY_STRIKE : BODY_NONE;
		if (vm.action == VM_CAST)
		{
			int range = castDef.effects.empty() ? 0 : castDef.effects[0].range;
			act.action = BODY_CAST;
			act.group = range == 2 ? "CastTarget" : range == 1 ? "CastTouch" : "CastSelf";
		}
		act.hit = bodyHit;
		bodyHit = false;
		act.charge = vm.charge;
		act.speed = vm.speed;
		act.strength = blowStrength;
		act.stance = vm.group == "Bow" || vm.group == "Throw" ? "1h" : vm.group == "Xbow" ? "2c" : vm.group.c_str();
		if (playerDead)
		{
			act.action = BODY_DEATH;              // falls and lies there (OpenMW's death camera shows it)
			act.group = "Death1";
		}
		body.update(w, dt, speed, w.player.swimming, w.player.sneaking, weaponDrawn, act);
	}
}

// Footsteps while walking; OpenMW's Npc::getSoundIdFromSndGen picks the sound: none flying, "Swim" while
// swimming, "FootWater" with the feet under the surface, else the ground's
void Session::updateFootsteps(const PlayerInput& in, bool menu, float dt)
{
	float moving = fabsf(in.moveX) + fabsf(in.moveY);
	bool wading = false;
	if (w.current >= 0)
	{
		Cell& stepCell = w.here();
		wading = stepCell.hasWater() && w.player.feet[2] < stepCell.waterZ;
	}
	if (!menu && !w.player.flying && (w.player.swimming || w.player.onGround) && moving > 0.2f)
	{
		stepTimer -= dt;
		if (stepTimer <= 0.0f)
		{
			const char* side = stepLeft ? "Left" : "Right";
			playSound(-1, std::string(w.player.swimming ? "Swim " : wading ? "FootWater" : "FootBare") + side);
			stepLeft = !stepLeft;
			stepTimer = w.player.swimming ? 0.65f : 0.42f;
		}
	}
	else
		stepTimer = 0.1f;
}
