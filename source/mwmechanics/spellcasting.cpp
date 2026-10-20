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

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

static const char* kSchoolName[6] = { "alteration", "conjuration", "destruction", "illusion", "mysticism", "restoration" };
static const float kTargetRange = 3000.0f;

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
	named.name = o ? o->name.str() : sp.id;
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
		// The cast's visual is modelled around the caster's feet, its flames up the arms (OpenMW attaches it to the
		// actor's own node: playSpellCastingEffects); a self spell's hit wraps the body when it lands (releaseSpell)
		castVfx(sp, -1);
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
		if (testLegit)
			monitorOnce(("legitcastfail:" + sp.id).c_str(), "%s failed its cast roll (chance %d%%) in a LEGIT run", sp.id.c_str(), (int)castChance(sp));
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
	// Its self effects land now: their hit wraps the caster (OpenMW's inflict, playEffects)
	for (auto& e : sp.effects)
		if (e.range == 0)
		{
			spellVfx(sp, 1, w.player.feet, -1, w.player.yaw);
			break;
		}
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
	// A touch lands as a blow does (OpenMW's getHitContact with fCombatDistance): the nearest within fCombatDistance of
	// its edge (its collision box half length and the player's taken off), less than that above or below, in front
	// within the angle whose sine is fCombatAngleXY / 90. It was 200 from its middle: a guar's middle is 120 inside it
	auto touchTarget = [&]() {
		const float reach = w.game.gmstf("fcombatdistance", 128.0f);
		const float cone = cosf(asinf(fminf(1.0f, w.game.gmstf("fcombatanglexy", 60.0f) / 90.0f)));
		int best = -1;
		float bestD = 1e9f;
		float fx = sinf(w.player.yaw), fy = cosf(w.player.yaw);
		w.forLoadedActors([&](int i) {
			const Ref& r = w.refs[i];
			if (r.actor < 0 || r.dead || !r.visible())
				return;
			float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
			float d = sqrtf(dx * dx + dy * dy), edge = d - actorHalfLength(i) - 28.5f;
			if (edge < reach && edge < bestD && fabsf(r.pos[2] - w.player.feet[2]) < reach && (d < 1.0f || (dx * fx + dy * fy) / d > cone))
			{
				bestD = edge;
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
			if (o.recType != REC_DOOR && o.recType != REC_CONT)
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
				who = e.range == 1 ? touchTarget() : aim(kTargetRange, 0.97f);
			if (who >= 0)
			{
				applyEffectToActor(who, e, true, (float)sp.cost, (float)castChance(sp), sp.name);
				playSound(who, spellSound(sp, 2));
				spellVfx(sp, 1, w.refs[who].pos, who, w.refs[who].rot[2]);
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
	spellVfx(sp, 2, at, -2, 0.0f, range);
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
