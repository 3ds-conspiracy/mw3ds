// Recharging enchanted items with soul gems
#include <cmath>
#include <functional>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"

// The player's enchanted items that cast when used or when they strike win back fMagicItemRechargePerSecond of charge
// each second (real time; skipped time counts as its real seconds), up to full (OpenMW's rechargeItems)
void Session::rechargeItems(float seconds)
{
	if (seconds <= 0.0f)
		return;
	const float rate = w.game.gmstf("fmagicitemrechargepersecond", 0.05f);
	for (auto& it : w.inventory)
	{
		if (it.charge < 0.0f)
			continue;                        // (full: never used)
		const SpellDef* en = w.enchantmentOf(it);
		if (!en || (en->type != ENCH_USE && en->type != ENCH_STRIKE))
			continue;
		it.charge = fminf((float)en->charge, it.charge + rate * seconds);
	}
}

// OpenMW's recharge roll: (Enchant + Intelligence / 5 + Luck / 10) x fatigue; the soul goes either way; a win
// restores the soul's size x roll / chance
bool Session::rechargeItem(int item, int gem)
{
	InventoryItem& it = w.inventory[item];
	const SpellDef* en = w.enchantmentOf(it);
	if (!en)
		return false;
	float x = rechargeChance();
	int soul = w.soulValue(w.inventory[gem]);
	int roll = rand() % 100;
	bool ok = (float)roll < x;
	float charge = w.chargeOf(it);
	if (ok)
		it.charge = fminf((float)en->charge, charge + rechargeGain(soul, roll, x));      // the better the roll, the more
	std::string gemId = w.inventory[gem].id;
	if (--w.inventory[gem].count <= 0)
		w.inventory.erase(w.inventory.begin() + gem);
	w.itemLeft(gemId);
	if (gemId == "misc_soulgem_azura")
		w.addItem(gemId, 1);
	logf("recharge: %s with soul %d, chance %.0f: %s", en->id.c_str(), soul, x, ok ? "done" : "failed");
	if (ok)
	{
		useSkill(9, 0);
		playSound(-1, "enchant success");
		notify("The item has been recharged.");
	}
	else
	{
		playSound(-1, "enchant fail");
		notify("The recharge failed.");
	}
	return ok;
}
