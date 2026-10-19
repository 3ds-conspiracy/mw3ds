// The enchanting screen: at an enchanter, or by the player's own hand
#include <cmath>
#include <functional>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/enchantinghelpers.hpp"

void Session::openEnchanting(int enchanter)
{
	enchanterRef = enchanter;
	makeEffects.clear();
	makeSel = -1;
	makeName.clear();
	makeNameItem.clear();
	enchantItem = enchantGem = -1;
	enchantType = -1;
	list2 = UiList();
	std::vector<int> items, gems;
	enchantChoices(items, gems);
	if (!items.empty())
		enchantItem = items[0];
	if (!gems.empty())
		enchantGem = gems[0];
	screen = SCR_ENCHANT;
}

// The new item's name starts as the chosen item's own name, again whenever another item is chosen (OpenMW's
// EnchantingDialog::setItem); a name typed in is kept while the item stays
void Session::syncEnchantName()
{
	const std::string id = enchantItem >= 0 && enchantItem < (int)w.inventory.size() ? w.inventory[enchantItem].id : "";
	if (id == makeNameItem)
		return;
	const Object* o = id.empty() ? nullptr : w.game.object(id);
	makeName = o ? o->name : "";
	makeNameItem = id;
}

void Session::drawEnchanting()
{
	int gold = w.itemCount("gold_001");
	header(w.game.gmst("senchanting", "Enchanting"), "Gold " + std::to_string(gold));
	std::vector<int> items, gems;
	enchantChoices(items, gems);
	auto indexIn = [](const std::vector<int>& v, int x) {
		for (size_t k = 0; k < v.size(); k++)
			if (v[k] == x)
				return (int)k;
		return -1;
	};
	if (indexIn(items, enchantItem) < 0)
		enchantItem = items.empty() ? -1 : items[0];
	if (indexIn(gems, enchantGem) < 0)
		enchantGem = gems.empty() ? -1 : gems[0];
	const Object* item = enchantItem >= 0 ? w.game.object(w.inventory[enchantItem].id) : nullptr;
	int soul = enchantGem >= 0 ? w.soulValue(w.inventory[enchantGem]) : 0;
	int types = castTypesFor(item);
	if (enchantType < 0 || !(types & (1 << enchantType)))
		for (int t = 0; t < 4; t++)
			if (types & (1 << t))
			{
				enchantType = t;
				break;
			}
	// Item / soul gem / cast type: tap to go to the next one
	if (uiButton(4, 24, 154, 18, item ? item->name : "No item to enchant") && !items.empty())
		enchantItem = items[(indexIn(items, enchantItem) + 1) % items.size()];
	std::string gemLabel = "No soul gem";
	if (enchantGem >= 0)
	{
		gemLabel = w.game.gmstLabel("ssoulgem", "Soul") + " " + std::to_string(soul);
		for (auto& a : w.game.actors)
			if (lower(a.id) == w.inventory[enchantGem].soul)
				gemLabel = a.name + " (" + std::to_string(soul) + ")";
	}
	if (uiButton(162, 24, 154, 18, gemLabel) && !gems.empty())
		enchantGem = gems[(indexIn(gems, enchantGem) + 1) % gems.size()];
	static const char* kCastKeys[4] = { "sitemcastonce", "sitemcastwhenstrikes", "sitemcastwhenused", "sitemcastconstant" };
	if (uiButton(4, 46, 154, 18, enchantType >= 0 ? w.game.gmstLabel(kCastKeys[enchantType], kCastNames[enchantType]) : "-") && types)
	{
		for (int k = 1; k <= 4; k++)
			if (types & (1 << ((enchantType + k) % 4)))
			{
				enchantType = (enchantType + k) % 4;
				break;
			}
		// Constant effects are on the wearer; strikes land on who's hit
		for (auto& e : makeEffects)
			e.range = enchantType == 3 ? 0 : enchantType == 1 && e.range == 0 ? 1 : e.range;
	}
	// Ranges this cast type allows: constant self only, on strike touch / target
	int ranges = enchantType == 3 ? 1 : enchantType == 1 ? 6 : 7;
	std::vector<SpellEffect> known = knownEffects(), usable;
	std::vector<std::string> rows;
	for (auto& e : known)
	{
		SpellEffect n = newEffect(e, ranges);
		int f = effectFlags(e.effect);
		bool ok = (n.range == 0 && (f & 0x40) && (ranges & 1)) || (n.range == 1 && (ranges & 2)) || (n.range == 2 && (ranges & 4));
		if (!ok)
			continue;
		usable.push_back(n);
		rows.push_back(effectLabel(e.effect, e.skill, e.attribute));
	}
	int pick = uiList(list2, 4, 68, 130, 136, rows, 0.4f, true);
	if (pick >= 0 && pick < (int)usable.size() && makeEffects.size() < 4)
	{
		makeEffects.push_back(usable[pick]);
		makeSel = makeEffects.size() - 1;
		playSound(-1, "Menu Click");
	}
	effectEditor(makeEffects, makeSel, ranges, 68, 4);
	syncEnchantName();
	EnchantCalc c = enchantCalc();
	char info[96];
	if (enchanterRef >= 0)
		snprintf(info, sizeof(info), "Points %.0f/%.0f  Price %d", c.points, c.capacity, c.price);
	else
		snprintf(info, sizeof(info), "Points %.0f/%.0f  Chance %d%%", c.points, c.capacity, std::max(0, (int)c.chance));
	uiText(162, 50, 0.38f, col::textDim, info);
	int b = makeButtons(w.game.gmst("senchanting", "Enchant"));
	if (b == 0)
	{
		playSound(-1, "Menu Click");
		wantText = TEXT_MAKENAME;
	}
	else if (b == 1)
		enchantConfirm();
	else if (b == 2 || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		makeEffects.clear();
		makeSel = -1;
		makeName.clear();
		makeNameItem.clear();
		screen = enchanterRef >= 0 ? SCR_DIALOGUE : SCR_INVENTORY;
	}
}
