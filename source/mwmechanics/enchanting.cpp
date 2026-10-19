// Enchanting: an item, a filled soul gem and effects the player knows make an enchanted item
// (Morrowind's rules as OpenMW has them). At an enchanter it always works and costs gold; by
// the player's own hand (using a filled soul gem) it's a roll on Enchant.
#include <cmath>
#include <functional>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"

const char* kCastNames[4] = { "Cast Once", "Cast When Strikes", "Cast When Used", "Constant Effect" };

// Which cast types an item takes: scrolls once; weapons on strike / when used / constant; armor and
// clothing when used / constant (bit k = type k)
int castTypesFor(const Object* o)
{
	if (!o)
		return 0;
	if (o->type == "BOOK")
		return 1;
	if (o->type == "WEAP")
		return 2 | 4 | 8;
	return 4 | 8;
}

// Items that can take an enchantment (capacity, not enchanted yet), and gems holding a soul
void Session::enchantChoices(std::vector<int>& items, std::vector<int>& gems)
{
	for (size_t i = 0; i < w.inventory.size(); i++)
	{
		const InventoryItem& it = w.inventory[i];
		const Object* o = w.game.object(it.id);
		if (!o)
			continue;
		if (o->enchant > 0 && o->ench.empty() && castTypesFor(o) && it.soul.empty())
			items.push_back(i);
		if (!it.soul.empty())
			gems.push_back(i);
	}
}

// Enchantment points (OpenMW's getEnchantPoints, which keeps vanilla's running cost): each effect's floored for
// the item's capacity and the cast cost, exact for the chance
float Session::enchantPoints(int castType, bool precise)
{
	float total = 0.0f, cost = 0.0f;
	for (auto& e : makeEffects)
	{
		auto it = w.game.magicEffects.find(e.effect);
		float base = it != w.game.magicEffects.end() ? it->second.cost : 1.0f;
		float duration = castType == 3 ? w.game.gmstf("fenchantmentconstantdurationmult", 100.0f) : (float)e.duration;
		cost += ((std::max(1, e.min) + std::max(1, e.max)) * duration + std::max(1, e.area)) * base * w.game.gmstf("feffectcostmult", 0.5f) * 0.05f;
		cost = fmaxf(1.0f, cost);
		if (e.range == 2)
			cost *= 1.5f;
		total += precise ? cost : floorf(cost);
	}
	return total;
}

// The chosen item, gem, cast type and effects: points (vanilla's running cost), the item's capacity, the
// player's chance (their own hand) and the enchanter's price
Session::EnchantCalc Session::enchantCalc()
{
	EnchantCalc c;
	const Object* item = enchantItem >= 0 && enchantItem < (int)w.inventory.size() ? w.game.object(w.inventory[enchantItem].id) : nullptr;
	c.soul = enchantGem >= 0 && enchantGem < (int)w.inventory.size() ? w.soulValue(w.inventory[enchantGem]) : 0;
	c.points = enchantPoints(enchantType);
	c.capacity = item ? item->enchant * w.game.gmstf("fenchantmentmult", 0.1f) : 0.0f;
	const PlayerStats& s = w.stats;
	// (Enchant - points x fEnchantmentChanceMult + Intelligence / 5 + Luck / 10) x the fatigue term, and
	// constant effect x fEnchantmentConstantChanceMult (OpenMW's getEnchantChance)
	c.chance = (s.skills[9] - enchantPoints(enchantType, true) * w.game.gmstf("fenchantmentchancemult", 3.0f)
		+ 0.2f * s.attributes[ATTR_INTELLIGENCE] + 0.1f * s.attributes[ATTR_LUCK])
		* (1.25f - 0.5f * (1.0f - s.fatigue / fmaxf(1.0f, s.fatigueMax)));
	if (enchantType == 3)
		c.chance *= w.game.gmstf("fenchantmentconstantchancemult", 0.5f);
	// OpenMW's getEnchantPrice: the last running cost x fEnchantmentValueMult at the enchanter's barter price, x the
	// number of items (1 here), at least 1
	float lastCost = 0.0f;
	for (auto& e : makeEffects)
	{
		auto it = w.game.magicEffects.find(e.effect);
		float base = it != w.game.magicEffects.end() ? it->second.cost : 1.0f;
		float duration = enchantType == 3 ? w.game.gmstf("fenchantmentconstantdurationmult", 100.0f) : (float)e.duration;
		lastCost += ((std::max(1, e.min) + std::max(1, e.max)) * duration + std::max(1, e.area)) * base
			* w.game.gmstf("feffectcostmult", 0.5f) * 0.05f;
		lastCost = fmaxf(1.0f, lastCost);
		if (e.range == 2)
			lastCost *= 1.5f;
	}
	int base = (int)(lastCost * w.game.gmstf("fenchantmentvaluemult", 1000.0f));
	c.price = enchanterRef >= 0 && w.refs[enchanterRef].actor >= 0 ? std::max(1, barterPrice(enchanterRef, base, true)) : std::max(1, base);
	return c;
}

