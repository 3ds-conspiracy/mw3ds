// Arrows, bolts, thrown weapons and spell bolts in flight. They fly straight, and hit the first
// actor (an upright cylinder) or wall along their way each frame.
//   marksman hit chance  as melee with the Marksman skill (unaware targets are always hit)
//   damage  (launcher chop + ammunition chop, by how far the shot was drawn; thrown weapons count
//           twice) x (0.5 + strength / 100), then armor as for blows
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <unordered_set>
#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"

static const float kActorRadius = 34.0f, kActorHeight = 135.0f;
static const float kPlayerRadius = 26.0f, kPlayerHeight = 128.0f;
static const int SKILL_MARKSMAN = 23;

static float frand() { return (rand() % 1000) / 1000.0f; }

// Segment a + t (b - a), t in 0..1, against an upright cylinder: earliest t or -1
static float hitCylinder(const float a[3], const float b[3], const float base[3], float radius, float height)
{
	float dx = b[0] - a[0], dy = b[1] - a[1];
	float fx = a[0] - base[0], fy = a[1] - base[1];
	float A = dx * dx + dy * dy, B = 2.0f * (fx * dx + fy * dy), C = fx * fx + fy * fy - radius * radius;
	float t;
	if (C <= 0.0f)
		t = 0.0f;                                  // starts inside
	else
	{
		if (A < 1e-6f)
			return -1.0f;
		float disc = B * B - 4.0f * A * C;
		if (disc < 0.0f)
			return -1.0f;
		t = (-B - sqrtf(disc)) / (2.0f * A);
		if (t < 0.0f || t > 1.0f)
			return -1.0f;
	}
	float z = a[2] + (b[2] - a[2]) * t;
	return z >= base[2] && z <= base[2] + height ? t : -1.0f;
}

static u32 spellGlow(const SpellDef& sp)
{
	for (auto& e : sp.effects)
		switch (e.effect)
		{
		case 14: return 0xFF2070FF;                // fire
		case 15: return 0xFFFF60C0;                // shock
		case 16: return 0xFFFFD090;                // frost
		case 27: return 0xFF40E060;                // poison
		default: break;
		}
	return 0xFFE0C0FF;
}

void Session::fireProjectile(int owner, const float from[3], const float dir[3], float speed, const std::string& item,
	const std::string& weapon, float charge, const std::string& spell)
{
	Projectile p;
	for (int k = 0; k < 3; k++)
	{
		p.pos[k] = from[k];
		p.vel[k] = dir[k] * speed;
	}
	p.owner = owner;
	p.item = item;
	p.weapon = weapon;
	p.charge = charge;
	p.spell = spell;
	p.life = 4.0f;
	if (!spell.empty())
	{
		auto it = w.game.spells.find(spell);
		p.glow = it != w.game.spells.end() ? spellGlow(it->second) : 0xFFFFFFFF;
		if (it != w.game.spells.end())
			p.bolt = boltPiece(it->second, &p.boltEffect);
		p.life = 3.0f;
	}
	projectiles.push_back(p);
}

// What happens when it reaches someone: victim is a ref index, or -1 for the player
// A shot that misses or strikes a wall still wears the player's bow or crossbow by 1 (OpenMW's reduceWeaponCondition
// with no hit); a thrown weapon is gone anyway
void Session::wearLauncher(const Projectile& p)
{
	InventoryItem* bow = playerWeaponItem();
	if (p.owner < 0 && bow && !(testGod && testGodBlows) && p.weapon != p.item && lower(bow->id) == p.weapon)
		wearItem(bow, 0.0f);
}

