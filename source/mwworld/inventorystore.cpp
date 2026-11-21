// What the player wears and holds: weapon, ammunition, armor rating, wear
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

static const float kArmorSlotWeight[11] = { 0.1f, 0.3f, 0.1f, 0.1f, 0.1f, 0.1f, 0.05f, 0.05f, 0.1f, 0.05f, 0.05f };

// Light, Medium or Heavy Armor: the piece's weight against its slot's base weight (fLightMaxMod, fMedMaxMod)
int Session::armorSkill(const Object* o)
{
	float wt = o->subtype >= 0 && o->subtype < 11 ? kArmorSlotBaseWeight[o->subtype] : 0.0f;
	return o->weight <= wt * w.game.gmstf("flightmaxmod", 0.6f) + 0.0005f ? SKILL_LIGHT_ARMOR
		: o->weight <= wt * w.game.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? SKILL_MEDIUM_ARMOR : SKILL_HEAVY_ARMOR;
}

// ---- Equipment

std::string actorWeaponId(const World& w, int ri)
{
	return w.refs[ri].actor >= 0 ? w.refs[ri].weapon.str() : std::string();
}

std::string actorShieldId(const World& w, int ri)
{
	return w.refs[ri].actor >= 0 ? w.refs[ri].shield.str() : std::string();
}

const std::vector<Atom>& actorLookWorn(const World& w, int ri)
{
	static const std::vector<Atom> kNone;
	const Ref& r = w.refs[ri];
	if (r.actor < 0)
		return kNone;
	const ActorDef& d = w.game.actors[r.actor];
	const ActorLook* l = d.look(r.look);
	return l ? l->worn : d.worn;
}

// The skill a weapon of that WEAP type trains, -1 for arrows and bolts (as npcstats.py WEAPON_SKILL)
static int gearSkill(int type)
{
	switch (type)
	{
	case 0: return SKILL_SHORT_BLADE;
	case 1: case 2: return SKILL_LONG_BLADE;
	case 3: case 4: case 5: return SKILL_BLUNT;
	case 6: return SKILL_SPEAR;
	case 7: case 8: return SKILL_AXE;
	case WEAP_BOW: case WEAP_CROSSBOW: case WEAP_THROWN: return SKILL_MARKSMAN;
	default: return -1;
	}
}

// OpenMW's InventoryStore::autoEquip, as the converter picks it for a library (npcstats.py combat_weapon,
// preferred_shield / best_shield, armor_rating):
// - the weapon: the weapon skills from the highest down (ties in autoEquipWeapon's order: long blade, axe, spear, short
//   blade, marksman, blunt; a skill at 0 never), and for each the carried weapon of that skill with the highest maximum
//   of chop, slash and thrust (the later of equals); a bow or crossbow only with arrows / bolts (the one of the highest
//   chop maximum, the later of equals), else the next skill. A creature only with the Weapon and Shield flag, and
//   without one carried, the weapon its script gives it
// - the shield: a person's of the best rating for their skill in its weight class (fUnarmoredBase1 / 2 the bar it must
//   pass), the first of equals; a creature's of the best rating
// - a person's armour rating: each slot's best carried piece for their skill, else Unarmored, by slot weight
void actorPickGear(const GameData& g, Ref& ref)
{
	const ActorDef& d = g.actors[ref.actor];
	if (!d.weaponShield)
		return;
	std::vector<const Object*> weapons, armor;
	for (auto& c : ref.contents)
		if (c.first != 0)
			if (const Object* o = g.object(c.second))
			{
				if (o->recType == REC_WEAP)
					weapons.push_back(o);
				else if (o->recType == REC_ARMO)
					armor.push_back(o);
			}
	auto bestOf = [&](int type) {
		const Object* best = nullptr;
		int top = 0;
		for (const Object* w : weapons)
			if (w->subtype == type && w->chop[1] >= top)
			{
				best = w;
				top = w->chop[1];
			}
		return best;
	};
	const Object* arrow = bestOf(WEAP_ARROW);
	const Object* bolt = bestOf(WEAP_BOLT);
	static const int kOrder[6] = { SKILL_LONG_BLADE, SKILL_AXE, SKILL_SPEAR, SKILL_SHORT_BLADE, SKILL_MARKSMAN, SKILL_BLUNT };
	bool visited[6] = {};
	for (int round = 0; round < 6 && ref.weapon.empty(); round++)
	{
		int pick = -1, top = 0;
		for (int k = 0; k < 6; k++)
			if (!visited[k] && d.skills[kOrder[k]] > top)
			{
				pick = k;
				top = d.skills[kOrder[k]];
			}
		if (pick < 0)
			break;
		visited[pick] = true;
		const Object* weapon = nullptr;
		int most = 0;
		for (const Object* w : weapons)
		{
			if (gearSkill(w->subtype) != kOrder[pick])
				continue;
			for (int hi : { (int)w->chop[1], (int)w->slash[1], (int)w->thrust[1] })
				if (hi >= most)
				{
					weapon = w;
					most = hi;
				}
		}
		if (!weapon)
			continue;
		const Object* ammo = weapon->subtype == WEAP_BOW ? arrow : weapon->subtype == WEAP_CROSSBOW ? bolt : nullptr;
		if ((weapon->subtype == WEAP_BOW || weapon->subtype == WEAP_CROSSBOW) && !ammo)
			continue;
		ref.weapon = lower(weapon->id.str());
		if (ammo)
			ref.ammoId = lower(ammo->id.str());
	}
	if (ref.weapon.empty() && d.creature)
		ref.weapon = d.scriptWeapon;
	const float unarmored = (g.gmstf("funarmoredbase1", 0.1f) * d.skills[SKILL_UNARMORED])
		* (g.gmstf("funarmoredbase2", 0.065f) * d.skills[SKILL_UNARMORED]);
	auto rating = [&](const Object* o) {
		float wt = o->subtype >= 0 && o->subtype < 11 ? kArmorSlotBaseWeight[o->subtype] : 0.0f;
		if (o->weight == 0.0f || wt == 0.0f)
			return (float)o->armor;
		int skill = o->weight <= wt * g.gmstf("flightmaxmod", 0.6f) + 0.0005f ? SKILL_LIGHT_ARMOR
			: o->weight <= wt * g.gmstf("fmedmaxmod", 0.9f) + 0.0005f ? SKILL_MEDIUM_ARMOR : SKILL_HEAVY_ARMOR;
		return o->armor * (float)d.skills[skill] / g.gmstf("ibasearmorskill", 30.0f);
	};
	const Object* shield = nullptr;
	float best = d.creature ? -1.0f : unarmored;
	for (const Object* o : armor)
		if (o->subtype == 8)
		{
			float r = d.creature ? (float)o->armor : rating(o);
			if (r > best)
			{
				shield = o;
				best = r;
			}
		}
	if (shield)
		ref.shield = lower(shield->id.str());
	if (d.creature)
		return;
	static const int kSlot[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 7 };
	float slots[9];
	bool worn[9] = {};
	for (int k = 0; k < 9; k++)
		slots[k] = unarmored;
	for (const Object* o : armor)
	{
		if (o->subtype < 0 || o->subtype >= 11)
			continue;
		int s = kSlot[o->subtype];
		float r = rating(o);
		if (!worn[s] || r > slots[s])
			slots[s] = r;
		worn[s] = true;
	}
	float total = 0.0f;
	for (int k = 0; k < 9; k++)
		total += kArmorSlotWeight[k] * slots[k];
	ref.armor = roundf(total * 10.0f) / 10.0f;
}