// The Enchant button: Morrowind's refusals, else the attempt (false: refused, nothing used up)
bool Session::enchantConfirm()
{
	const Object* item = enchantItem >= 0 && enchantItem < (int)w.inventory.size() ? w.game.object(w.inventory[enchantItem].id) : nullptr;
	syncEnchantName();
	EnchantCalc c = enchantCalc();
	std::string msg;
	if (!item || enchantGem < 0)
		msg = "Choose an item and a soul gem.";
	else if (makeEffects.empty())
		msg = w.game.gmst("senchantmentmenu11", "You must add at least one effect to an enchantment.");
	else if (makeName.empty())
		msg = w.game.gmst("snotifymessage10", "You have to name the spell before buying it.");   // (OpenMW's wording too)
	else if (c.points > c.capacity)
		msg = "The item can't hold that much.";
	else if (enchantType == 3 && c.soul < w.game.gmstf("isoulamountforconstanteffect", 400))
		msg = "A constant effect needs a greater soul.";
	else if (enchanterRef >= 0 && c.price > w.itemCount("gold_001"))
		msg = w.game.gmst("snotifymessage18", "You don't have enough gold.");
	if (!msg.empty())
	{
		notify(msg);
		logf("enchanting: refused: %s", msg.c_str());
		return false;
	}
	makeEnchantment(c.points, c.soul, enchanterRef >= 0 ? 100.0f : c.chance, enchanterRef >= 0 ? c.price : 0);
	return true;
}

// Uses up the item and the gem's soul (Azura's Star keeps the gem); success makes the new item
void Session::makeEnchantment(float points, int soul, float chance, int price)
{
	InventoryItem base = w.inventory[enchantItem];
	const Object* bo = w.game.object(base.id);
	if (!bo)
		return;
	std::string gem = w.inventory[enchantGem].id;
	// the soul goes in any case
	if (--w.inventory[enchantGem].count <= 0)
		w.inventory.erase(w.inventory.begin() + enchantGem);
	if (gem == "misc_soulgem_azura")
		w.addItem(gem, 1);
	if (price > 0)
	{
		w.removeItem("gold_001", price);
		if (enchanterRef >= 0)
			w.refs[enchanterRef].gold += price;
	}
	bool ok = (persuadeRoll >= 0 ? persuadeRoll : rand() % 100) < (int)chance;
	logf("enchanting: %s, %zu effects, %.0f points, soul %d, chance %.0f: %s", bo->id.c_str(), makeEffects.size(), points,
		soul, chance, ok ? "made" : "failed");
	if (!ok)
	{
		w.removeItem(base.id, 1);            // the item is lost with the soul
		playSound(-1, "enchant fail");
		notify("Your enchantment failed.");
		enchantItem = enchantGem = -1;
		return;
	}
	w.removeItem(base.id, 1);
	SpellDef en;
	en.type = ENCH_ONCE + enchantType;
	en.cost = std::max(1, (int)points);
	en.charge = soul;
	en.effects = makeEffects;
	std::string key = bo->id + "_" + makeName + "_" + std::to_string(enchantType) + "_";
	for (auto& e : makeEffects)
		key += std::to_string(e.effect) + "_" + std::to_string(e.skill) + "_" + std::to_string(e.attribute) + "_"
			+ std::to_string(e.min) + "_" + std::to_string(e.max) + "_" + std::to_string(e.duration) + "_"
			+ std::to_string(e.range) + "_";
	unsigned h = std::hash<std::string>()(key) % 1000000;
	en.id = "mw3ds_ench_" + std::to_string(h);
	Object item = *bo;
	item.id = "mw3ds_item_" + std::to_string(h);
	item.iconBase = bo->iconBase.empty() ? Atom(bo->id) : bo->iconBase;
	item.ench = en.id;
	item.magic = true;
	item.value = bo->value + (int)(points * 10);
	item.name = makeName;
	item.script.clear();
	w.game.spells[en.id] = en;
	w.game.objects[item.id] = item;
	w.madeItems.push_back(item.id);
	w.addItem(item.id, 1);
	if (enchanterRef < 0)
		useSkill(9, 2);                    // Enchant: making a magic item (SKDT use 2)
	playSound(-1, "enchant success");
	notify(w.game.gmst("senchantmentmenu12", "You have successfully created an enchanted item."));
	makeEffects.clear();
	makeSel = -1;
	makeName.clear();
	makeNameItem.clear();
	enchantItem = enchantGem = -1;
}
