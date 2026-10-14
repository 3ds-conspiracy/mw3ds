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
#include "console.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "session.h"

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
			bool peaceful = t.ai != AI_COMBAT;
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

// Spell visuals' pieces (meshes, flipbook sheets, baked frames) stay loaded once seen, for the next cast; only with
// linear memory running short are those no visual is playing let go (the next cast loads its own again: the log's
// "viewmodel: loaded" lines say what that costs). Freed in the update: the memory goes back once the GPU is done
// with the frame (linearRetire)
void Session::trimVfxPieces()
{
	if (linearSpaceFree() >= (g_profile.slowCpu ? (3u << 20) : (4u << 20)))
		return;
	std::unordered_set<std::string> fx, used;
	for (auto& kv : w.game.firstPerson.vfx)
		fx.insert(kv.second);
	for (auto& v : vfx)
		used.insert(v.piece);
	int kept = 0;
	std::vector<std::string> idle;
	for (auto& kv : vm.pieces)
		if (kv.second && fx.count(kv.first))
		{
			kept++;
			if (!used.count(kv.first))
				idle.push_back(kv.first);
		}
	int freed = 0;
	for (auto& n : idle)
	{
		fpFreePiece(w, vm.pieces[n]);
		vm.pieces.erase(n);
		kept--;
		freed++;
	}
	if (freed)
		logf("vfx: %d unused visual pieces let go (%d kept, linear free %u KB)", freed, kept, (unsigned)(linearSpaceFree() / 1024));
}

void Session::spawnVfx(const std::string& staticId, const float pos[3], float life, int follow, float yaw, float scale, bool loop, int effect)
{
	auto it = w.game.firstPerson.vfx.find(lower(staticId));
	// (prototype, particles.py: MW3DS_PFX=flip / path / spr, several with commas, draws a visual's flipbook, baked
	// paths or particle sprites where it has one)
	if (const char* pfx = pfxModeList())
	{
		std::string modes = pfx;
		for (size_t a = 0; a <= modes.size();)
		{
			size_t b = modes.find(',', a);
			if (b == std::string::npos)
				b = modes.size();
			auto alt = w.game.firstPerson.vfx.find(lower(staticId) + "#" + modes.substr(a, b - a));
			if (alt != w.game.firstPerson.vfx.end())
			{
				it = alt;
				break;
			}
			a = b + 1;
		}
	}
	if (it == w.game.firstPerson.vfx.end())
		return;
	// Many casters at once: only those near the player show, the nearest when there are too many
	auto dist2 = [&](const float* p) {
		float dx = p[0] - w.player.feet[0], dy = p[1] - w.player.feet[1], dz = p[2] - w.player.feet[2];
		return dx * dx + dy * dy + dz * dz;
	};
	float d2 = dist2(pos);
	if (d2 > kVfxRange * kVfxRange)
		return;
	// (placed objects' own visuals don't count: they come and go with their objects)
	int spells = 0;
	for (auto& o : vfx)
		spells += !o.world;
	if (spells >= kMaxVfx)
	{
		int far = -1;
		for (size_t i = 0; i < vfx.size(); i++)
			if (!vfx[i].world && (far < 0 || dist2(vfx[i].pos) > dist2(vfx[far].pos)))
				far = (int)i;
		if (far < 0 || dist2(vfx[far].pos) <= d2)
			return;
		vfx.erase(vfx.begin() + far);
	}
	// (a continuous one cast again takes the old one's place: one bubble, not two)
	if (loop)
		for (size_t i = 0; i < vfx.size();)
			if (vfx[i].loop && vfx[i].piece == it->second && vfx[i].follow == follow)
				vfx.erase(vfx.begin() + i);
			else
				i++;
	// a visual lasts as long as its longest controller runs, particle sprays' too (OpenMW drops a non-looping effect
	// when it has played: FindMaxControllerLengthVisitor), else as long as its own animation
	FpPiece* piece = vm.piece(w, it->second);
	auto len = w.game.firstPerson.vfxLength.find(it->second);
	float length = len != w.game.firstPerson.vfxLength.end() ? len->second : piece ? piece->length : 0.0f;
	Vfx v;
	v.piece = it->second;
	v.what = lower(staticId);
	memcpy(v.pos, pos, sizeof(v.pos));
	v.age = 0.0f;
	v.life = length > 0.0f && !loop ? length : life;
	v.loop = loop;
	v.follow = follow;
	v.yaw = yaw;
	v.scale = scale;
	v.effect = effect;
	vfx.push_back(v);
}