void Session::projectileHits(Projectile& p, int victim)
{
	if (!p.spell.empty())
	{
		auto it = w.game.spells.find(p.spell);
		if (it == w.game.spells.end())
			return;
		const SpellDef& sp = it->second;
		for (auto& e : sp.effects)
		{
			if (e.range != 2)
				continue;
			if (victim < 0)
				applyEffectToPlayer(e.effect == 86 ? SpellEffect{ 23, e.skill, e.attribute, e.min, e.max, e.duration, 2 } : e, sp.name, p.owner, (float)sp.cost);
			else
				applyEffectToActor(victim, e, p.owner < 0, (float)sp.cost, p.owner < 0 ? (float)castChance(sp) : 100.0f, sp.name);
		}
		playSound(victim, spellSound(sp, 2));
		{
			// the hit visual at the victim's feet (it is modelled around them), the burst around the chest
			float feet[3] = { victim < 0 ? w.player.feet[0] : w.refs[victim].pos[0], victim < 0 ? w.player.feet[1] : w.refs[victim].pos[1],
				victim < 0 ? w.player.feet[2] : w.refs[victim].pos[2] };
			float at[3] = { feet[0], feet[1], feet[2] + 60.0f };
			spellVfx(sp, 1, feet, victim < 0 ? -1 : victim, victim < 0 ? w.player.yaw : w.refs[victim].rot[2]);
			areaBurst(sp, 2, at, victim, p.owner);
		}
		logf("combat: %s hits %s", sp.name.c_str(), victim < 0 ? "the player" : w.refs[victim].id.c_str());
		return;
	}
	const Object* ammo = w.game.object(p.item);
	const Object* launcher = w.game.object(p.weapon);
	if (!ammo)
		return;
	float c = p.charge;
	float dmg = ammo->chop[0] + (ammo->chop[1] - ammo->chop[0]) * c;
	if (launcher && launcher != ammo)
		dmg += launcher->chop[0] + (launcher->chop[1] - launcher->chop[0]) * c;
	else
		dmg *= 2.0f;                                // thrown: weapon and ammunition at once
	if (p.owner < 0 && victim >= 0)
	{
		Ref& t = w.refs[victim];
		const ActorDef& def = w.game.actors[t.actor];
		const PlayerStats& s = w.stats;
		// OpenMW's getHitChance with Marksman (evasion 0 when unaware, knocked down or paralyzed)
		bool aware = npcAware(victim);
		float chance = roundf(attackTermOf(s.skills[SKILL_MARKSMAN], s.attributes[ATTR_AGILITY], s.attributes[ATTR_LUCK],
			s.fatigue, s.fatigueMax, w.effectTotal(117), w.effectTotal(47)) - npcDefense(victim, !aware || t.knockTimer > 0.0f));
		// Resist Normal Weapons (OpenMW): a thrown weapon, or a plain launcher, leaves it to the missile to get through
		bool launcherSpecial = launcher && launcher != ammo && ((launcher->flags & 3) || launcher->magic);
		bool missileSpecial = (ammo->flags & 3) || ammo->magic;
		float normalResist = normalWeaponResist(victim);
		bool resisted = normalResist != 0.0f && !launcherSpecial && !missileSpecial;
		bool immune = resisted && normalResist >= 100.0f;
		// GBAC (Options > Combat): glances and crits (x1.5 at range); sneak shots stay vanilla
		BlowRoll roll = rollBlow(chance, true, !aware || immune);
		bool hit = roll.lands;
		logf("combat: %s at %s, chance %d%%, %s", p.item.c_str(), t.id.c_str(), (int)chance, hit ? "hit" : "miss");
		if (!hit)
		{
			addIndicator("Miss (" + std::to_string((int)fmaxf(0.0f, chance)) + "%)", 0xffffff);
			wearLauncher(p);
			bool peaceful = t.ai != AI_COMBAT && t.duelFoe < 0;
			makeHostile(victim);
			if (peaceful && !def.creature && !t.aggressor)
				crimeSeen(CRIME_ASSAULT, 0, victim), reportCrime(victim, kBountyAssault);
			return;
		}
		useSkill(SKILL_MARKSMAN, 0, roll.skill);
		blowSound(victim, roll);
		dmg *= (w.game.gmstf("fdamagestrengthbase", 0.5f) + 0.1f * w.game.gmstf("fdamagestrengthmult", 0.1f) * s.attributes[ATTR_STRENGTH])
			* roll.damage;
		InventoryItem* bow = playerWeaponItem();
		if (bow && launcher && lower(bow->id) == lower(launcher->id) && launcher->health > 0)
		{
			dmg *= (float)itemCondition(*bow) / launcher->health;
			wearItem(bow, dmg * w.game.gmstf("fweapondamagemult", 0.1f));
		}
		if (resisted)
			dmg *= 1.0f - fminf(1.0f, normalResist / 100.0f);
		// the arrow's own on-strike enchantment, and the target's elemental shields on the shooter (OpenMW's projectileHit)
		strikeEnchantment(victim, ammo);
		if (!npcShieldsBurn(victim))
			return;
		playerHitsNpc(victim, dmg, SKILL_MARKSMAN, false, true);
		// Some arrows can be taken back from the body: fProjectileThrownStoreChance percent, not enchanted ones
		if (!ammo->magic && rand() % 100 < (int)w.game.gmstf("fprojectilethrownstorechance", 25.0f))
			t.contents.emplace_back(1, lower(p.item));
	}
	else if (p.owner >= 0 && victim < 0)
	{
		Ref& r = w.refs[p.owner];
		const ActorDef& def = w.game.actors[r.actor];
		const int* pa = w.stats.attributes;
		bool helpless = w.player.knockTimer > 0.0f || w.effectTotal(45) > 0.0f;
		float chance = roundf(attackTermOf(def.skills[SKILL_MARKSMAN], def.attributes[ATTR_AGILITY], def.attributes[ATTR_LUCK],
			r.fatigue, r.fatigueMax, w.actorEffect(p.owner, 117), w.actorEffect(p.owner, 47)) - playerDefense(helpless));
		(void)pa;
		BlowRoll roll = rollBlow(chance, true, false);
		if (!roll.lands)
		{
			logf("combat: %s's %s misses", r.id.c_str(), p.item.c_str());
			return;
		}
		blowSound(-1, roll);
		npcStrikeEnchantment(p.owner, ammo);
		if (!playerShieldsBurn(p.owner))
			return;
		npcHitsPlayer(p.owner, dmg * (w.game.gmstf("fdamagestrengthbase", 0.5f) + 0.1f * w.game.gmstf("fdamagestrengthmult", 0.1f)
			* def.attributes[ATTR_STRENGTH]) * roll.damage, false, SKILL_MARKSMAN, false, true);
	}
}

