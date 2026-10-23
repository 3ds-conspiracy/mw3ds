// Applying magic effects to the player and actors, instant effects, summons and bound items
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/magichelpers.hpp"

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

const char* summonCreatureId(int effect)
{
	return summonCreature(effect);
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

// A lasting change to an attribute or skill: it never shows below 0, the part below 0 is owed back first
static void shiftStat(int& value, int& below, int by)
{
	int total = value + below + by;
	value = std::max(0, total);
	below = std::min(0, total);
}

// Maximum magicka made again from Intelligence: x (magicka per point + a tenth of the spells' Fortify Maximum
// Magicka), plus Fortify Magicka. When Intelligence or the multiplier changes (keepShare), current magicka stays the same
// share of the maximum; with no maximum before (Intelligence 0) the share counts as full, so a Drain Intelligence to 0
// ending fills the bar, as in the original game (UESP's Drain Attribute; the Morrowind Code Patch and OpenMW give the
// empty bar instead, we keep Morrowind's)
void refitMagicka(PlayerStats& s, bool keepShare)
{
	float max = s.attributes[ATTR_INTELLIGENCE] * (s.magickaMult + 0.1f * s.magickaFortify) + s.magickaFlat;
	if (keepShare)
		s.magicka = s.magickaMax > 0.0f ? s.magicka * max / s.magickaMax : max;
	s.magickaMax = max;
}

// Adds (sign 1) or removes (sign -1) a lasting effect's change to the player's numbers
void applyFortify(World& w, ActiveEffect& a, float sign)
{
	PlayerStats& s = w.stats;
	float m = a.magnitude * sign;
	switch (a.effect)
	{
	case EFF_FORTIFY_ATTRIBUTE:
		if (a.attribute >= 0 && a.attribute < 8) shiftStat(s.attributes[a.attribute], s.attrBelow[a.attribute], (int)lroundf(m));
		if (a.attribute == ATTR_INTELLIGENCE) refitMagicka(s, true);
		break;
	case EFF_FORTIFY_SKILL: if (a.skill >= 0 && a.skill < 27) shiftStat(s.skills[a.skill], s.skillBelow[a.skill], (int)lroundf(m)); break;
	case EFF_FORTIFY_HEALTH: s.healthMax += m; s.health += m; break;
	case EFF_FORTIFY_MAGICKA: s.magickaFlat += m; refitMagicka(s, false); s.magicka += m; break;
	case EFF_FORTIFY_FATIGUE: s.fatigueMax += m; s.fatigue += m; break;
	case EFF_DRAIN_ATTRIBUTE: case EFF_ABSORB_ATTRIBUTE:
		if (a.attribute >= 0 && a.attribute < 8) shiftStat(s.attributes[a.attribute], s.attrBelow[a.attribute], -(int)lroundf(m));
		if (a.attribute == ATTR_INTELLIGENCE) refitMagicka(s, true);
		break;
	case EFF_DRAIN_SKILL: case EFF_ABSORB_SKILL:
		if (a.skill >= 0 && a.skill < 27) shiftStat(s.skills[a.skill], s.skillBelow[a.skill], -(int)lroundf(m));
		break;
	case EFF_FORTIFY_MAX_MAGICKA:
		// (made again from Intelligence each time, so a Fortify Intelligence that began later can't leave the maximum
		// off when this ends)
		s.magickaFortify += m;
		refitMagicka(s, true);
		break;
	default: break;          // attack, shield, jump, levitate, swift swim are read while active
	}
	// (Levitate is read while active too: Player::levitate, with collision. It used to switch on the free fly mode,
	// which went through walls at a fixed speed.)
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
		// (kCasterObject: an object's script cast it, a shrine's blessing: from someone else all the same, as in OpenMW)
		bool fromOther = casterRef >= 0 || casterRef == kCasterObject;
		if (fromOther && w.effectTotal(EFF_REFLECT) > 0.0f && rand() % 100 < (int)w.effectTotal(EFF_REFLECT))
		{
			notify("The spell was reflected.");
			if (casterRef >= 0 && w.active(casterRef))
				applyEffectToActor(casterRef, e, false);         // (a reflected spell isn't reflected again)
			return;
		}
		if (fromOther && w.effectTotal(EFF_SPELL_ABSORPTION) > 0.0f
			&& rand() % 100 < (int)w.effectTotal(EFF_SPELL_ABSORPTION))
		{
			notify("The spell was absorbed.");
			w.stats.magicka = fminf(w.stats.magickaMax, w.stats.magicka + spellCost);   // (OpenMW: no higher than the maximum)
			return;
		}
	}
	if (harmful(e.effect))
	{
		auto me = w.game.magicEffects.find(e.effect);
		bool resistable = false;
		float r = resistance(w, e.effect, me != w.game.magicEffects.end() && (me->second.flags & 0x8), castChance, resistable,
			resistDie);
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
		spawnVfx("vfx_summon_start", r.pos, 1.2f, a.ref, r.rot[2]);     // it arrives in a flash (OpenMW's summoning)
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
				const Object* io = it.equipped ? w.game.object(it) : nullptr;
				if (io && io->type == o->type && (o->recType == REC_WEAP || io->subtype == o->subtype))
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
		// The nearest Imperial shrine / Tribunal temple marker. From inside: the exterior spot of the nearest door
		// that leads out, counting interiors door by door (OpenMW's getClosestMarker); the last spot outdoors when
		// no way out is found
		const float* from = w.player.feet;
		if (w.cells[w.current].interior)
		{
			from = w.lastOutside;
			std::vector<int> seen, now = { w.current };
			bool found = false;
			while (!found && !now.empty())
			{
				std::vector<int> next;
				for (int c : now)
					seen.push_back(c);
				// the doors of cells not in memory are read from the data (OpenMW follows the cells' records)
				std::vector<int> idx;
				for (int c : now)
					if (w.ensureRefs(c))
						for (int i = w.cells[c].refBase; i < w.cells[c].refBase + w.cells[c].refCount; i++)
							idx.push_back(i);
				for (size_t k = 0; k < idx.size() && !found; k++)
				{
					const Ref& d = w.refs[idx[k]];
					if (d.recType != REC_DOOR || !d.hasDest || d.destUnconverted)
						continue;
					if (d.destHasGrid)
					{
						from = d.destPos;
						found = true;
					}
					else
					{
						int c = w.cellIndex(d.destCell);
						if (c >= 0 && std::find(seen.begin(), seen.end(), c) == seen.end() && std::find(next.begin(), next.end(), c) == next.end())
							next.push_back(c);
					}
				}
				now = next;
			}
		}
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
			(float)def.attributes[ATTR_WILLPOWER], (float)def.attributes[ATTR_LUCK], r.fatigue, r.fatigueMax, castChance, resistable, resistDie);
		if (resistable)
		{
			mag *= fmaxf(0.0f, 1.0f - res / 100.0f);
			if (mag <= 0.0f)
			{
				logf("magic: %s resists %d", r.id.c_str(), e.effect);
				if (!r.dead && byPlayer && !friendlyHit(ri))
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
			damageNpc(ri, mag, false, true);         // (the difficulty slider scales blows, not spells)
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
		damageNpc(ri, mag, false, true);
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
	// Cure Common / Blight Disease takes every disease of that type off them (OpenMW's removeSpells on the actor's
	// spell list): the record's own go on noSpells so GetCommonDisease / GetBlightDisease read 0
	case EFF_CURE_COMMON_DISEASE: case EFF_CURE_BLIGHT_DISEASE:
	{
		int type = e.effect == EFF_CURE_COMMON_DISEASE ? 3 : 2;
		std::vector<std::string> ids = r.spells;
		for (const std::vector<Atom>* list : { &def.spells, &def.diseases })
			for (auto& id : *list)
				ids.push_back(lower(id));
		for (auto& id : ids)
		{
			auto sp = w.game.spells.find(id);
			if (sp == w.game.spells.end() || sp->second.type != type)
				continue;
			r.spells.erase(std::remove(r.spells.begin(), r.spells.end(), id), r.spells.end());
			if (std::find(r.noSpells.begin(), r.noSpells.end(), id) == r.noSpells.end())
				r.noSpells.push_back(id);
		}
		return;
	}
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
	// (the player's spell only: one cast by someone else turned whoever it struck on the player; on one siding with the
	// player it is a friendly hit, as a blow is)
	if (harmful(e.effect) && !r.dead && byPlayer && !friendlyHit(ri))
	{
		bool peaceful = r.ai != AI_COMBAT;
		makeHostile(ri);
		if (peaceful && !def.creature)
			crimeSeen(CRIME_ASSAULT, 0, ri), reportCrime(ri, kBountyAssault);
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
	{
		// the summoned creature goes back, in a flash where it stood (OpenMW's VFX_Summon_End)
		if (a.ref < (int)w.refs.size() && w.refs[a.ref].visible())
			spawnVfx("vfx_summon_end", w.refs[a.ref].pos, 1.2f, -2, w.refs[a.ref].rot[2]);
		w.despawn(a.ref);
	}
	if (!a.item.empty())
	{
		// The bound item vanishes; what it replaced comes back
		w.removeItem(a.item, w.itemCount(a.item));
		for (auto& it : w.inventory)
			if (!a.prev.empty() && it.id == a.prev)
				it.equipped = true;
	}
}