void Session::spellVfx(const SpellDef& sp, int kind, const float pos[3], int follow, float yaw, int range)
{
	static const char* kDefault[3] = { "vfx_defaultcast", "vfx_defaulthit", "vfx_defaultarea" };
	std::vector<std::pair<std::string, float>> shown;
	for (auto& e : sp.effects)
	{
		if (kind == 2 && (e.range != range || e.area <= 0))
			continue;
		auto me = w.game.magicEffects.find(e.effect);
		if (me == w.game.magicEffects.end())
			continue;
		const std::string& id = kind == 0 ? me->second.cvfx : kind == 1 ? me->second.hvfx : me->second.avfx;
		std::pair<std::string, float> one(id.empty() ? std::string(kDefault[kind]) : id, kind == 2 ? e.area * 2.0f : 1.0f);
		if (std::find(shown.begin(), shown.end(), one) != shown.end())
			continue;
		shown.push_back(one);
		// a continuous effect's hit repeats for as long as the effect lasts (OpenMW's ContinuousVfx: a shield's bubble)
		bool loop = kind == 1 && (me->second.flags & 0x20) && e.duration > 1;
		spawnVfx(one.first, pos, loop ? (float)e.duration : 1.2f, follow, yaw, one.second, loop, e.effect);
		if (kind == 1)
			return;                                    // one hit visual a spell (the first effect's)
	}
}

void Session::castVfx(const SpellDef& sp, int caster)
{
	const float* feet = caster < 0 ? w.player.feet : w.refs[caster].pos;
	spellVfx(sp, 0, feet, caster, caster < 0 ? w.player.yaw : w.refs[caster].rot[2]);
}

// A bolt flies as its first target effect's bolt model, VFX_DefaultBolt for an effect without one (OpenMW's
// ProjectileManager::launchMagicBolt); looked up once, when it leaves
std::string Session::boltPiece(const SpellDef& sp, int* effect)
{
	for (auto& e : sp.effects)
	{
		if (e.range != 2)
			continue;
		auto me = w.game.magicEffects.find(e.effect);
		std::string id = me == w.game.magicEffects.end() || me->second.bvfx.empty() ? std::string("vfx_defaultbolt") : me->second.bvfx;
		auto pc = w.game.firstPerson.vfx.find(id);
		// (prototype particle pieces: the shed-sprite bolt, MW3DS_PFX / PFX: with pfx)
		if (const char* modes = pfxModeList())
			if (strstr(modes, "pfx"))
			{
				auto alt = w.game.firstPerson.vfx.find(lower(id) + "#pfx");
				if (alt != w.game.firstPerson.vfx.end())
					pc = alt;
			}
		if (pc != w.game.firstPerson.vfx.end())
		{
			if (effect)
				*effect = e.effect;
			return pc->second;
		}
	}
	return "";
}

// A spell visual's shape drawn in its magic effect's own particle texture when it takes one (OpenMW's
// overrideFirstRootTexture: the shapes sharing the model root's first texture), else its own
C3D_Tex* Session::fxTexture(const ActorMesh& m, C3D_Tex* own, int effect)
{
	if (!m.fx || effect < 0)
		return own;
	auto it = w.game.firstPerson.ptex.find(effect);
	C3D_Tex* t = it == w.game.firstPerson.ptex.end() ? nullptr : skyTex(it->second);
	return t ? t : own;
}

// A particle record's size at its age: grown in over the spray's grow time, shrunk over its fade (OpenMW's
// GrowFadeAffector)
static float spriteSize(const ActorMesh& m, float size, float age, float life)
{
	if (m.spriteGrow > 0.0f && age < m.spriteGrow)
		size *= age / m.spriteGrow;
	if (m.spriteFade > 0.0f && life - age < m.spriteFade)
		size *= (life - age) / m.spriteFade;
	return size;
}