// OnPCEquip back to 0 on a carried item that isn't worn or held (OpenMW's InventoryStore::unequipSlot unsets it as the
// player's slot empties, however it empties: taken off, swapped, sold, dropped). It stayed 1 here: Wraithguard's script
// holds the global wraithguardEquipped at 1 while its OnPCEquip is, so once it had been worn Keening and Sunder never
// wounded again, and Keening put away went on wounding. Only things that are worn or held, and not an item whose script
// sets PCSkipEquip (it is used rather than worn, its script clears OnPCEquip itself). Every frame, before the scripts:
// a saved game holding a stale 1 comes right too
void World::unsetUnwornEquip()
{
	for (auto& s : scripts)
	{
		if (s.item.empty())
			continue;
		float* on = s.local("onpcequip");
		if (!on || *on == 0.0f)
			continue;
		float* skip = s.local("pcskipequip");
		if (skip && *skip != 0.0f)
			continue;
		const Object* o = game.object(s.item);
		if (!o || (o->recType != REC_WEAP && o->recType != REC_ARMO && o->recType != REC_CLOT && o->recType != REC_LIGH
			&& o->recType != REC_LOCK && o->recType != REC_PROB))
			continue;
		bool worn = false;
		for (auto& it : inventory)
			if (it.equipped && it.id == s.item)
			{
				worn = true;
				break;
			}
		if (!worn)
			*on = 0.0f;
	}
}

InventoryItem* Session::playerWeaponItem()
{
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it))
				if (o->recType == REC_WEAP && o->subtype < WEAP_ARROW)
					return &it;
	return nullptr;
}

const Object* Session::playerWeapon()
{
	InventoryItem* it = playerWeaponItem();
	return it ? w.game.object(*it) : nullptr;
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
			if (const Object* o = w.game.object(it))
				if (o->recType == REC_WEAP && o->subtype == want)
					return o;
	return nullptr;
}

int Session::itemCondition(const InventoryItem& it)
{
	const Object* o = w.game.object(it);
	int max = o ? o->health : 0;
	return it.condition < 0 ? max : it.condition;
}

// Wears an equipped weapon / armor piece down; one of a stack splits off first
void Session::wearItem(InventoryItem* it, float amount)
{
	const Object* o = it ? w.game.object(*it) : nullptr;
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
		notify(o->name + (o->recType == REC_WEAP ? " is broken." : " is broken and gives no protection."));
		logf("combat: %s broke", o->id.c_str());
		if (o->recType == REC_WEAP)
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
			if (const Object* o = w.game.object(it))
				if (o->recType == REC_ARMO && o->subtype >= 0 && o->subtype < 11)
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