void Session::updateProjectiles(float dt)
{
	for (size_t i = 0; i < projectiles.size();)
	{
		Projectile& p = projectiles[i];
		p.life -= dt;
		// Arrows, bolts and thrown weapons fall a little, nose following their path: OpenMW's moveProjectiles pulls them
		// down at a tenth of the world's gravity (9.8 m/s2 x 69.99 units a metre x 0.1); spell bolts fly straight
		if (p.spell.empty() && !p.item.empty())
			p.vel[2] -= 9.8f * 69.99f * 0.1f * dt;
		float next[3] = { p.pos[0] + p.vel[0] * dt, p.pos[1] + p.vel[1] * dt, p.pos[2] + p.vel[2] * dt };
		float best = 2.0f;
		int victim = -2;                             // -2 nothing, -1 player, >= 0 ref
		// Actors (the shooter's own arrows pass through them; NPC shots only hurt the player)
		if (p.owner < 0)
			w.forLoadedActors([&](int k) {
				const Ref& r = w.refs[k];
				if (r.actor < 0 || r.dead || !r.visible())
					return;
				float t = hitCylinder(p.pos, next, r.pos, kActorRadius, kActorHeight);
				if (t >= 0.0f && t < best)
				{
					best = t;
					victim = k;
				}
			});
		else if (!playerDead)
		{
			float t = hitCylinder(p.pos, next, w.player.feet, kPlayerRadius, kPlayerHeight);
			if (t >= 0.0f && t < best)
			{
				best = t;
				victim = -1;
			}
		}
		// Walls and the ground
		bool wall = false;
		for (LoadedCell* l : w.loaded)
		{
			float t;
			if (collisionRaycast(l->cell.collision, p.pos, next, &t) && t < best)
			{
				best = t;
				wall = true;
				victim = -2;
			}
		}
		if (victim != -2)
		{
			Projectile hit = p;
			projectiles.erase(projectiles.begin() + i);
			projectileHits(hit, victim);
			continue;
		}
		if (wall || p.life <= 0.0f)
		{
			if (wall && !p.spell.empty())
			{
				auto it = w.game.spells.find(p.spell);
				playSound(-1, it != w.game.spells.end() ? spellSound(it->second, 2) : std::string("destruction hit"));
				// where it struck the wall: back off a little so the burst isn't inside it
				if (it != w.game.spells.end())
				{
					float at[3];
					for (int k = 0; k < 3; k++)
						at[k] = p.pos[0 + k] + p.vel[k] * dt * std::max(0.0f, best - 0.05f);
					areaBurst(it->second, 2, at, -2, p.owner);
				}
			}
			if (wall && p.spell.empty())
				wearLauncher(p);
			projectiles.erase(projectiles.begin() + i);
			continue;
		}
		for (int k = 0; k < 3; k++)
			p.pos[k] = next[k];
		i++;
	}
	updateCreatureFx(dt);
	updateWorldFx();
	vfxTrimTimer += dt;
	if (vfxTrimTimer >= 0.25f)
	{
		vfxTrimTimer = 0.0f;
		trimVfxPieces();
	}
}