// Its colour at that share of its life: the spray's colour keys, straight lines between (0xAABBGGRR)
static u32 spriteColour(const ActorMesh& m, float f)
{
	u32 c = m.spriteKeyC.empty() ? 0xFFFFFFFF : m.spriteKeyC[0];
	for (size_t k = 0; k + 1 < m.spriteKeyT.size(); k++)
		if (f >= m.spriteKeyT[k])
		{
			float t0 = m.spriteKeyT[k], t1 = m.spriteKeyT[k + 1];
			float w2 = t1 > t0 ? fminf(1.0f, (f - t0) / (t1 - t0)) : 1.0f;
			u32 c0 = m.spriteKeyC[k], c1 = m.spriteKeyC[k + 1];
			c = 0;
			for (int ch = 0; ch < 4; ch++)
			{
				float x0 = (c0 >> (8 * ch)) & 255, x1 = (c1 >> (8 * ch)) & 255;
				c |= (u32)(x0 + (x1 - x0) * w2) << (8 * ch);
			}
		}
	return c;
}

// A thinned visual's particles drawn a little bigger to fill in, only once it is far enough to be small on screen
// (from 600 units, fully by 1500) and at most 40 % bigger: up close thinning alone looks right, bigger ones looked
// blown up
static float vfxThinGrow(float share, float dist)
{
	if (share >= 1.0f)
		return 1.0f;
	float far = fminf(1.0f, fmaxf(0.0f, (dist - 600.0f) / 900.0f));
	return 1.0f + fminf(0.4f, 1.0f / sqrtf(share) - 1.0f) * far;
}

// Placed objects drawn as their visual (prototype: particles.py world_entry): a steaming pool or a lava river's steam
// over and over, Dagoth Ur's death sparkles from when they are enabled, an activator (the Akulakhan chamber, its
// bridge) held at rest until a script plays one of its groups. Kept while the object shows and is near
void Session::updateWorldFx()
{
	const char* modes = pfxModeList();
	bool on = modes && strstr(modes, "pfx") && !w.game.firstPerson.worldFx.empty();
	std::unordered_set<int> have;
	for (size_t i = 0; i < vfx.size();)
	{
		Vfx& v = vfx[i];
		if (v.world && (!on || v.follow < 0 || v.follow >= (int)w.refs.size() || !w.refs[v.follow].visible()
			|| w.refs[v.follow].cell < 0 || !w.cells[w.placeOf(v.follow)].live || w.distanceToPlayer(v.follow) > kWorldFxRange * 1.1f))
		{
			vfx.erase(vfx.begin() + i);
			continue;
		}
		if (v.world)
			have.insert(v.follow);
		i++;
	}
	if (!on)
		return;
	w.forLoadedRefs([&](int ri) {
		const Ref& r = w.refs[ri];
		if (have.count(ri) || !r.visible())
			return;
		auto it = w.game.firstPerson.worldFx.find(r.idLower);
		if (it == w.game.firstPerson.worldFx.end() || w.distanceToPlayer(ri) > kWorldFxRange)
			return;
		const FirstPersonDef::WorldFxDef& d = it->second;
		Vfx v;
		v.piece = d.piece;
		memcpy(v.pos, r.pos, sizeof(v.pos));
		v.yaw = r.rot[2];
		v.scale = r.scale;
		v.follow = ri;
		v.what = r.idLower;
		v.world = true;
		v.life = 1e9f;
		v.loop = d.loop;
		// an activator with groups: at its idle (or its start) until a script plays one
		auto idle = d.groups.find("idle");
		v.age = d.groups.empty() ? 0.0f : idle != d.groups.end() ? idle->second.first : 0.0f;
		v.until = d.groups.empty() ? -1.0f : v.age;
		// (a group a script played before it came in range: as far into it as the time since)
		auto pg = objectGroups.find(ri);
		auto g = pg != objectGroups.end() ? d.groups.find(pg->second.first) : d.groups.end();
		if (g != d.groups.end())
		{
			v.age = fminf(g->second.second, g->second.first + (float)(w.time - pg->second.second));
			v.until = g->second.second;
		}
		vm.piece(w, v.piece);
		vfx.push_back(v);
	});
}

// PlayGroup on an object with a world visual: its group from its start, held at its end (OpenMW: an activator's
// animation; the chamber's collapse, the bridge falling)
void Session::playObjectGroup(int ref, const std::string& group, bool loop)
{
	if (!loop)
		objectGroups[ref] = { group, (float)w.time };
	for (auto& v : vfx)
		if (v.world && v.follow == ref)
		{
			auto d = w.game.firstPerson.worldFx.find(v.what);
			if (d == w.game.firstPerson.worldFx.end())
				return;
			auto g = d->second.groups.find(group);
			if (g == d->second.groups.end())
			{
				v.age = 0.0f;                    // (no such group: its animation from the start, Dagoth Ur's sparkles)
				return;
			}
			v.age = g->second.first;
			v.until = loop ? -1.0f : g->second.second;
			logf("worldfx: %s plays %s (%.2f - %.2f)", v.what.c_str(), group.c_str(), g->second.first, g->second.second);
			return;
		}
}

