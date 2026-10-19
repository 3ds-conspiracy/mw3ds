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