// A creature's sprays (an atronach's fire, a ghost's death spray, the Heart's bursts) shed into the world from their
// emitter bones as its animation passes each particle's birth (OpenMW's world-space particles): the records are in the
// bone's frame, born at times on the model's own animation timeline; a group just started counts from its start
void Session::updateCreatureFx(float dt)
{
	const char* modes = pfxModeList();
	if (!modes || !strstr(modes, "pfx") || w.game.firstPerson.cpfx.empty())
	{
		creatureFx.clear();
		return;
	}
	std::unordered_set<int> seen;
	w.forLoadedActors([&](int ri) {
		const Ref& r = w.refs[ri];
		auto pc = w.game.firstPerson.cpfx.find(r.idLower);
		Actor* a = pc != w.game.firstPerson.cpfx.end() && r.visible() ? w.actorOf(ri) : nullptr;
		if (!a || w.distanceToPlayer(ri) > kVfxRange)
			return;
		seen.insert(ri);
		CreatureFx& st = creatureFx[ri];
		st.piece = pc->second;
		FpPiece* piece = vm.piece(w, st.piece);
		if (!piece)
			return;
		const Skeleton& sk = actorSkeleton(*w.actorsOf(ri), a->skeleton);
		if (st.bones.size() != piece->meshes.size())
		{
			st.bones.assign(piece->meshes.size(), -1);
			for (size_t k = 0; k < piece->meshes.size(); k++)
				for (size_t b = 0; b < sk.bones.size(); b++)
					if (strcasecmp(sk.bones[b].name, piece->meshes[k].boneName.c_str()) == 0)
						st.bones[k] = (int)b;
		}
		// (its old crossed quads on those bones, meshes.particle_shape: hidden)
		if (!st.hid)
		{
			for (auto& am : a->meshes)
				for (int b : st.bones)
					if (b >= 0 && am.bone == b && (am.flags & BATCH_BLEND) && am.numVerts == 12)
						am.vis = { 0.0f, 0.0f };
			st.hid = true;
		}
		float cur = a->time;
		if (st.prevTime < 0.0f)
			st.prevTime = cur;
		else if (st.prevGroup != a->group || cur < st.prevTime || cur - st.prevTime > 1.0f)
			st.prevTime = cur - 0.001f;
		for (size_t k = 0; k < piece->meshes.size(); k++)
		{
			const ActorMesh& m = piece->meshes[k];
			int b = st.bones[k];
			if (m.sprites.empty() || b < 0 || (size_t)(b + 1) * 12 > a->pose.size())
				continue;
			// the bone's frame in the world: the actor's placement after its pose
			const float* P = a->place;
			const float* Q = &a->pose[b * 12];
			float B[12];
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 4; j++)
				{
					float v = 0.0f;
					for (int q = 0; q < 3; q++)
						v += P[i * 4 + q] * Q[q * 4 + j];
					B[i * 4 + j] = v + (j == 3 ? P[i * 4 + 3] : 0.0f);
				}
			// A spray that goes on (its records one period of it) emits in real time while the animation is within its
			// emit start .. stop, as OpenMW's emitter does, held pose or not; a burst's records are born at their times
			// on the timeline
			float period = m.spriteRepeat[0], from = m.spriteRepeat[1], until = m.spriteRepeat[2];
			// (a burst while its animation time stands still, a held pose, inside its emit time: it goes on emitting in
			// real time, as OpenMW's emitter does, its records over again)
			if (period <= 0.0f && cur == st.prevTime && !m.sprites.empty())
			{
				float lo = 1e9f, hi = -1e9f;
				for (size_t i = 0; i < m.sprites.size(); i += 9)
					lo = fminf(lo, m.sprites[i]), hi = fmaxf(hi, m.sprites[i]);
				if (cur >= lo && cur <= hi + 0.05f)
					period = fmaxf(0.2f, hi - lo + 0.05f), from = lo, until = 1e9f;
			}
			if (st.clock.size() != piece->meshes.size())
				st.clock.assign(piece->meshes.size(), 0.0f);
			float c0 = st.clock[k], c1 = c0;
			if (period > 0.0f && cur >= from && cur < until)
				c1 = st.clock[k] = c0 + dt;
			for (size_t i = 0; i + 9 <= m.sprites.size(); i += 9)
			{
				const float* rc = &m.sprites[i];
				float age;
				if (period > 0.0f)
				{
					float off = rc[0] - from;                           // its moment in the period
					float n = floorf((c0 - off) / period) + 1.0f;
					float at = off + fmaxf(0.0f, n) * period;           // its next birth on the emitting clock
					if (at <= c0 || at > c1)
						continue;
					age = c1 - at;
				}
				else
				{
					if (rc[0] <= st.prevTime || rc[0] > cur)
						continue;
					age = cur - rc[0];
				}
				if (st.live.size() >= 600 * 10)
					continue;
				float lv[10];
				lv[0] = (float)k;
				for (int q = 0; q < 3; q++)
				{
					lv[1 + q] = B[q * 4] * rc[1] + B[q * 4 + 1] * rc[2] + B[q * 4 + 2] * rc[3] + B[q * 4 + 3];
					lv[4 + q] = B[q * 4] * rc[4] + B[q * 4 + 1] * rc[5] + B[q * 4 + 2] * rc[6];
				}
				for (int q = 0; q < 3; q++)
					lv[1 + q] += lv[4 + q] * age;
				lv[7] = age;
				lv[8] = rc[7];
				lv[9] = rc[8];
				st.live.insert(st.live.end(), lv, lv + 10);
			}
		}
		st.prevTime = cur;
		st.prevGroup = a->group;
		// the live ones fly on: their spray's pull (in the creature's frame), then gone at the end of their life
		size_t o = 0;
		for (size_t i = 0; i + 10 <= st.live.size(); i += 10)
		{
			float* lv = &st.live[i];
			const ActorMesh& m = piece->meshes[(size_t)lv[0]];
			for (int q = 0; q < 3; q++)
			{
				float acc = a->place[q * 4] * m.spriteAccel[0] + a->place[q * 4 + 1] * m.spriteAccel[1]
					+ a->place[q * 4 + 2] * m.spriteAccel[2];
				lv[4 + q] += acc * dt;
				lv[1 + q] += lv[4 + q] * dt;
			}
			lv[7] += dt;
			if (lv[7] < lv[8])
			{
				if (o != i)
					memmove(&st.live[o], lv, 10 * sizeof(float));
				o += 10;
			}
		}
		st.live.resize(o);
	});
	for (auto it = creatureFx.begin(); it != creatureFx.end();)
		if (!seen.count(it->first))
			it = creatureFx.erase(it);
		else
			++it;
}

void Session::drawCreatureFx(const float eye[3], bool secondEye)
{
	for (auto& kv : creatureFx)
	{
		CreatureFx& st = kv.second;
		FpPiece* piece = st.live.empty() ? nullptr : vm.piece(w, st.piece);
		if (!piece)
			continue;
		const Ref& r = w.refs[kv.first];
		float ex = r.pos[0] - eye[0], ey = r.pos[1] - eye[1], ez = r.pos[2] - eye[2];
		float dist = sqrtf(ex * ex + ey * ey + ez * ez);
		float weight = fminf(2.0f, fmaxf(0.3f, 400.0f / fmaxf(1.0f, dist)));
		float share = fminf(1.0f, rendererSpriteShare() * weight);
		u32 keepBelow = share >= 1.0f ? 0xFFFFFFFFu : (u32)(share * 4294967295.0f);
		float grow = vfxThinGrow(share, dist);
		int wanted = 0;
		for (size_t k = 0; k < piece->meshes.size(); k++)
		{
			const ActorMesh& m = piece->meshes[k];
			float pos[100 * 3], half[100];
			u32 col[100];
			int n = 0;
			for (size_t i = 0; i + 10 <= st.live.size(); i += 10)
			{
				const float* lv = &st.live[i];
				if ((size_t)lv[0] != k)
					continue;
				wanted++;
				if (n >= 100 || (u32)((i / 10 + 1) * 2654435761u) > keepBelow)
					continue;
				pos[n * 3] = lv[1];
				pos[n * 3 + 1] = lv[2];
				pos[n * 3 + 2] = lv[3];
				half[n] = spriteSize(m, lv[9], lv[7], lv[8]) * grow;
				col[n] = spriteColour(m, lv[7] / lv[8]);
				n++;
			}
			if (n)
				rendererDrawSprites(pos, half, n, piece->textures[m.tex], 0xFFFFFFFF, col, (m.flags & BATCH_ADDITIVE) != 0);
		}
		if (!secondEye)
			rendererSpritesWanted(wanted * weight);
	}
}

// Projectiles, then the player's arms over everything (after rendererDrawWorld); arms false: a view held elsewhere for
// a side-by-side shot, no arms (the body still, in third person)
void Session::drawExtras(const float eye[3], bool secondEye, bool arms)
{
	if (!secondEye)
	{
		rendererSpriteFrame();
		vfxSpritedLast = vfxSprited;
		vfxSprited = 0;
	}
	drawCreatureFx(eye, secondEye);
	for (auto& p : projectiles)
	{
		if (!p.spell.empty())
		{
			// the bolt's own visual (found when it left), else a glow of its school's colour
			FpPiece* bolt = p.bolt.empty() ? nullptr : vm.piece(w, p.bolt);
			if (!bolt)
			{
				rendererDrawGlow(p.pos, 9.0f, p.glow);
				continue;
			}
			float hb = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
			C3D_Mtx bm;
			Mtx_Identity(&bm);
			Mtx_Translate(&bm, p.pos[0], p.pos[1], p.pos[2], true);
			Mtx_RotateZ(&bm, -atan2f(p.vel[0], p.vel[1]), true);
			Mtx_RotateX(&bm, atan2f(p.vel[2], hb), true);
			float age = bolt->length > 0.0f ? fmodf(3.0f - p.life, bolt->length) : 0.0f;    // its animation over and over
			// Particle records (particles.py, a bolt's '#pfx' piece): its sprays shed where the bolt has been, each
			// particle left behind as it leaves (OpenMW's world-space bolt particles); its fixed quads aren't drawn
			bool shed = false;
			for (auto& m : bolt->meshes)
				shed |= !m.sprites.empty();
			for (auto& m : bolt->meshes)
			{
				if (!m.sprites.empty())
					continue;
				C3D_Mtx at;
				if (fpTrackMatrix(m, bm, age, at))
					rendererDrawMesh(m, &at, fxTexture(m, bolt->textures[m.tex], p.boltEffect));
			}
			if (shed)
			{
				float speed = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
				float d[3] = { p.vel[0] / speed, p.vel[1] / speed, p.vel[2] / speed };
				float flight = 3.0f - p.life;
				auto vl = w.game.firstPerson.vfxLength.find(p.bolt);
				float cycle = vl != w.game.firstPerson.vfxLength.end() && vl->second > 0.0f ? vl->second : 1.0f;
				float ex = p.pos[0] - eye[0], ey = p.pos[1] - eye[1], ez = p.pos[2] - eye[2];
				float dist = sqrtf(ex * ex + ey * ey + ez * ez);
				float weight = fminf(2.0f, fmaxf(0.3f, 400.0f / fmaxf(1.0f, dist)));
				float share = fminf(1.0f, rendererSpriteShare() * weight);
				u32 keepBelow = share >= 1.0f ? 0xFFFFFFFFu : (u32)(share * 4294967295.0f);
				float grow = vfxThinGrow(share, dist);
				int wanted = 0;
				for (auto& m : bolt->meshes)
				{
					if (m.sprites.empty())
						continue;
					float pos[100 * 3], half[100];
					u32 col[100];
					int n = 0;
					// each record a particle of one emission cycle, born again every cycle the bolt has flown
					for (size_t i = 0; i + 9 <= m.sprites.size(); i += 9)
					{
						const float* r = &m.sprites[i];
						if (r[0] < 0.0f || r[0] > flight)
							continue;
						for (int k = (int)((flight - r[0]) / cycle); k >= 0; k--)
						{
							float a = flight - r[0] - k * cycle;
							if (a >= r[7])
								break;
							if (a < 0.0f)
								continue;
							wanted++;
							if (n >= 100 || (u32)((i / 9 + 1 + k * 7919) * 2654435761u) > keepBelow)
								continue;
							float lp[3];
							for (int c = 0; c < 3; c++)
								lp[c] = r[1 + c] + r[4 + c] * a + 0.5f * m.spriteAccel[c] * a * a;
							// (where the bolt was when it left, its own frame then: a straight flight keeps the frame)
							for (int c = 0; c < 3; c++)
								pos[n * 3 + c] = bm.r[c].x * lp[0] + bm.r[c].y * lp[1] + bm.r[c].z * lp[2] + bm.r[c].w
									- d[c] * speed * a;
							half[n] = spriteSize(m, r[8], a, r[7]) * grow;
							col[n] = spriteColour(m, a / r[7]);
							n++;
						}
					}
					rendererDrawSprites(pos, half, n, fxTexture(m, bolt->textures[m.tex], p.boltEffect), 0xFFFFFFFF, col,
						(m.flags & BATCH_ADDITIVE) != 0);
				}
				if (!secondEye)
					rendererSpritesWanted(wanted * weight);
			}
			continue;
		}
		auto it = w.game.firstPerson.items.find(p.item);
		FpPiece* piece = it != w.game.firstPerson.items.end() ? vm.piece(w, it->second.model) : nullptr;
		if (!piece)
			continue;
		// The model's +Y points along the flight
		float h = sqrtf(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
		C3D_Mtx model;
		Mtx_Identity(&model);
		Mtx_Translate(&model, p.pos[0], p.pos[1], p.pos[2], true);
		Mtx_RotateZ(&model, -atan2f(p.vel[0], p.vel[1]), true);
		Mtx_RotateX(&model, atan2f(p.vel[2], h), true);
		u32 glow = w.game.enchantGlow(p.item);      // an enchanted arrow or dart shimmers in flight
		for (auto& m : piece->meshes)
			rendererDrawMeshGlow(m, &model, piece->textures[m.tex], glow);
	}
	// Spell visuals of the moment, facing as their actor faces (none past kVfxRange from the eye: many casters at once)
	for (auto& v : vfx)
	{
		float dx = v.pos[0] - eye[0], dy = v.pos[1] - eye[1];
		float range = v.world ? kWorldFxRange : kVfxRange;
		if (dx * dx + dy * dy > range * range)
			continue;
		FpPiece* piece = vm.piece(w, v.piece);
		if (!piece)
			continue;
		C3D_Mtx vm_;
		Mtx_Identity(&vm_);
		Mtx_Translate(&vm_, v.pos[0], v.pos[1], v.pos[2], true);
		Mtx_RotateZ(&vm_, -v.yaw, true);
		// (a piece with no animation of its own swells into place)
		float grow = v.scale * (piece->length > 0.0f ? 1.0f : 0.6f + 0.4f * fminf(1.0f, v.age / 0.25f));
		Mtx_Scale(&vm_, grow, grow, grow);
		float age = v.loop && piece->length > 0.0f ? fmodf(v.age, piece->length) : v.age;   // (a continuous one over and over)
		bool sprited = false;
		for (auto& m : piece->meshes)
		{
			C3D_Mtx at;
			if (!m.sprites.empty())
			{
				// particle records: each where its velocity and the spray's pull have it now, its size and colour
				// over its life (particles.py)
				float pos[100 * 3], half[100];
				u32 col[100];
				int n = 0;
				// (a continuous one replays its particles every cycle; the last cycle's still finish)
				auto vl = w.game.firstPerson.vfxLength.find(v.piece);
				float cycle = vl != w.game.firstPerson.vfxLength.end() ? vl->second : 0.0f;
				float now = v.loop && cycle > 0.0f ? fmodf(v.age, cycle) : v.age;
				// (the frame's budget spread evenly: every visual keeps the same share of its particles, the same ones
				// each frame, each drawn as much bigger as it stands for more)
				// (weighted by distance: a near one keeps more, a far one, small on screen, fewer)
				float ex = v.pos[0] - eye[0], ey = v.pos[1] - eye[1], ez = v.pos[2] - eye[2];
				float dist = sqrtf(ex * ex + ey * ey + ez * ez);
				float weight = fminf(2.0f, fmaxf(0.3f, 400.0f / fmaxf(1.0f, dist)));
				float share = fminf(1.0f, rendererSpriteShare() * weight);
				u32 keepBelow = share >= 1.0f ? 0xFFFFFFFFu : (u32)(share * 4294967295.0f);
				float grow = vfxThinGrow(share, dist);
				int wanted = 0;
				for (size_t i = 0; i + 9 <= m.sprites.size() * (v.loop && cycle > 0.0f ? 2 : 1); i += 9)
				{
					size_t ri = i % m.sprites.size();
					const float* r = &m.sprites[ri];
					float a = now - r[0] + (i >= m.sprites.size() ? cycle : 0.0f);
					if (a < 0.0f || a >= r[7] || (i >= m.sprites.size() && v.age < cycle))
						continue;
					// (born before 0: one the model was saved with, alive at the start only, not every cycle)
					if (r[0] < 0.0f && v.age >= cycle && v.loop)
						continue;
					wanted++;
					if (n >= 100 || (u32)((ri / 9 + 1) * 2654435761u) > keepBelow)
						continue;
					float lp[3];
					for (int k = 0; k < 3; k++)
						lp[k] = r[1 + k] + r[4 + k] * a + 0.5f * m.spriteAccel[k] * a * a;
					for (int k = 0; k < 3; k++)
						pos[n * 3 + k] = vm_.r[k].x * lp[0] + vm_.r[k].y * lp[1] + vm_.r[k].z * lp[2] + vm_.r[k].w;
					half[n] = spriteSize(m, r[8], a, r[7]) * v.scale * grow;
					col[n] = spriteColour(m, a / r[7]);
					n++;
				}
				if (!secondEye)
				{
					rendererSpritesWanted(wanted * weight);
					if (n > 0)
						sprited = true;
				}
				rendererDrawSprites(pos, half, n, fxTexture(m, piece->textures[m.tex], v.effect), 0xFFFFFFFF, col,
					(m.flags & BATCH_ADDITIVE) != 0);
				continue;
			}
			if (!fpTrackMatrix(m, vm_, age, at))
				continue;
			// its texture sliding and its fade, as their controllers have them at this point (looping with it)
			float trackT = v.loop && piece->length > 0.0f ? age : v.age;
			if (!m.uvTrack.empty())
			{
				size_t nT = m.uvTrack.size() / 2;
				size_t k = std::min(nT - 1, (size_t)(trackT * kVfxFps));
				if (v.loop)
					k = (size_t)(trackT * kVfxFps) % nT;
				rendererUvOverride(true, m.uvTrack[k * 2], -m.uvTrack[k * 2 + 1]);
			}
			if (!m.alphaTrack.empty())
			{
				size_t nT = m.alphaTrack.size();
				size_t k = v.loop ? (size_t)(trackT * kVfxFps) % nT : std::min(nT - 1, (size_t)(trackT * kVfxFps));
				rendererAlphaScale(m.alphaTrack[k]);
			}
			if (m.flipFrames)
			{
				int f = (int)(v.age * m.flipFps);
				f = v.loop ? f % m.flipFrames : std::min((int)m.flipFrames - 1, f);
				rendererUvOverride(true, (float)(f % m.flipCols) / m.flipCols, -(float)(f / m.flipCols) / m.flipRows);
			}
			if (!m.pathFrames.empty())
			{
				ActorMesh fm = m;
				fm.verts = m.pathFrames[std::min(m.pathFrames.size() - 1, (size_t)(v.age * m.pathFps))];
				rendererDrawMesh(fm, &at, fxTexture(m, piece->textures[m.tex], v.effect));
			}
			else
				rendererDrawMesh(m, &at, fxTexture(m, piece->textures[m.tex], v.effect));
			rendererUvOverride(false);
			if (!m.alphaTrack.empty())
				rendererAlphaScale(1.0f);
		}
		if (sprited && !secondEye)
			vfxSprited++;
	}
	// Dropped items, with their own mesh where they lie
	for (int i : w.spawned)
	{
		const Ref& r = w.refs[i];
		if (!r.dropped || !r.visible() || r.cell < 0 || !w.cells[w.placeOf(i)].live)
			continue;
		float dx = r.pos[0] - eye[0], dy = r.pos[1] - eye[1];
		if (dx * dx + dy * dy > 3000.0f * 3000.0f)
			continue;
		auto it = w.game.firstPerson.items.find(r.idLower);
		FpPiece* piece = it != w.game.firstPerson.items.end() ? vm.piece(w, it->second.model) : nullptr;
		if (!piece)
			continue;
		C3D_Mtx model;
		Mtx_Identity(&model);
		Mtx_Translate(&model, r.pos[0], r.pos[1], r.pos[2], true);
		Mtx_RotateZ(&model, -r.rot[2], true);
		u32 glow = w.game.enchantGlow(r.idLower);   // an enchanted one shimmers where it lies
		for (auto& m : piece->meshes)
			rendererDrawMeshGlow(m, &model, piece->textures[m.tex], glow);
	}
	if (!arms && !thirdPerson)
		return;
	if (thirdPerson || previewFace)
		body.draw(w, !secondEye);
	else
		vm.draw(eye, w.player.yaw, w.player.pitch, !secondEye);
}
